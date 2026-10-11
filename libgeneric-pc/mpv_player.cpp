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

#include <fcntl.h>
#include <poll.h>
#include <time.h>

#include <OpenThreads/ScopedLock>

#include <mpv/client.h>
#include <mpv/stream_cb.h>
extern "C" {
/* MKTAG for the AVERROR_ macros: ffmpeg 4.4's error.h does not bring it */
#include <libavutil/common.h>
#include <libavutil/error.h>
}

#include "mpv_player.h"
#include "dmx_hal.h"
#include "video_lib.h"
#include "audio_lib.h"
#include "hal_debug.h"

extern cVideo *videoDecoder;
extern cAudio *audioDecoder;
void hal_live_pids(uint16_t &vpid, uint16_t &apid, uint16_t &pcrpid);

#define hal_debug(args...) _hal_debug(HAL_DEBUG_PLAYER, this, args)
#define hal_info(args...) _hal_info(HAL_DEBUG_PLAYER, this, args)

#define NEUTRINO_MPV_CONF "/etc/neutrino/mpv.conf"

cMpvEngine *cMpvEngine::instance = NULL;
static OpenThreads::Mutex instanceLock;

/*
 * Live TV as a custom stream: tsdmx://<serial>
 *
 * The stream is a transport stream demux with the PIDs of the session. zapit
 * knows the PIDs and what is in them, but the HAL is never told the PMT PID,
 * so a PAT and a PMT that describe exactly these PIDs are put into the
 * stream. mpv then knows the codecs without guessing and sees one audio track.
 */
#define LIVE_PROTOCOL		"tsdmx"
#define LIVE_DMX_BUFFER		(4 * 1024 * 1024)
#define LIVE_PSI_INTERVAL_MS	400
#define TS_SIZE			188

struct LiveStream
{
	cDemux *dmx;
	int cancel[2];
	volatile bool cancelled;
	uint8_t pat[TS_SIZE];
	uint8_t pmt[TS_SIZE];
	int cc;
	int phase;		/* bytes into a transport packet */
	int64_t psi_time;
};

static int64_t monotonic_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint32_t crc32_mpeg(const uint8_t *data, int len)
{
	uint32_t crc = 0xffffffff;
	for (int i = 0; i < len; i++)
	{
		crc ^= (uint32_t)data[i] << 24;
		for (int k = 0; k < 8; k++)
			crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04c11db7 : (crc << 1);
	}
	return crc;
}

/* one section in one transport packet */
static void psi_packet(uint8_t *pkt, int pid, const uint8_t *section, int len)
{
	memset(pkt, 0xff, TS_SIZE);
	pkt[0] = 0x47;
	pkt[1] = 0x40 | ((pid >> 8) & 0x1f);
	pkt[2] = pid & 0xff;
	pkt[3] = 0x10;
	pkt[4] = 0x00; /* pointer field */
	memcpy(pkt + 5, section, len);
}

static int psi_finish(uint8_t *sec, int len)
{
	int section_length = len - 3 + 4;
	sec[1] = (sec[1] & 0xf0) | ((section_length >> 8) & 0x0f);
	sec[2] = section_length & 0xff;
	uint32_t crc = crc32_mpeg(sec, len);
	sec[len++] = crc >> 24;
	sec[len++] = crc >> 16;
	sec[len++] = crc >> 8;
	sec[len++] = crc;
	return len;
}

static int es_entry(uint8_t *p, int stream_type, int pid, const uint8_t *desc, int desc_len)
{
	p[0] = stream_type;
	p[1] = 0xe0 | ((pid >> 8) & 0x1f);
	p[2] = pid & 0xff;
	p[3] = 0xf0 | ((desc_len >> 8) & 0x0f);
	p[4] = desc_len & 0xff;
	if (desc_len)
		memcpy(p + 5, desc, desc_len);
	return 5 + desc_len;
}

static void live_psi(LiveStream *ls, const cMpvEngine::LiveParams &lp)
{
	/* any PID that is not in use carries the PMT */
	int pmt_pid = 0x0020;
	while (pmt_pid == lp.vpid || pmt_pid == lp.apid || pmt_pid == lp.pcrpid)
		pmt_pid++;

	uint8_t sec[TS_SIZE];
	int n = 0;
	sec[n++] = 0x00;	/* program association section */
	sec[n++] = 0xb0;
	sec[n++] = 0x00;
	sec[n++] = 0x00;	/* transport stream id */
	sec[n++] = 0x01;
	sec[n++] = 0xc1;	/* version 0, current */
	sec[n++] = 0x00;
	sec[n++] = 0x00;
	sec[n++] = 0x00;	/* program 1 */
	sec[n++] = 0x01;
	sec[n++] = 0xe0 | (pmt_pid >> 8);
	sec[n++] = pmt_pid & 0xff;
	n = psi_finish(sec, n);
	psi_packet(ls->pat, 0, sec, n);

	int pcr = lp.pcrpid ? lp.pcrpid : (lp.vpid ? lp.vpid : 0x1fff);
	n = 0;
	sec[n++] = 0x02;	/* program map section */
	sec[n++] = 0xb0;
	sec[n++] = 0x00;
	sec[n++] = 0x00;	/* program 1 */
	sec[n++] = 0x01;
	sec[n++] = 0xc1;
	sec[n++] = 0x00;
	sec[n++] = 0x00;
	sec[n++] = 0xe0 | (pcr >> 8);
	sec[n++] = pcr & 0xff;
	sec[n++] = 0xf0;	/* no program descriptors */
	sec[n++] = 0x00;
	if (lp.vpid)
	{
		int st;
		static const uint8_t vc1[] = { 0x05, 0x04, 'V', 'C', '-', '1' };
		switch (lp.vtype)
		{
			case VIDEO_FORMAT_MPEG4_H264:	st = 0x1b; break;
			case VIDEO_FORMAT_MPEG4_H265:	st = 0x24; break;
			case VIDEO_FORMAT_AVS:		st = 0x42; break;
			case VIDEO_FORMAT_VC1:		st = 0xea; break;
			default:			st = 0x02; break;
		}
		if (lp.vtype == VIDEO_FORMAT_VC1)
			n += es_entry(sec + n, st, lp.vpid, vc1, sizeof(vc1));
		else
			n += es_entry(sec + n, st, lp.vpid, NULL, 0);
	}
	if (lp.apid)
	{
		/* DVB signals these in a private stream with a descriptor */
		static const uint8_t ac3[] = { 0x6a, 0x01, 0x00 };
		static const uint8_t eac3[] = { 0x7a, 0x01, 0x00 };
		static const uint8_t dts[] = { 0x05, 0x04, 'D', 'T', 'S', '2' };
		switch (lp.atype)
		{
			case 0:		/* AC3 */
				n += es_entry(sec + n, 0x06, lp.apid, ac3, sizeof(ac3));
				break;
			case 0x22:	/* EAC3 */
				n += es_entry(sec + n, 0x06, lp.apid, eac3, sizeof(eac3));
				break;
			case 2:		/* DTS */
			case 0x10:	/* DTSHD */
				n += es_entry(sec + n, 0x06, lp.apid, dts, sizeof(dts));
				break;
			case 8:		/* AAC, ADTS */
				n += es_entry(sec + n, 0x0f, lp.apid, NULL, 0);
				break;
			case 9:		/* AAC, LATM */
				n += es_entry(sec + n, 0x11, lp.apid, NULL, 0);
				break;
			default:	/* MPEG */
				n += es_entry(sec + n, 0x04, lp.apid, NULL, 0);
				break;
		}
	}
	n = psi_finish(sec, n);
	psi_packet(ls->pmt, pmt_pid, sec, n);
}

static int64_t live_read(void *cookie, char *buf, uint64_t nbytes)
{
	LiveStream *ls = (LiveStream *)cookie;

	/* the tables go in between two packets, first of all and then again and again */
	int64_t now = monotonic_ms();
	if (ls->phase == 0 && nbytes >= 2 * TS_SIZE && (ls->psi_time == 0 || now - ls->psi_time >= LIVE_PSI_INTERVAL_MS))
	{
		ls->psi_time = now;
		ls->pat[3] = 0x10 | ls->cc;
		ls->pmt[3] = 0x10 | ls->cc;
		ls->cc = (ls->cc + 1) & 0x0f;
		memcpy(buf, ls->pat, TS_SIZE);
		memcpy(buf + TS_SIZE, ls->pmt, TS_SIZE);
		return 2 * TS_SIZE;
	}

	size_t want = nbytes;
	if (ls->phase == 0 && want > TS_SIZE)
		want -= want % TS_SIZE;

	struct pollfd pfd[2];
	pfd[0].fd = ls->dmx->getFD();
	pfd[0].events = POLLIN;
	pfd[1].fd = ls->cancel[0];
	pfd[1].events = POLLIN;
	while (!ls->cancelled)
	{
		pfd[0].revents = pfd[1].revents = 0;
		int r = poll(pfd, 2, 1000);
		if (ls->cancelled || (pfd[1].revents & POLLIN))
			break;
		if (r <= 0)
			continue; /* no signal, no data: keep waiting, a cancel ends it */
		ssize_t n = read(pfd[0].fd, buf, want);
		if (n > 0)
		{
			ls->phase = (ls->phase + n) % TS_SIZE;
			return n;
		}
		/* EOVERFLOW: the demux buffer ran over and was flushed, go on */
		if (n < 0 && errno != EAGAIN && errno != EINTR && errno != EOVERFLOW)
			return -1;
		if (n == 0 || (pfd[0].revents & (POLLHUP | POLLNVAL)))
			usleep(10000);
	}
	return 0;
}

static void live_close(void *cookie)
{
	LiveStream *ls = (LiveStream *)cookie;
	delete ls->dmx;
	close(ls->cancel[0]);
	close(ls->cancel[1]);
	delete ls;
}

static void live_cancel(void *cookie)
{
	LiveStream *ls = (LiveStream *)cookie;
	ls->cancelled = true;
	if (write(ls->cancel[1], "x", 1) < 0)
		; /* the flag is enough once the poll times out */
}

static int live_open(void *user_data, char *uri, mpv_stream_cb_info *info)
{
	cMpvEngine *engine = (cMpvEngine *)user_data;
	cMpvEngine::LiveParams lp;
	const char *serial = strstr(uri, "://");
	if (!serial || !engine->liveParams(atoi(serial + 3), lp))
		return MPV_ERROR_LOADING_FAILED; /* a zap has overtaken this one */

	LiveStream *ls = new LiveStream;
	ls->cancelled = false;
	ls->cc = 0;
	ls->phase = 0;
	ls->psi_time = 0;
	if (pipe2(ls->cancel, O_CLOEXEC | O_NONBLOCK) < 0)
	{
		delete ls;
		return MPV_ERROR_LOADING_FAILED;
	}
	live_psi(ls, lp);

	int pids[3] = { lp.vpid, lp.apid, lp.pcrpid };
	bool ok = false;
	ls->dmx = new cDemux(0);
	if (ls->dmx->Open(DMX_TP_CHANNEL, NULL, LIVE_DMX_BUFFER))
	{
		bool first = true;
		ok = true;
		for (int i = 0; i < 3 && ok; i++)
		{
			if (!pids[i] || (i > 0 && pids[i] == pids[0]) || (i > 1 && pids[i] == pids[1]))
				continue;
			ok = first ? ls->dmx->pesFilter(pids[i]) : ls->dmx->addPid(pids[i]);
			first = false;
		}
		ok = ok && !first && ls->dmx->Start();
	}
	if (!ok)
	{
		live_close(ls);
		return MPV_ERROR_LOADING_FAILED;
	}

	info->cookie = ls;
	info->read_fn = live_read;
	info->close_fn = live_close;
	info->cancel_fn = live_cancel;
	return 0;
}

cMpvEngine *cMpvEngine::getInstance()
{
	OpenThreads::ScopedLock<OpenThreads::Mutex> lock(instanceLock);
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
	OpenThreads::ScopedLock<OpenThreads::Mutex> lock(instanceLock);
	delete instance;
	instance = NULL;
}

cMpvEngine::cMpvEngine()
	: mpv(NULL), mQuit(false), mLoaded(false), mFailed(false), mAborted(false),
	  mEof(false), mIdle(true), mLastError(0), mOwner(OWNER_NONE), mLiveSerial(0),
	  mLiveVideoOn(false), mLiveAudioOn(false), mLiveSpeed(1.0), mLiveClockTime(0), mLiveClockTicks(0), mTimePos(0), mNoDeinterlace(false),
	  mWantEntry(0), mStartedEntry(0), mLoadedEntry(0)
{
	mVideo.valid = false;
	mVideo.w = mVideo.h = 0;
	mVideo.sw = mVideo.sh = 0;
	mVideo.aspect = 0;
	mVideo.fps = 0;
	memset(&mLive, 0, sizeof(mLive));

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
	/* only touches what is flagged as interlaced: TV and recordings of it */
	mpv_set_option_string(h, "deinterlace", "auto");
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
	mpv_observe_property(h, 5, "time-pos", MPV_FORMAT_DOUBLE);
	mpv_observe_property(h, 6, "demuxer-cache-duration", MPV_FORMAT_DOUBLE);
	r = mpv_stream_cb_add_ro(h, LIVE_PROTOCOL, this, live_open);
	if (r < 0)
		hal_info("%s: no live TV, mpv_stream_cb_add_ro: %s\n", __func__, mpv_error_string(r));
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
			mRenderLock.lock();
			mStartedEntry = ((mpv_event_start_file *)ev->data)->playlist_entry_id;
			mRenderLock.unlock();
			mStateLock.lock();
			mFailed = false;
			mEof = false;
			mStateLock.unlock();
			break;
		case MPV_EVENT_FILE_LOADED:
			hal_debug("%s: file loaded\n", __func__);
			mRenderLock.lock();
			mLoadedEntry = mStartedEntry;
			mRenderLock.unlock();
			mStateLock.lock();
			mLoaded = true;
			mStateCond.broadcast();
			mStateLock.unlock();
			break;
		case MPV_EVENT_END_FILE:
			handleEndFile((mpv_event_end_file *)ev->data);
			break;
		case MPV_EVENT_PLAYBACK_RESTART:
			liveReport();
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
			else if (ev->reply_userdata == 5)
			{
				mLiveLock.lock();
				mTimePos = (p->format == MPV_FORMAT_DOUBLE) ? *(double *)p->data : 0;
				mLiveLock.unlock();
			}
			else if (ev->reply_userdata == 6 && p->format == MPV_FORMAT_DOUBLE)
				liveClock(*(double *)p->data);
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
	v.sw = v.sh = 0;
	v.aspect = 0;
	if (node && node->format == MPV_FORMAT_NODE_MAP)
	{
		v.w = nodeInt(nodeMapGet(node, "dw"));
		v.h = nodeInt(nodeMapGet(node, "dh"));
		v.sw = nodeInt(nodeMapGet(node, "w"));
		v.sh = nodeInt(nodeMapGet(node, "h"));
		v.aspect = nodeDouble(nodeMapGet(node, "aspect"));
		v.valid = v.w > 0 && v.h > 0;

		/* Frames that stay in the decoder's memory (DRM PRIME, as from
		 * V4L2 on a Raspberry Pi) can only be deinterlaced after copying
		 * them out, and a software filter on top of that is more than
		 * such a machine can do at 1080i. Go without for those. */
		bool zero_copy = nodeString(nodeMapGet(node, "pixelformat")) == "drm_prime";
		if (v.valid && zero_copy != mNoDeinterlace)
		{
			mNoDeinterlace = zero_copy;
			static const char *no = "no", *automatic = "auto";
			hal_info("%s: deinterlacing %s\n", __func__, zero_copy ? "off, the frames are DRM PRIME" : "automatic");
			mpv_set_property_async(mpv, 0, "deinterlace", MPV_FORMAT_STRING, (void *)(zero_copy ? &no : &automatic));
		}
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
	/* a file takes the player away from live TV, a live session that is
	 * still on its way to mpv is not opened any more */
	mLiveLock.lock();
	mOwner = OWNER_PLAYBACK;
	mLiveSerial++;
	mLiveSpeed = 1.0;
	mLiveLock.unlock();
	setDouble("speed", 1.0);
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

	if (!loadFile(url.c_str(), NULL))
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

/* stop() and abort() are the file player's, they leave a live session alone */
void cMpvEngine::stop()
{
	mLiveLock.lock();
	bool live = (mOwner == OWNER_LIVE);
	if (!live)
		mOwner = OWNER_NONE;
	mLiveLock.unlock();
	if (live)
		return;
	renderBlock();
	const char *cmd[] = { "stop", NULL };
	command(cmd);
}

/* no frame of the current file is drawn from here on; waits for a frame that
 * is being drawn right now */
void cMpvEngine::renderBlock()
{
	mRenderLock.lock();
	mWantEntry = 0;
	mRenderLock.unlock();
}

bool cMpvEngine::renderBegin()
{
	mRenderLock.lock();
	return mWantEntry > 0 && mLoadedEntry == mWantEntry;
}

void cMpvEngine::renderEnd()
{
	mRenderLock.unlock();
}

/* "loadfile <url> replace", with the frames of what is replaced locked out */
bool cMpvEngine::loadFile(const char *url, const char *options)
{
	renderBlock();
	const char *cmd[] = { "loadfile", url, "replace", options ? "-1" : NULL, options, NULL };
	mpv_node res;
	int r = mpv_command_ret(mpv, cmd, &res);
	if (r < 0)
	{
		hal_info("%s: loadfile: %s\n", __func__, mpv_error_string(r));
		return false;
	}
	int64_t entry = nodeInt(nodeMapGet(&res, "playlist_entry_id"));
	mpv_free_node_contents(&res);
	mRenderLock.lock();
	mWantEntry = entry;
	mRenderLock.unlock();
	return true;
}

void cMpvEngine::abort()
{
	hal_info("%s\n", __func__);
	mLiveLock.lock();
	bool live = (mOwner == OWNER_LIVE);
	mLiveLock.unlock();
	if (live)
		return;
	mStateLock.lock();
	mAborted = true;
	mStateCond.broadcast();
	mStateLock.unlock();
	renderBlock();
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

bool cMpvEngine::getAudioParams(AudioParams &a)
{
	a.codec.clear();
	a.samplerate = 0;
	a.channels = 0;
	if (!getString("audio-codec-name", a.codec))
		return false;
	mpv_node node;
	if (mpv_get_property(mpv, "audio-params", MPV_FORMAT_NODE, &node) < 0)
		return true;
	a.samplerate = nodeInt(nodeMapGet(&node, "samplerate"));
	a.channels = nodeInt(nodeMapGet(&node, "channel-count"));
	mpv_free_node_contents(&node);
	return true;
}

/*
 * zapit sets the PID filters of the video, audio and PCR demux first and
 * then starts the audio and the video decoder one after the other. So the
 * first decoder that starts already finds every PID of the channel, and the
 * second one finds the session it needs running. A radio channel only ever
 * starts the audio decoder. Changing the audio track stops and starts the
 * audio decoder alone, with a new PID: the session is loaded again.
 */
void cMpvEngine::liveDecoder(bool video, bool on)
{
	mLiveLock.lock();
	if (video)
		mLiveVideoOn = on;
	else
		mLiveAudioOn = on;
	bool any = mLiveVideoOn || mLiveAudioOn;
	mLiveLock.unlock();

	if (!on)
	{
		if (!any)
			liveStop();
		return;
	}

	uint16_t vpid, apid, pcrpid;
	hal_live_pids(vpid, apid, pcrpid);
	LiveParams p;
	p.vpid = vpid;
	p.apid = apid;
	p.pcrpid = pcrpid;
	p.vtype = videoDecoder ? videoDecoder->GetStreamType() : 0;
	p.atype = audioDecoder ? audioDecoder->GetStreamType() : 0;
	if (!p.vpid && !p.apid)
		return;
	liveStart(p);
}

void cMpvEngine::liveStart(const LiveParams &p)
{
	mLiveLock.lock();
	if (mOwner == OWNER_LIVE && !memcmp(&mLive, &p, sizeof(p)))
	{
		mLiveLock.unlock();
		return;
	}
	mLive = p;
	mOwner = OWNER_LIVE;
	mLiveSpeed = 1.0;
	mLiveClockTime = 0;
	mLiveClockTicks = 0;
	mTimePos = 0;
	int serial = ++mLiveSerial;
	mLiveLock.unlock();

	hal_info("%s: #%d vpid 0x%04x (type %d) apid 0x%04x (type %d) pcr 0x%04x\n", __func__,
		 serial, p.vpid, p.vtype, p.apid, p.atype, p.pcrpid);

	mStateLock.lock();
	mLoaded = false;
	mFailed = false;
	mAborted = false;
	mEof = false;
	mLastError = 0;
	mLastErrorMsg.clear();
	mLastLogLine.clear();
	mStateLock.unlock();

	/* what a file may have left behind */
	static const char *reset[][2] =
	{
		{ "pause", "no" }, { "speed", "1.0" }, { "aid", "auto" }, { "sid", "auto" }, { "start", "none" }
	};
	for (size_t i = 0; i < sizeof(reset) / sizeof(reset[0]); i++)
		mpv_set_property_async(mpv, 0, reset[i][0], MPV_FORMAT_STRING, (void *)&reset[i][1]);
	std::vector<std::string> none;
	setStringList(mpv, "audio-files", none);
	setStringList(mpv, "http-header-fields", none);

	/* these only hold for the live stream, [protocol.tsdmx] in mpv.conf can
	 * override them per machine */
	const char *options =
		"demuxer-lavf-format=mpegts,"
		"rebase-start-time=no,"		/* time-pos is the PTS of the stream */
		"keep-open=no,"
		"cache=yes,"
		"cache-pause=no,"
		"demuxer-max-back-bytes=0,"
		"demuxer-lavf-analyzeduration=0.4,"
		"demuxer-lavf-probesize=524288,"
		"video-sync=audio,"
		"interpolation=no,"
		"audio-pitch-correction=no";	/* the clock is tuned by fractions of a percent */
	char url[32];
	snprintf(url, sizeof(url), LIVE_PROTOCOL "://%d", serial);
	loadFile(url, options);
}

void cMpvEngine::liveStop()
{
	mLiveLock.lock();
	bool live = (mOwner == OWNER_LIVE);
	if (live)
	{
		mOwner = OWNER_NONE;
		mLiveSerial++;
		mTimePos = 0;
		memset(&mLive, 0, sizeof(mLive));
	}
	mLiveLock.unlock();
	if (!live)
		return;
	hal_info("%s\n", __func__);
	renderBlock();
	const char *cmd[] = { "stop", NULL };
	mpv_command_async(mpv, 0, cmd);
	static const char *one = "1.0";
	mpv_set_property_async(mpv, 0, "speed", MPV_FORMAT_STRING, (void *)&one);
}

bool cMpvEngine::liveParams(int serial, LiveParams &p)
{
	OpenThreads::ScopedLock<OpenThreads::Mutex> lock(mLiveLock);
	if (mOwner != OWNER_LIVE || serial != mLiveSerial)
		return false;
	p = mLive;
	return true;
}

bool cMpvEngine::liveActive()
{
	OpenThreads::ScopedLock<OpenThreads::Mutex> lock(mLiveLock);
	return mOwner == OWNER_LIVE;
}

int64_t cMpvEngine::livePts()
{
	OpenThreads::ScopedLock<OpenThreads::Mutex> lock(mLiveLock);
	if (mOwner != OWNER_LIVE || mTimePos <= 0)
		return 0;
	return (int64_t)(mTimePos * 90000.0) & 0x1ffffffffLL;
}

/*
 * The sender's clock and ours are not the same one. Without correction the
 * buffer either runs dry or runs over, sooner or later. Keep about half a
 * second in it by playing a tiny bit slower or faster.
 */
void cMpvEngine::liveClock(double buffered)
{
	static const double target = 0.5, deadband = 0.15, limit = 0.01;

	mLiveLock.lock();
	bool live = (mOwner == OWNER_LIVE);
	int64_t now = monotonic_ms();
	bool due = (now - mLiveClockTime >= 1000);
	if (live && due)
		mLiveClockTime = now;
	double old = mLiveSpeed;
	int ticks = (live && due) ? ++mLiveClockTicks : 0;
	mLiveLock.unlock();
	if (!live || !due)
		return;

	if (ticks % 10 == 0)
	{
		/* for whoever watches a channel with the debug output on */
		int64_t vo_drops = 0, dec_drops = 0;
		getInt("frame-drop-count", vo_drops);
		getInt("decoder-frame-drop-count", dec_drops);
		hal_debug("live: %.2f s buffered, speed %.4f, frames dropped: %lld by the decoder, %lld at the output\n",
			  buffered, old, (long long)dec_drops, (long long)vo_drops);
	}

	double err = buffered - target;
	double speed = 1.0;
	if (err > deadband || err < -deadband)
	{
		speed = 1.0 + 0.02 * err;
		if (speed > 1.0 + limit)
			speed = 1.0 + limit;
		if (speed < 1.0 - limit)
			speed = 1.0 - limit;
	}
	if (speed > old - 0.0005 && speed < old + 0.0005)
		return;

	mLiveLock.lock();
	mLiveSpeed = speed;
	mLiveLock.unlock();
	hal_debug("%s: %.2f s buffered, speed %.4f\n", __func__, buffered, speed);
	mpv_set_property_async(mpv, 0, "speed", MPV_FORMAT_DOUBLE, &speed);
}

/* one line when a live session has its first picture or sound: what plays, and how */
void cMpvEngine::liveReport()
{
	mLiveLock.lock();
	bool live = (mOwner == OWNER_LIVE);
	int serial = mLiveSerial;
	mLiveLock.unlock();
	if (!live)
		return;

	std::string vcodec = "none", acodec = "none", hwdec = "no";
	std::vector<Track> tracks;
	getTracks(tracks);
	for (size_t i = 0; i < tracks.size(); i++)
	{
		if (!tracks[i].selected)
			continue;
		if (tracks[i].type == "video")
			vcodec = tracks[i].codec;
		else if (tracks[i].type == "audio")
			acodec = tracks[i].codec;
	}
	getString("hwdec-current", hwdec);
	int64_t w = 0, h = 0;
	getInt("video-params/w", w);
	getInt("video-params/h", h);
	hal_info("live #%d: playing, video %s %dx%d hwdec %s, audio %s\n", serial,
		 vcodec.c_str(), (int)w, (int)h, hwdec.empty() ? "no" : hwdec.c_str(), acodec.c_str());
}
