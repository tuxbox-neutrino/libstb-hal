#ifndef __PLAYBACK_LIB_H__
#define __PLAYBACK_LIB_H__

#include <stdint.h>
#include <string>
#include <vector>
#include <OpenThreads/Mutex>

typedef enum
{
	PLAYMODE_TS = 0,
	PLAYMODE_FILE
} playmode_t;

struct AVFormatContext;
class cMpvEngine;

class cPlayback
{
		friend class CStreamInfo2;
	private:
		static OpenThreads::Mutex mutex;
		cMpvEngine *engine;
		bool playing;
		bool first;
		int nPlaybackSpeed;
		int mAudioStream;
		int mSubtitleStream;
		int mTeletextStream;
		playmode_t pm;
		std::string fn_ts;
		std::string fn_xml;
		off64_t last_size;
		int init_jump;
		std::string extractParam(const std::string &hdrs, const std::string &paramName);
		void parseHeaders(const std::string &headers, std::vector<std::string> &fields, std::string &userAgent);
		bool Stop(void);
	public:
		cPlayback(int num = 0);
		~cPlayback();
		bool Open(playmode_t PlayMode);
		void Close(void);
		bool Start(char *filename, int vpid, int vtype, int apid, int ac3, int duration, std::string headers = "", std::string filename2 = "");
		bool Start(std::string filename, std::string headers = "", std::string filename2 = "");
		bool SetAPid(int pid, bool ac3 = false);
		bool SetVPid(int /*pid*/) { return true; }
		bool SetSubtitlePid(int pid);
		bool SetTeletextPid(int pid);
		int GetAPid(void) { return mAudioStream; }
		int GetVPid(void) { return 0; }
		int GetSubtitlePid(void) { return mSubtitleStream; }
		int GetTeletextPid(void) { return mTeletextStream; }
		void SuspendSubtitle(bool);
		int GetFirstTeletextPid(void) { return -1; }
		bool SetSpeed(int speed);
		bool GetSpeed(int &speed) const;
		bool GetPosition(int &position, int &duration, bool isWebChannel = false);
		void GetPts(uint64_t &pts);
		bool SetPosition(int position, bool absolute = false);
		void FindAllPids(int *apids, unsigned int *ac3flags, unsigned int *numpida, std::string *language);
		void FindAllPids(uint16_t *apids, unsigned short *ac3flags, uint16_t *numpida, std::string *language);
		void FindAllSubtitlePids(int *pids, unsigned int *numpids, std::string *language);
		void FindAllTeletextsubtitlePids(int *pids, unsigned int *numpidt, std::string *tlanguage, int *mags, int *pages);
		void RequestAbort(void);
		bool IsPlaying(void);
		uint64_t GetReadCount(void);
		bool GetLastOpenError(int &code, std::string &message);
		void FindAllSubs(int *pids, unsigned int *supported, unsigned int *numpida, std::string *language);
		void FindAllSubs(uint16_t *pids, unsigned short *supported, uint16_t *numpida, std::string *language);
		bool SelectSubtitles(int pid, std::string charset = "");
		void GetTitles(std::vector<int> &playlists, std::vector<std::string> &titles, int &current);
		void SetTitle(int title);
		void GetChapters(std::vector<int> &positions, std::vector<std::string> &titles);
		void GetMetadata(std::vector<std::string> &keys, std::vector<std::string> &values);
		AVFormatContext *GetAVFormatContext() { return NULL; }
		void ReleaseAVFormatContext() {}
};

#endif // __PLAYBACK_LIB_H__
