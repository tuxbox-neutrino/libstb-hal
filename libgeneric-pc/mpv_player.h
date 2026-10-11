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

#ifndef __MPV_PLAYER_H__
#define __MPV_PLAYER_H__

#include <stdint.h>
#include <string>
#include <vector>
#include <map>
#include <OpenThreads/Thread>
#include <OpenThreads/Mutex>
#include <OpenThreads/Condition>

struct mpv_handle;
struct mpv_node;
struct mpv_event;
struct mpv_event_end_file;
struct mpv_event_log_message;

class cMpvEngine : public OpenThreads::Thread
{
	public:
		struct Track
		{
			int64_t id;
			int64_t srcId; /* the PID for MPEG-TS */
			std::string type; /* video, audio, sub */
			std::string codec;
			std::string lang;
			std::string title;
			bool external;
			bool selected;
		};
		struct Chapter
		{
			double time;
			std::string title;
		};
		struct Edition
		{
			int64_t id;
			std::string title;
			bool current;
		};
		struct VideoParams
		{
			bool valid;
			int w; /* display size, aspect ratio applied */
			int h;
			int sw; /* size of the coded picture */
			int sh;
			double aspect;
			double fps;
		};
		struct AudioParams
		{
			std::string codec;
			int samplerate;
			int channels;
		};
		/* what a live TV session is made of, a PID of 0 means "none" */
		struct LiveParams
		{
			int vpid;
			int vtype; /* VIDEO_FORMAT */
			int apid;
			int atype; /* CZapitAudioChannel::ZapitAudioChannelType */
			int pcrpid;
		};

		static cMpvEngine *getInstance(); /* created by hal_api_init() */
		static void shutdown();

		mpv_handle *getHandle() { return mpv; }

		/* loads a file or URL paused, blocks until mpv has it open or failed */
		bool load(const std::string &url, const std::vector<std::string> &headers,
			  const std::string &userAgent, const std::string &audioFile, double start);
		void stop();
		void abort();
		bool isLoaded();
		bool isIdle();
		bool eofReached();
		int getLastError(std::string &message);

		bool setFlag(const char *name, bool v);
		bool setInt(const char *name, int64_t v);
		bool setDouble(const char *name, double v);
		bool setString(const char *name, const std::string &v);
		bool getFlag(const char *name, bool &v);
		bool getInt(const char *name, int64_t &v);
		bool getDouble(const char *name, double &v);
		bool getString(const char *name, std::string &v);
		bool command(const char *const *args);

		bool getTracks(std::vector<Track> &tracks);
		bool getChapters(std::vector<Chapter> &chapters);
		bool getEditions(std::vector<Edition> &editions);
		bool getMetadata(std::map<std::string, std::string> &meta);

		/* cached by the event thread, read by the GL thread */
		VideoParams getVideoParams();
		/* The GL thread brackets every mpv_render_context_render() with these.
		 * renderBegin() says whether the frame may be drawn: not while the file
		 * it belongs to is being replaced, its decoder is torn down by then and
		 * a hardware surface must not be touched any more. */
		bool renderBegin();
		void renderEnd();
		bool getAudioParams(AudioParams &a);

		/* Live TV. The video and the audio decoder report when they are started
		 * and stopped, the PIDs come from the demuxes zapit has set up. Nothing
		 * here waits for mpv, a zap must not block. */
		void liveDecoder(bool video, bool on);
		bool liveActive();
		/* presentation time of what is played, in 90 kHz units as in the stream */
		int64_t livePts();
		/* for the stream callbacks */
		bool liveParams(int serial, LiveParams &p);

		void run();

	private:
		cMpvEngine();
		~cMpvEngine();
		void handleEvent(mpv_event *ev);
		void handleEndFile(mpv_event_end_file *ef);
		void handleLog(mpv_event_log_message *msg);
		void updateVideoParams(mpv_node *node);
		int mapError(int error);
		void liveStart(const LiveParams &p);
		void liveStop();
		void renderBlock();
		bool loadFile(const char *url, const char *options);

		OpenThreads::Mutex mRenderLock;
		int64_t mWantEntry;	/* playlist entry that was asked for last, 0: none */
		int64_t mStartedEntry;
		int64_t mLoadedEntry;
		void liveClock(double buffered);
		void liveReport();

		enum Owner { OWNER_NONE, OWNER_PLAYBACK, OWNER_LIVE };
		OpenThreads::Mutex mLiveLock;
		Owner mOwner;
		LiveParams mLive;
		int mLiveSerial;
		bool mLiveVideoOn;
		bool mLiveAudioOn;
		double mLiveSpeed;
		int64_t mLiveClockTime;
		int mLiveClockTicks;
		double mTimePos;
		bool mNoDeinterlace;

		static cMpvEngine *instance;
		mpv_handle *mpv;
		bool mQuit;

		OpenThreads::Mutex mStateLock;
		OpenThreads::Condition mStateCond;
		bool mLoaded;
		bool mFailed;
		bool mAborted;
		bool mEof;
		bool mIdle;
		int mLastError;
		std::string mLastErrorMsg;
		std::string mLastLogLine;

		OpenThreads::Mutex mVideoLock;
		VideoParams mVideo;
};

#endif // __MPV_PLAYER_H__
