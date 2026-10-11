/*
	libmpv based playback engine for the generic PC backend

	Copyright 2026 Markus Volk <f_l_k@t-online.de>

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "config.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <unistd.h>
#include <sys/stat.h>

#include <mpv/client.h>
extern "C" {
/* MKTAG for the AVERROR_ macros: ffmpeg 4.4's error.h does not bring it */
#include <libavutil/common.h>
#include <libavutil/error.h>
}

#include "mpv_player.h"
#include "hal_debug.h"

#define hal_debug(args...) _hal_debug(HAL_DEBUG_PLAYER, this, args)
#define hal_info(args...) _hal_info(HAL_DEBUG_PLAYER, this, args)

#define NEUTRINO_MPV_CONF "/etc/neutrino/mpv.conf"

cMpvEngine *cMpvEngine::instance = NULL;

cMpvEngine *cMpvEngine::getInstance()
{
	if (!instance)
	{
		cMpvEngine *e = new cMpvEngine();
		if (!e->mpv)
		{
			delete e;
			return NULL;
		}
		instance = e;
	}
	return instance;
}

void cMpvEngine::shutdown()
{
	delete instance;
	instance = NULL;
}

cMpvEngine::cMpvEngine()
	: mpv(NULL), mQuit(false), mLoaded(false), mFailed(false), mAborted(false),
	  mEof(false), mIdle(true), mLastError(0)
{
	mVideo.valid = false;
	mVideo.w = mVideo.h = 0;
	mVideo.fps = 0;

	mpv_handle *h = mpv_create();
	if (!h)
	{
		hal_info("%s: mpv_create failed\n", __func__);
		return;
	}
	mpv_set_option_string(h, "vo", "libmpv");
	mpv_set_option_string(h, "idle", "yes");
	mpv_set_option_string(h, "keep-open", "yes");
	mpv_set_option_string(h, "input-default-bindings", "no");
	mpv_set_option_string(h, "input-vo-keyboard", "no");
	mpv_set_option_string(h, "osc", "no");
	mpv_set_option_string(h, "osd-level", "0");
	mpv_set_option_string(h, "load-scripts", "no");
	mpv_set_option_string(h, "terminal", "no");
	mpv_set_option_string(h, "ytdl", "no");
	mpv_set_option_string(h, "hwdec", "auto-safe");
	mpv_set_option_string(h, "demuxer-max-bytes", "64MiB");
	mpv_set_option_string(h, "demuxer-max-back-bytes", "16MiB");
	mpv_set_option_string(h, "audio-client-name", "neutrino");
	mpv_set_option_string(h, "sub-auto", "fuzzy");
	mpv_set_option_string(h, "volume-max", "100");
	if (access(NEUTRINO_MPV_CONF, R_OK) == 0)
	{
		int r = mpv_load_config_file(h, NEUTRINO_MPV_CONF);
		if (r < 0)
			hal_info("%s: %s: %s\n", __func__, NEUTRINO_MPV_CONF, mpv_error_string(r));
	}
	int r = mpv_initialize(h);
	if (r < 0)
	{
		hal_info("%s: mpv_initialize failed: %s\n", __func__, mpv_error_string(r));
		mpv_destroy(h);
		return;
	}
	mpv_request_log_messages(h, getenv("HAL_DEBUG_MPV") ? "v" : "warn");
	mpv_observe_property(h, 1, "video-params", MPV_FORMAT_NODE);
	mpv_observe_property(h, 2, "container-fps", MPV_FORMAT_DOUBLE);
	mpv_observe_property(h, 3, "eof-reached", MPV_FORMAT_FLAG);
	mpv_observe_property(h, 4, "idle-active", MPV_FORMAT_FLAG);
	mpv = h;
	hal_info("%s: libmpv %s\n", __func__, mpv_get_property_string(h, "mpv-version") ? : "");
	start();
}

cMpvEngine::~cMpvEngine()
{
	if (!mpv)
		return;
	mQuit = true;
	mpv_wakeup(mpv);
	join();
	/* the render context has to be gone by now (GLFramebuffer destructor) */
	mpv_terminate_destroy(mpv);
	mpv = NULL;
}

void cMpvEngine::run()
{
	while (!mQuit)
	{
		mpv_event *ev = mpv_wait_event(mpv, -1);
		if (ev->event_id == MPV_EVENT_NONE)
			continue;
		if (ev->event_id == MPV_EVENT_SHUTDOWN)
			break;
		handleEvent(ev);
	}
}

void cMpvEngine::handleEvent(mpv_event *ev)
{
	switch (ev->event_id)
	{
		case MPV_EVENT_START_FILE:
			mStateLock.lock();
			mFailed = false;
			mEof = false;
			mStateLock.unlock();
			break;
		case MPV_EVENT_FILE_LOADED:
			hal_debug("%s: file loaded\n", __func__);
			mStateLock.lock();
			mLoaded = true;
			mStateCond.broadcast();
			mStateLock.unlock();
			break;
		case MPV_EVENT_END_FILE:
			handleEndFile((mpv_event_end_file *)ev->data);
			break;
		case MPV_EVENT_LOG_MESSAGE:
			handleLog((mpv_event_log_message *)ev->data);
			break;
		case MPV_EVENT_PROPERTY_CHANGE:
		{
			mpv_event_property *p = (mpv_event_property *)ev->data;
			if (ev->reply_userdata == 1)
				updateVideoParams(p->format == MPV_FORMAT_NODE ? (mpv_node *)p->data : NULL);
			else if (ev->reply_userdata == 2)
			{
				mVideoLock.lock();
				mVideo.fps = (p->format == MPV_FORMAT_DOUBLE) ? *(double *)p->data : 0;
				mVideoLock.unlock();
			}
			else if (ev->reply_userdata == 3 && p->format == MPV_FORMAT_FLAG)
			{
				mStateLock.lock();
				mEof = *(int *)p->data;
				mStateLock.unlock();
			}
			else if (ev->reply_userdata == 4 && p->format == MPV_FORMAT_FLAG)
			{
				mStateLock.lock();
				mIdle = *(int *)p->data;
				mStateLock.unlock();
			}
			break;
		}
		default:
			break;
	}
}

void cMpvEngine::handleEndFile(mpv_event_end_file *ef)
{
	mStateLock.lock();
	mLoaded = false;
	switch (ef->reason)
	{
		case MPV_END_FILE_REASON_EOF:
			hal_debug("%s: eof\n", __func__);
			mEof = true;
			break;
		case MPV_END_FILE_REASON_ERROR:
			mFailed = true;
			mLastError = mAborted ? AVERROR_EXIT : mapError(ef->error);
			mLastErrorMsg = mpv_error_string(ef->error);
			if (!mLastLogLine.empty())
				mLastErrorMsg += ": " + mLastLogLine;
			hal_info("%s: error %d (%s)\n", __func__, mLastError, mLastErrorMsg.c_str());
			break;
		default:
			break;
	}
	mStateCond.broadcast();
	mStateLock.unlock();
}

void cMpvEngine::handleLog(mpv_event_log_message *msg)
{
	std::string text(msg->text ? msg->text : "");
	while (!text.empty() && (text[text.size() - 1] == '\n' || text[text.size() - 1] == '\r'))
		text.erase(text.size() - 1);
	if (msg->log_level <= MPV_LOG_LEVEL_WARN)
	{
		hal_info("mpv/%s: %s\n", msg->prefix, text.c_str());
		/* only ffmpeg's own message says why, and mpv forwards it as a warning; the
		 * stream and player layers just repeat that the open failed */
		if (!strncmp(msg->prefix, "ffmpeg", 6))
		{
			mStateLock.lock();
			mLastLogLine = text;
			mStateLock.unlock();
		}
	}
	else
		hal_debug("mpv/%s: %s\n", msg->prefix, text.c_str());
}

/* translate what mpv tells us into the AVERROR vocabulary that neutrino's
 * webtv restart logic (streaminput_classify_averror) understands */
int cMpvEngine::mapError(int error)
{
	const std::string &l = mLastLogLine;
	switch (error)
	{
		case MPV_ERROR_UNKNOWN_FORMAT:
		case MPV_ERROR_NOTHING_TO_PLAY:
			return AVERROR_INVALIDDATA;
		default:
			break;
	}
	/* ffmpeg says "Server returned 4xx ..." or "HTTP error 4xx ..." depending on the protocol handler */
	size_t h = l.find("Server returned ");
	if (h != std::string::npos)
		h += 16;
	else if ((h = l.find("HTTP error ")) != std::string::npos)
		h += 11;
	if (h != std::string::npos && h + 3 <= l.size())
	{
		int code = atoi(l.substr(h, 3).c_str());
		switch (code)
		{
			case 400: return AVERROR_HTTP_BAD_REQUEST;
			case 401: return AVERROR_HTTP_UNAUTHORIZED;
			case 403: return AVERROR_HTTP_FORBIDDEN;
			case 404: return AVERROR_HTTP_NOT_FOUND;
			default:
				if (code >= 400 && code < 500)
					return AVERROR_HTTP_OTHER_4XX;
				if (code >= 500 && code < 600)
					return AVERROR_HTTP_SERVER_ERROR;
		}
	}
	if (l.find("Connection reset") != std::string::npos)
		return AVERROR(ECONNRESET);
	if (l.find("Connection refused") != std::string::npos)
		return AVERROR(ECONNREFUSED);
	if (l.find("No route to host") != std::string::npos)
		return AVERROR(EHOSTUNREACH);
	if (l.find("Network is unreachable") != std::string::npos)
		return AVERROR(ENETUNREACH);
	if (l.find("timed out") != std::string::npos || l.find("Timeout") != std::string::npos)
		return AVERROR(ETIMEDOUT);
	if (l.find("Protocol not found") != std::string::npos)
		return AVERROR_PROTOCOL_NOT_FOUND;
	if (l.find("Invalid data found") != std::string::npos)
		return AVERROR_INVALIDDATA;
	if (l.find("End of file") != std::string::npos)
		return AVERROR_EOF;
	if (l.find("Input/output error") != std::string::npos)
		return AVERROR(EIO);
	return AVERROR_UNKNOWN;
}

static const mpv_node *nodeMapGet(const mpv_node *map, const char *key)
{
	if (!map || map->format != MPV_FORMAT_NODE_MAP)
		return NULL;
	for (int i = 0; i < map->u.list->num; i++)
		if (!strcmp(map->u.list->keys[i], key))
			return &map->u.list->values[i];
	return NULL;
}

static std::string nodeString(const mpv_node *n, const char *def = "")
{
	if (n && n->format == MPV_FORMAT_STRING && n->u.string)
		return n->u.string;
	return def;
}

static int64_t nodeInt(const mpv_node *n, int64_t def = 0)
{
	if (!n)
		return def;
	if (n->format == MPV_FORMAT_INT64)
		return n->u.int64;
	if (n->format == MPV_FORMAT_DOUBLE)
		return (int64_t)n->u.double_;
	return def;
}

static double nodeDouble(const mpv_node *n, double def = 0)
{
	if (!n)
		return def;
	if (n->format == MPV_FORMAT_DOUBLE)
		return n->u.double_;
	if (n->format == MPV_FORMAT_INT64)
		return (double)n->u.int64;
	return def;
}

static bool nodeFlag(const mpv_node *n, bool def = false)
{
	if (n && n->format == MPV_FORMAT_FLAG)
		return n->u.flag != 0;
	return def;
}

void cMpvEngine::updateVideoParams(mpv_node *node)
{
	VideoParams v;
	v.valid = false;
	v.w = v.h = 0;
	if (node && node->format == MPV_FORMAT_NODE_MAP)
	{
		v.w = nodeInt(nodeMapGet(node, "dw"));
		v.h = nodeInt(nodeMapGet(node, "dh"));
		v.valid = v.w > 0 && v.h > 0;
	}
	mVideoLock.lock();
	v.fps = mVideo.fps;
	if (v.valid != mVideo.valid || v.w != mVideo.w || v.h != mVideo.h)
		hal_info("%s: video %dx%d%s\n", __func__, v.w, v.h, v.valid ? "" : " (none)");
	mVideo = v;
	mVideoLock.unlock();
}

cMpvEngine::VideoParams cMpvEngine::getVideoParams()
{
	mVideoLock.lock();
	VideoParams v = mVideo;
	mVideoLock.unlock();
	return v;
}

static void setStringList(mpv_handle *mpv, const char *name, const std::vector<std::string> &list)
{
	std::vector<mpv_node> values(list.size());
	std::vector<char *> strs(list.size());
	for (size_t i = 0; i < list.size(); i++)
	{
		strs[i] = const_cast<char *>(list[i].c_str());
		values[i].format = MPV_FORMAT_STRING;
		values[i].u.string = strs[i];
	}
	mpv_node_list nl;
	nl.num = (int)list.size();
	nl.values = list.empty() ? NULL : &values[0];
	nl.keys = NULL;
	mpv_node node;
	node.format = MPV_FORMAT_NODE_ARRAY;
	node.u.list = &nl;
	mpv_set_property(mpv, name, MPV_FORMAT_NODE, &node);
}

bool cMpvEngine::load(const std::string &url, const std::vector<std::string> &headers,
		      const std::string &userAgent, const std::string &audioFile, double start)
{
	hal_info("%s: %s%s\n", __func__, url.c_str(), audioFile.empty() ? "" : " (+audio file)");
	mStateLock.lock();
	mLoaded = false;
	mFailed = false;
	mAborted = false;
	mEof = false;
	mLastError = 0;
	mLastErrorMsg.clear();
	mLastLogLine.clear();
	mStateLock.unlock();

	setFlag("pause", true);
	/* track selections are options in mpv and would carry over to the next file */
	setString("aid", "auto");
	setString("sid", "auto");
	setStringList(mpv, "http-header-fields", headers);
	setString("user-agent", userAgent.empty() ? "libmpv" : userAgent);
	std::vector<std::string> af;
	if (!audioFile.empty())
		af.push_back(audioFile);
	setStringList(mpv, "audio-files", af);
	if (start > 0)
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "%.3f", start);
		setString("start", buf);
	}
	else
		setString("start", "none");

	const char *cmd[] = { "loadfile", url.c_str(), "replace", NULL };
	if (!command(cmd))
		return false;

	mStateLock.lock();
	int waited = 0;
	while (!mLoaded && !mFailed && !mAborted && waited < 600)
	{
		mStateCond.wait(&mStateLock, 100);
		waited++;
	}
	bool ok = mLoaded && !mAborted;
	if (!mLoaded && !mFailed && !mAborted)
	{
		mLastError = AVERROR(ETIMEDOUT);
		mLastErrorMsg = "timeout opening stream";
	}
	mStateLock.unlock();
	if (!ok)
	{
		const char *scmd[] = { "stop", NULL };
		command(scmd);
	}
	hal_info("%s: %s\n", __func__, ok ? "ok" : "failed");
	return ok;
}

void cMpvEngine::stop()
{
	const char *cmd[] = { "stop", NULL };
	command(cmd);
}

void cMpvEngine::abort()
{
	hal_info("%s\n", __func__);
	mStateLock.lock();
	mAborted = true;
	mStateCond.broadcast();
	mStateLock.unlock();
	const char *cmd[] = { "stop", NULL };
	mpv_command_async(mpv, 0, cmd);
}

bool cMpvEngine::isLoaded()
{
	mStateLock.lock();
	bool r = mLoaded;
	mStateLock.unlock();
	return r;
}

bool cMpvEngine::isIdle()
{
	mStateLock.lock();
	bool r = mIdle;
	mStateLock.unlock();
	return r;
}

bool cMpvEngine::eofReached()
{
	mStateLock.lock();
	bool r = mEof;
	mStateLock.unlock();
	return r;
}

int cMpvEngine::getLastError(std::string &message)
{
	mStateLock.lock();
	int r = mLastError;
	message = mLastErrorMsg;
	mStateLock.unlock();
	return r;
}

bool cMpvEngine::setFlag(const char *name, bool v)
{
	int f = v;
	return mpv_set_property(mpv, name, MPV_FORMAT_FLAG, &f) >= 0;
}

bool cMpvEngine::setInt(const char *name, int64_t v)
{
	return mpv_set_property(mpv, name, MPV_FORMAT_INT64, &v) >= 0;
}

bool cMpvEngine::setDouble(const char *name, double v)
{
	return mpv_set_property(mpv, name, MPV_FORMAT_DOUBLE, &v) >= 0;
}

bool cMpvEngine::setString(const char *name, const std::string &v)
{
	return mpv_set_property_string(mpv, name, v.c_str()) >= 0;
}

bool cMpvEngine::getFlag(const char *name, bool &v)
{
	int f = 0;
	if (mpv_get_property(mpv, name, MPV_FORMAT_FLAG, &f) < 0)
		return false;
	v = f;
	return true;
}

bool cMpvEngine::getInt(const char *name, int64_t &v)
{
	return mpv_get_property(mpv, name, MPV_FORMAT_INT64, &v) >= 0;
}

bool cMpvEngine::getDouble(const char *name, double &v)
{
	return mpv_get_property(mpv, name, MPV_FORMAT_DOUBLE, &v) >= 0;
}

bool cMpvEngine::getString(const char *name, std::string &v)
{
	char *s = mpv_get_property_string(mpv, name);
	if (!s)
		return false;
	v = s;
	mpv_free(s);
	return true;
}

bool cMpvEngine::command(const char *const *args)
{
	int r = mpv_command(mpv, const_cast<const char **>(args));
	if (r < 0)
		hal_info("%s: %s: %s\n", __func__, args[0], mpv_error_string(r));
	return r >= 0;
}

bool cMpvEngine::getTracks(std::vector<Track> &tracks)
{
	tracks.clear();
	mpv_node node;
	if (mpv_get_property(mpv, "track-list", MPV_FORMAT_NODE, &node) < 0)
		return false;
	if (node.format == MPV_FORMAT_NODE_ARRAY)
	{
		for (int i = 0; i < node.u.list->num; i++)
		{
			const mpv_node *t = &node.u.list->values[i];
			Track tr;
			tr.id = nodeInt(nodeMapGet(t, "id"));
			tr.srcId = nodeInt(nodeMapGet(t, "src-id"), -1);
			tr.type = nodeString(nodeMapGet(t, "type"));
			tr.codec = nodeString(nodeMapGet(t, "codec"));
			tr.lang = nodeString(nodeMapGet(t, "lang"));
			tr.title = nodeString(nodeMapGet(t, "title"));
			tr.external = nodeFlag(nodeMapGet(t, "external"));
			tr.selected = nodeFlag(nodeMapGet(t, "selected"));
			tracks.push_back(tr);
		}
	}
	mpv_free_node_contents(&node);
	return true;
}

bool cMpvEngine::getChapters(std::vector<Chapter> &chapters)
{
	chapters.clear();
	mpv_node node;
	if (mpv_get_property(mpv, "chapter-list", MPV_FORMAT_NODE, &node) < 0)
		return false;
	if (node.format == MPV_FORMAT_NODE_ARRAY)
	{
		for (int i = 0; i < node.u.list->num; i++)
		{
			const mpv_node *c = &node.u.list->values[i];
			Chapter ch;
			ch.time = nodeDouble(nodeMapGet(c, "time"));
			ch.title = nodeString(nodeMapGet(c, "title"));
			chapters.push_back(ch);
		}
	}
	mpv_free_node_contents(&node);
	return true;
}

bool cMpvEngine::getEditions(std::vector<Edition> &editions)
{
	editions.clear();
	mpv_node node;
	if (mpv_get_property(mpv, "edition-list", MPV_FORMAT_NODE, &node) < 0)
		return false;
	if (node.format == MPV_FORMAT_NODE_ARRAY)
	{
		for (int i = 0; i < node.u.list->num; i++)
		{
			const mpv_node *e = &node.u.list->values[i];
			Edition ed;
			ed.id = nodeInt(nodeMapGet(e, "id"));
			ed.title = nodeString(nodeMapGet(e, "title"));
			ed.current = nodeFlag(nodeMapGet(e, "default"));
			editions.push_back(ed);
		}
	}
	mpv_free_node_contents(&node);
	return true;
}

bool cMpvEngine::getMetadata(std::map<std::string, std::string> &meta)
{
	meta.clear();
	mpv_node node;
	if (mpv_get_property(mpv, "metadata", MPV_FORMAT_NODE, &node) < 0)
		return false;
	if (node.format == MPV_FORMAT_NODE_MAP)
	{
		for (int i = 0; i < node.u.list->num; i++)
			meta[node.u.list->keys[i]] = nodeString(&node.u.list->values[i]);
	}
	mpv_free_node_contents(&node);
	return true;
}
