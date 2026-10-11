/*
	cPlayback on top of libmpv for the generic PC backend

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
#include <map>
#include <sys/stat.h>

#include "playback_lib.h"
#include "mpv_player.h"
#include "hal_debug.h"

#define hal_debug(args...) _hal_debug(HAL_DEBUG_PLAYBACK, this, args)
#define hal_info(args...) _hal_info(HAL_DEBUG_PLAYBACK, this, args)

/* neutrino's arrays in CMoviePlayerGui are MAX_PLAYBACK_PIDS long */
#define MAX_TRACKS 40

OpenThreads::Mutex cPlayback::mutex;

cPlayback::cPlayback(int)
{
	hal_info("%s\n", __func__);
	engine = cMpvEngine::getInstance();
	playing = false;
	first = false;
	nPlaybackSpeed = 0;
	mAudioStream = 0;
	mSubtitleStream = -1;
	mTeletextStream = -1;
	pm = PLAYMODE_FILE;
	last_size = 0;
	init_jump = -1;
}

cPlayback::~cPlayback()
{
	hal_info("%s\n", __func__);
	Close();
}

bool cPlayback::Open(playmode_t PlayMode)
{
	hal_info("%s: mode %d\n", __func__, PlayMode);
	pm = PlayMode;
	fn_ts = "";
	fn_xml = "";
	last_size = 0;
	init_jump = -1;
	return engine != NULL;
}

void cPlayback::Close(void)
{
	hal_info("%s\n", __func__);
	Stop();
}

bool cPlayback::Stop(void)
{
	if (!engine)
		return false;
	engine->stop();
	playing = false;
	nPlaybackSpeed = 0;
	return true;
}

std::string cPlayback::extractParam(const std::string &hdrs, const std::string &paramName)
{
	size_t paramPos = hdrs.find(paramName);
	if (paramPos == std::string::npos)
		return "";
	size_t valuePos = paramPos + paramName.length();
	size_t valueEndPos = hdrs.find('&', valuePos);
	if (valueEndPos == std::string::npos)
		valueEndPos = hdrs.length();
	std::string value = hdrs.substr(valuePos, valueEndPos - valuePos);
	size_t trailingSpacePos = value.find_last_not_of(" \t\r\n");
	if (trailingSpacePos != std::string::npos)
		value.erase(trailingSpacePos + 1);
	return value;
}

/* neutrino hands over HTTP header lines ("Key: value\n"); some channel
 * lists append "&User-Agent=...&Referer=..." to the URL instead */
void cPlayback::parseHeaders(const std::string &headers, std::vector<std::string> &fields, std::string &userAgent)
{
	size_t pos = 0;
	while (pos < headers.size())
	{
		size_t end = headers.find('\n', pos);
		if (end == std::string::npos)
			end = headers.size();
		std::string line = headers.substr(pos, end - pos);
		pos = end + 1;
		while (!line.empty() && (line[line.size() - 1] == '\r' || line[line.size() - 1] == ' '))
			line.erase(line.size() - 1);
		if (line.empty())
			continue;
		size_t colon = line.find(':');
		if (colon == std::string::npos)
			continue;
		if (!strncasecmp(line.c_str(), "User-Agent:", 11))
		{
			userAgent = line.substr(colon + 1);
			size_t s = userAgent.find_first_not_of(' ');
			userAgent = (s == std::string::npos) ? "" : userAgent.substr(s);
			continue;
		}
		fields.push_back(line);
	}
}

bool cPlayback::Start(std::string filename, std::string headers, std::string filename2)
{
	return Start((char *)filename.c_str(), 0, 0, 0, 0, 0, headers, filename2);
}

bool cPlayback::Start(char *filename, int /*vpid*/, int /*vtype*/, int apid, int /*ac3*/, int /*duration*/, std::string headers, std::string filename2)
{
	if (!engine)
		return false;
	std::string file(filename);
	std::string url(file);
	bool isHTTP = file.compare(0, 7, "http://") == 0 || file.compare(0, 8, "https://") == 0;
	hal_info("%s: %s%s\n", __func__, filename, filename2.empty() ? "" : " (with second file)");

	if (file.compare(0, 7, "file://") == 0)
	{
		file = file.substr(7);
		url = file;
	}
	if (pm == PLAYMODE_TS && !isHTTP && file.size() > 3 && file.compare(file.size() - 3, 3, ".ts") == 0)
	{
		/* a recording that may still be growing */
		fn_ts = file;
		fn_xml = file.substr(0, file.size() - 2) + "xml";
		struct stat64 s;
		if (!stat64(fn_ts.c_str(), &s))
			last_size = s.st_size;
		url = "appending://" + file;
	}

	std::vector<std::string> fields;
	std::string userAgent;
	if (isHTTP && headers.empty())
	{
		size_t amp = file.find('&');
		if (amp != std::string::npos)
		{
			std::string hdrs = file.substr(amp + 1);
			std::string val = extractParam(hdrs, "User-Agent=");
			if (!val.empty())
				headers += "User-Agent: " + val + "\n";
			val = extractParam(hdrs, "Referer=");
			if (!val.empty())
				headers += "Referer: " + val + "\n";
			if (!headers.empty())
				url = file.substr(0, amp);
		}
	}
	parseHeaders(headers, fields, userAgent);

	double start = (init_jump > 0) ? init_jump / 1000.0 : 0;
	init_jump = -1;
	playing = engine->load(url, fields, userAgent, filename2, start);
	first = true;
	nPlaybackSpeed = 0;
	if (playing && apid > 0)
		SetAPid(apid, false);
	return playing;
}

/* neutrino hands over mpv track ids it got from FindAllPids(), but the
 * PIDs from a recording's .xml for MPEG-TS; map a PID onto its track */
static int64_t trackForPid(cMpvEngine *engine, const char *type, int pid)
{
	std::vector<cMpvEngine::Track> tracks;
	engine->getTracks(tracks);
	for (size_t i = 0; i < tracks.size(); i++)
		if (tracks[i].type == type && tracks[i].id == pid)
			return pid;
	for (size_t i = 0; i < tracks.size(); i++)
		if (tracks[i].type == type && tracks[i].srcId == pid)
			return tracks[i].id;
	return -1;
}

bool cPlayback::SetAPid(int pid, bool /*ac3*/)
{
	hal_info("%s: %d\n", __func__, pid);
	if (!engine)
		return false;
	mAudioStream = pid;
	int64_t id = trackForPid(engine, "audio", pid);
	if (id < 0)
	{
		hal_info("%s: no audio track for %d\n", __func__, pid);
		return false;
	}
	return engine->setInt("aid", id);
}

bool cPlayback::SetSubtitlePid(int pid)
{
	return SelectSubtitles(pid);
}

bool cPlayback::SetTeletextPid(int /*pid*/)
{
	return false;
}

void cPlayback::SuspendSubtitle(bool suspend)
{
	if (engine)
		engine->setFlag("sub-visibility", !suspend);
}

bool cPlayback::SetSpeed(int speed)
{
	hal_info("%s: %d\n", __func__, speed);
	if (!engine || !playing)
		return false;
	if (speed == 0)
	{
		engine->setFlag("pause", true);
	}
	else
	{
		engine->setString("play-direction", speed < 0 ? "backward" : "forward");
		engine->setDouble("speed", speed < 0 ? -speed : speed);
		engine->setFlag("pause", false);
	}
	nPlaybackSpeed = speed;
	return true;
}

bool cPlayback::GetSpeed(int &speed) const
{
	speed = nPlaybackSpeed;
	return true;
}

bool cPlayback::GetPosition(int &position, int &duration, bool isWebChannel)
{
	if (!engine)
		return false;

	/* a recording in progress grows while it is played, take its
	 * duration from the file times as the hardware boxes do */
	if (pm == PLAYMODE_TS && !fn_ts.empty())
	{
		struct stat64 s;
		if (!stat64(fn_ts.c_str(), &s))
		{
			if (!playing || last_size != s.st_size)
			{
				last_size = s.st_size;
				time_t curr_time = s.st_mtime;
				if (!stat64(fn_xml.c_str(), &s))
				{
					duration = (curr_time - s.st_mtime) * 1000;
					if (!playing)
						return true;
				}
			}
		}
	}

	if (!playing)
		return false;

	double pos = 0, dur = 0;
	engine->getDouble("time-pos", pos);
	position = (int)(pos * 1000);
	if (engine->getDouble("duration", dur))
		duration = (int)(dur * 1000);
	else if (pm != PLAYMODE_TS || fn_ts.empty())
		duration = 0;

	if (engine->eofReached())
	{
		hal_info("%s: end of stream\n", __func__);
		if (isWebChannel)
		{
			position = duration - 1000;
			return true;
		}
		return false;
	}
	return true;
}

void cPlayback::GetPts(uint64_t &pts)
{
	double pos = 0;
	if (engine && engine->getDouble("time-pos", pos))
		pts = (uint64_t)(pos * 90000);
	else
		pts = 0;
}

bool cPlayback::SetPosition(int position, bool absolute)
{
	hal_info("%s: %d %s\n", __func__, position, absolute ? "absolute" : "relative");
	if (!engine)
		return false;
	if (!playing)
	{
		init_jump = absolute ? position : -1;
		return false;
	}
	char buf[32];
	snprintf(buf, sizeof(buf), "%.3f", position / 1000.0);
	const char *cmd[] = { "seek", buf, absolute ? "absolute" : "relative", NULL };
	return engine->command(cmd);
}

static unsigned int ac3flagFromCodec(const std::string &codec)
{
	if (codec == "ac3" || codec == "eac3")
		return 1;
	if (codec == "mp3")
		return 4;
	if (codec.compare(0, 3, "aac") == 0)
		return 5;
	if (codec == "dts" || codec == "dca")
		return 6;
	return 0;
}

static std::string trackLanguage(const cMpvEngine::Track &t)
{
	if (!t.lang.empty())
		return t.lang;
	if (!t.title.empty())
		return t.title;
	return "unk";
}

void cPlayback::FindAllPids(int *apids, unsigned int *ac3flags, unsigned int *numpida, std::string *language)
{
	*numpida = 0;
	if (!engine)
		return;
	std::vector<cMpvEngine::Track> tracks;
	engine->getTracks(tracks);
	unsigned int n = 0;
	for (size_t i = 0; i < tracks.size() && n < MAX_TRACKS; i++)
	{
		if (tracks[i].type != "audio")
			continue;
		apids[n] = (int)tracks[i].id;
		ac3flags[n] = ac3flagFromCodec(tracks[i].codec);
		language[n] = trackLanguage(tracks[i]);
		if (tracks[i].selected)
			mAudioStream = apids[n];
		hal_debug("%s: audio %d codec %s lang %s flags %u\n", __func__, apids[n], tracks[i].codec.c_str(), language[n].c_str(), ac3flags[n]);
		n++;
	}
	*numpida = n;
}

void cPlayback::FindAllPids(uint16_t *apids, unsigned short *ac3flags, uint16_t *numpida, std::string *language)
{
	int ipids[MAX_TRACKS];
	unsigned int iflags[MAX_TRACKS];
	unsigned int n = 0;
	FindAllPids(ipids, iflags, &n, language);
	for (unsigned int i = 0; i < n; i++)
	{
		apids[i] = ipids[i];
		ac3flags[i] = iflags[i];
	}
	*numpida = n;
}

void cPlayback::FindAllSubs(int *pids, unsigned int *supported, unsigned int *numpida, std::string *language)
{
	*numpida = 0;
	if (!engine)
		return;
	std::vector<cMpvEngine::Track> tracks;
	engine->getTracks(tracks);
	unsigned int n = 0;
	for (size_t i = 0; i < tracks.size() && n < MAX_TRACKS; i++)
	{
		if (tracks[i].type != "sub")
			continue;
		pids[n] = (int)tracks[i].id;
		supported[n] = 1;
		language[n] = trackLanguage(tracks[i]);
		n++;
	}
	*numpida = n;
}

void cPlayback::FindAllSubs(uint16_t *pids, unsigned short *supported, uint16_t *numpida, std::string *language)
{
	int ipids[MAX_TRACKS];
	unsigned int isup[MAX_TRACKS];
	unsigned int n = 0;
	FindAllSubs(ipids, isup, &n, language);
	for (unsigned int i = 0; i < n; i++)
	{
		pids[i] = ipids[i];
		supported[i] = isup[i];
	}
	*numpida = n;
}

void cPlayback::FindAllSubtitlePids(int *pids, unsigned int *numpids, std::string *language)
{
	unsigned int supported[MAX_TRACKS];
	FindAllSubs(pids, supported, numpids, language);
}

void cPlayback::FindAllTeletextsubtitlePids(int * /*pids*/, unsigned int *numpidt, std::string * /*tlanguage*/, int * /*mags*/, int * /*pages*/)
{
	*numpidt = 0;
}

bool cPlayback::SelectSubtitles(int pid, std::string charset)
{
	hal_info("%s: %d %s\n", __func__, pid, charset.c_str());
	if (!engine)
		return false;
	mSubtitleStream = pid;
	if (!charset.empty())
		engine->setString("sub-codepage", charset);
	if (pid < 0)
		return engine->setString("sid", "no");
	int64_t id = trackForPid(engine, "sub", pid);
	return id >= 0 && engine->setInt("sid", id);
}

void cPlayback::RequestAbort(void)
{
	hal_info("%s\n", __func__);
	if (engine)
		engine->abort();
}

bool cPlayback::IsPlaying(void)
{
	return playing && engine && engine->isLoaded();
}

uint64_t cPlayback::GetReadCount(void)
{
	int64_t pos = 0;
	if (engine && engine->getInt("stream-pos", pos) && pos > 0)
		return (uint64_t)pos;
	return 0;
}

bool cPlayback::GetLastOpenError(int &code, std::string &message)
{
	code = 0;
	message.clear();
	if (!engine)
		return false;
	code = engine->getLastError(message);
	return code != 0;
}

void cPlayback::GetTitles(std::vector<int> &playlists, std::vector<std::string> &titles, int &current)
{
	playlists.clear();
	titles.clear();
	current = -1;
	if (!engine)
		return;
	std::vector<cMpvEngine::Edition> editions;
	engine->getEditions(editions);
	int64_t cur = -1;
	engine->getInt("current-edition", cur);
	for (size_t i = 0; i < editions.size(); i++)
	{
		playlists.push_back((int)editions[i].id);
		titles.push_back(editions[i].title);
		if (editions[i].id == cur)
			current = (int)editions[i].id;
	}
}

void cPlayback::SetTitle(int title)
{
	if (engine)
		engine->setInt("edition", title);
}

void cPlayback::GetChapters(std::vector<int> &positions, std::vector<std::string> &titles)
{
	positions.clear();
	titles.clear();
	if (!engine)
		return;
	std::vector<cMpvEngine::Chapter> chapters;
	engine->getChapters(chapters);
	for (size_t i = 0; i < chapters.size(); i++)
	{
		positions.push_back((int)(chapters[i].time * 1000));
		titles.push_back(chapters[i].title);
	}
}

void cPlayback::GetMetadata(std::vector<std::string> &keys, std::vector<std::string> &values)
{
	keys.clear();
	values.clear();
	if (!engine)
		return;
	std::map<std::string, std::string> meta;
	engine->getMetadata(meta);
	if (meta.find("title") == meta.end() && meta.find("icy-title") != meta.end())
		meta["title"] = meta["icy-title"];
	for (std::map<std::string, std::string>::const_iterator i = meta.begin(); i != meta.end(); ++i)
	{
		keys.push_back(i->first);
		values.push_back(i->second);
	}
}
