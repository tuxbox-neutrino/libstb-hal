/*
 * a demux in software that is fed from a transport stream file
 *
 * (C) 2026 Markus Volk <f_l_k@t-online.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <map>
#include <set>
#include <vector>

#include <OpenThreads/Mutex>
#include <OpenThreads/ScopedLock>
#include <OpenThreads/Thread>

#include "dmx_file.h"
#include "hal_debug.h"

#define hal_debug(args...) _hal_debug(HAL_DEBUG_DEMUX, NULL, args)
#define hal_info(args...) _hal_info(HAL_DEBUG_DEMUX, NULL, args)

#define TS_SIZE		188
#define FILTER_SIZE	16	/* DMX_FILTER_SIZE */
#define MAX_SECTION	4096

struct TsFileFilter
{
	int wfd;
	bool sections;
	bool payload;
	bool active;
	std::set<uint16_t> pids;
	/* section filter, laid over the section as the kernel does it: byte 0,
	 * then bytes 3.. because nobody filters on the length */
	uint8_t value[FILTER_SIZE];
	uint8_t mask[FILTER_SIZE];
	uint8_t mode[FILTER_SIZE];
	bool check_crc;
	/* what is collected for one write */
	std::vector<uint8_t> out;
	unsigned dropped;
};

/* collects the sections of one PID */
struct SectionBuf
{
	std::vector<uint8_t> data;
	int cc;
	bool synced;
	SectionBuf() : cc(-1), synced(false) {}
};

class cTsFile : public OpenThreads::Thread
{
	public:
		static cTsFile *getInstance();
		static const char *path();

		void add(TsFileFilter *f);
		void remove(TsFileFilter *f);
		OpenThreads::Mutex lock;

		void run();

	private:
		cTsFile();
		void packet(const uint8_t *p);
		void sectionData(uint16_t pid, const uint8_t *p, int len, bool start, int cc);
		void sectionsOut(uint16_t pid, SectionBuf &sb);
		void deliver(uint16_t pid, const uint8_t *sec, int len);
		void flush();

		std::set<TsFileFilter *> filters;
		std::map<uint16_t, SectionBuf> sections;
		int fd;
		double rate;
		/* pacing */
		int pcr_pid;
		int64_t pcr_base;
		int64_t wall_base;
};

static uint32_t crc32_mpeg(const uint8_t *data, int len)
{
	static uint32_t table[256];
	static bool init = false;
	if (!init)
	{
		for (uint32_t i = 0; i < 256; i++)
		{
			uint32_t c = i << 24;
			for (int k = 0; k < 8; k++)
				c = (c & 0x80000000) ? (c << 1) ^ 0x04c11db7 : (c << 1);
			table[i] = c;
		}
		init = true;
	}
	uint32_t crc = 0xffffffff;
	for (int i = 0; i < len; i++)
		crc = (crc << 8) ^ table[((crc >> 24) ^ data[i]) & 0xff];
	return crc;
}

static int64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

const char *cTsFile::path()
{
	const char *p = getenv("HAL_TSDMX_FILE");
	return (p && *p) ? p : NULL;
}

bool tsfile_enabled(void)
{
	static int enabled = -1;
	if (enabled < 0)
	{
		const char *p = cTsFile::path();
		enabled = (p && access(p, R_OK) == 0) ? 1 : 0;
		if (p && !enabled)
			hal_info("HAL_TSDMX_FILE: cannot read %s\n", p);
	}
	return enabled == 1;
}

cTsFile *cTsFile::getInstance()
{
	static OpenThreads::Mutex m;
	static cTsFile *instance = NULL;
	OpenThreads::ScopedLock<OpenThreads::Mutex> l(m);
	if (!instance)
	{
		instance = new cTsFile();
		instance->start();
	}
	return instance;
}

cTsFile::cTsFile()
{
	fd = -1;
	rate = 1.0;
	const char *r = getenv("HAL_TSDMX_RATE");
	if (r && atof(r) > 0.1)
		rate = atof(r);
	pcr_pid = -1;
	pcr_base = wall_base = 0;
}

void cTsFile::add(TsFileFilter *f)
{
	OpenThreads::ScopedLock<OpenThreads::Mutex> l(lock);
	filters.insert(f);
}

void cTsFile::remove(TsFileFilter *f)
{
	OpenThreads::ScopedLock<OpenThreads::Mutex> l(lock);
	filters.erase(f);
}

void cTsFile::run()
{
	hal_set_threadname("hal:tsfile");
	const char *file = path();
	fd = open(file, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
	{
		hal_info("HAL_TSDMX_FILE: %s: %m\n", file);
		return;
	}
	hal_info("HAL_TSDMX_FILE: demux data comes from %s, rate %.4f\n", file, rate);

	uint8_t buf[TS_SIZE * 64];
	int have = 0;
	while (true)
	{
		int r = read(fd, buf + have, sizeof(buf) - have);
		if (r <= 0)
		{
			/* around again, the clock of the stream starts over */
			lseek(fd, 0, SEEK_SET);
			have = 0;
			pcr_pid = -1;
			lock.lock();
			sections.clear();
			lock.unlock();
			if (r < 0)
				usleep(100000);
			continue;
		}
		have += r;
		int pos = 0;
		while (have - pos >= TS_SIZE)
		{
			if (buf[pos] != 0x47)
			{
				pos++;
				continue;
			}
			const uint8_t *p = buf + pos;
			pos += TS_SIZE;

			/* a PCR on the pace-making PID says when this packet is due */
			int pid = ((p[1] & 0x1f) << 8) | p[2];
			if ((p[3] & 0x20) && p[4] >= 7 && (p[5] & 0x10) && (pcr_pid < 0 || pcr_pid == pid))
			{
				int64_t pcr = ((int64_t)p[6] << 25) | ((int64_t)p[7] << 17) | ((int64_t)p[8] << 9) |
					      ((int64_t)p[9] << 1) | (p[10] >> 7);
				pcr = pcr * 300 + (((p[10] & 1) << 8) | p[11]);
				int64_t wall = now_us();
				int64_t due = wall_base + (int64_t)((pcr - pcr_base) / 27.0 / rate);
				if (pcr_pid < 0 || due - wall > 1000000 || due - wall < -1000000)
				{
					/* first one, or the stream's clock jumped */
					pcr_pid = pid;
					pcr_base = pcr;
					wall_base = wall;
				}
				else if (due > wall)
				{
					flush();
					usleep(due - wall);
				}
			}
			lock.lock();
			packet(p);
			lock.unlock();
		}
		flush();
		have -= pos;
		if (have > 0)
			memmove(buf, buf + pos, have);
	}
}

/* called with the lock held */
void cTsFile::packet(const uint8_t *p)
{
	if (p[1] & 0x80) /* transport error */
		return;
	uint16_t pid = ((p[1] & 0x1f) << 8) | p[2];
	bool start = p[1] & 0x40;
	int cc = p[3] & 0x0f;
	const uint8_t *pl = NULL;
	int pl_len = 0;
	if (p[3] & 0x10)
	{
		int off = 4;
		if (p[3] & 0x20)
			off += 1 + p[4];
		if (off < TS_SIZE)
		{
			pl = p + off;
			pl_len = TS_SIZE - off;
		}
	}

	bool want_sections = false;
	for (std::set<TsFileFilter *>::iterator it = filters.begin(); it != filters.end(); ++it)
	{
		TsFileFilter *f = *it;
		if (!f->active || !f->pids.count(pid))
			continue;
		if (f->sections)
			want_sections = true;
		else if (f->payload)
		{
			if (pl)
				f->out.insert(f->out.end(), pl, pl + pl_len);
		}
		else
			f->out.insert(f->out.end(), p, p + TS_SIZE);
	}
	if (want_sections && pl)
		sectionData(pid, pl, pl_len, start, cc);
}

void cTsFile::sectionData(uint16_t pid, const uint8_t *p, int len, bool start, int cc)
{
	SectionBuf &sb = sections[pid];
	if (sb.cc >= 0 && ((sb.cc + 1) & 0x0f) != cc)
	{
		/* packets are missing, what was collected is worthless */
		sb.data.clear();
		sb.synced = false;
	}
	sb.cc = cc;

	if (start)
	{
		int pointer = p[0];
		if (1 + pointer > len)
			return;
		if (sb.synced && pointer > 0)
		{
			/* the rest of the section before the new one */
			sb.data.insert(sb.data.end(), p + 1, p + 1 + pointer);
			sectionsOut(pid, sb);
		}
		sb.data.assign(p + 1 + pointer, p + len);
		sb.synced = true;
	}
	else if (sb.synced)
		sb.data.insert(sb.data.end(), p, p + len);
	else
		return;

	sectionsOut(pid, sb);
}

/* hand out every complete section at the head of the buffer */
void cTsFile::sectionsOut(uint16_t pid, SectionBuf &sb)
{
	while (sb.data.size() >= 3)
	{
		if (sb.data[0] == 0xff)
		{
			/* stuffing up to the end of the packet */
			sb.data.clear();
			sb.synced = false;
			return;
		}
		size_t len = 3 + (((sb.data[1] & 0x0f) << 8) | sb.data[2]);
		if (len > MAX_SECTION)
		{
			sb.data.clear();
			sb.synced = false;
			return;
		}
		if (sb.data.size() < len)
			return;
		deliver(pid, &sb.data[0], len);
		sb.data.erase(sb.data.begin(), sb.data.begin() + len);
	}
}

void cTsFile::deliver(uint16_t pid, const uint8_t *sec, int len)
{
	int crc_ok = -1;
	for (std::set<TsFileFilter *>::iterator it = filters.begin(); it != filters.end(); ++it)
	{
		TsFileFilter *f = *it;
		if (!f->active || !f->sections || !f->pids.count(pid))
			continue;

		/* the bits without "mode" all have to match; of those with it, at
		 * least one has to differ, if there are any */
		bool match = true, neq = false, have_neq = false;
		for (int i = 0; i < FILTER_SIZE && match; i++)
		{
			int pos = i ? i + 2 : 0;
			uint8_t x = (pos < len) ? (f->value[i] ^ sec[pos]) : f->value[i];
			if (f->mask[i] & ~f->mode[i] & x)
				match = false;
			if (f->mask[i] & f->mode[i])
			{
				have_neq = true;
				if (f->mask[i] & f->mode[i] & x)
					neq = true;
			}
		}
		if (!match || (have_neq && !neq))
			continue;

		if (f->check_crc && (sec[1] & 0x80))
		{
			if (crc_ok < 0)
				crc_ok = (crc32_mpeg(sec, len) == 0);
			if (!crc_ok)
				continue;
		}
		if (send(f->wfd, sec, len, MSG_DONTWAIT | MSG_NOSIGNAL) < 0)
			f->dropped++;
	}
}

/* write what the stream filters have collected */
void cTsFile::flush()
{
	OpenThreads::ScopedLock<OpenThreads::Mutex> l(lock);
	for (std::set<TsFileFilter *>::iterator it = filters.begin(); it != filters.end(); ++it)
	{
		TsFileFilter *f = *it;
		if (f->sections || f->out.empty())
			continue;
		/* a reader that does not keep up loses data, as with the kernel's buffer */
		ssize_t w = send(f->wfd, &f->out[0], f->out.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
		if (w != (ssize_t)f->out.size())
		{
			if (!(f->dropped++ % 500))
				hal_info("HAL_TSDMX_FILE: a reader is too slow, data dropped\n");
			/* keep whole transport packets apart */
			if (w > 0 && !f->payload && (w % TS_SIZE))
			{
				size_t rest = TS_SIZE - (w % TS_SIZE);
				if (w + rest <= f->out.size())
					send(f->wfd, &f->out[w], rest, MSG_DONTWAIT | MSG_NOSIGNAL);
			}
		}
		f->out.clear();
	}
}

TsFileFilter *tsfile_open(bool sections, int bufsize, int *fd)
{
	int sv[2];
	if (socketpair(AF_UNIX, (sections ? SOCK_SEQPACKET : SOCK_STREAM) | SOCK_CLOEXEC, 0, sv) < 0)
	{
		hal_info("%s: socketpair: %m\n", __func__);
		return NULL;
	}
	if (bufsize < 256 * 1024)
		bufsize = 256 * 1024;
	setsockopt(sv[1], SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));

	TsFileFilter *f = new TsFileFilter;
	f->wfd = sv[1];
	f->sections = sections;
	f->payload = false;
	f->active = false;
	f->check_crc = false;
	f->dropped = 0;
	memset(f->value, 0, sizeof(f->value));
	memset(f->mask, 0, sizeof(f->mask));
	memset(f->mode, 0, sizeof(f->mode));
	*fd = sv[0];
	cTsFile::getInstance()->add(f);
	return f;
}

void tsfile_close(TsFileFilter *f)
{
	if (!f)
		return;
	cTsFile::getInstance()->remove(f);
	close(f->wfd);
	delete f;
}

void tsfile_set_section(TsFileFilter *f, uint16_t pid, const uint8_t *filter,
			const uint8_t *mask, const uint8_t *mode, int len, bool check_crc)
{
	OpenThreads::ScopedLock<OpenThreads::Mutex> l(cTsFile::getInstance()->lock);
	memset(f->value, 0, sizeof(f->value));
	memset(f->mask, 0, sizeof(f->mask));
	memset(f->mode, 0, sizeof(f->mode));
	if (len > FILTER_SIZE)
		len = FILTER_SIZE;
	memcpy(f->value, filter, len);
	memcpy(f->mask, mask, len);
	if (mode)
		memcpy(f->mode, mode, len);
	f->check_crc = check_crc;
	f->pids.clear();
	f->pids.insert(pid);
	/* DMX_IMMEDIATE_START */
	f->active = true;
}

void tsfile_set_pid(TsFileFilter *f, uint16_t pid, bool payload)
{
	OpenThreads::ScopedLock<OpenThreads::Mutex> l(cTsFile::getInstance()->lock);
	f->payload = payload;
	f->pids.clear();
	f->pids.insert(pid);
	f->out.clear();
}

void tsfile_add_pid(TsFileFilter *f, uint16_t pid)
{
	OpenThreads::ScopedLock<OpenThreads::Mutex> l(cTsFile::getInstance()->lock);
	f->pids.insert(pid);
}

void tsfile_remove_pid(TsFileFilter *f, uint16_t pid)
{
	OpenThreads::ScopedLock<OpenThreads::Mutex> l(cTsFile::getInstance()->lock);
	f->pids.erase(pid);
}

void tsfile_start(TsFileFilter *f)
{
	OpenThreads::ScopedLock<OpenThreads::Mutex> l(cTsFile::getInstance()->lock);
	f->active = true;
}

void tsfile_stop(TsFileFilter *f)
{
	OpenThreads::ScopedLock<OpenThreads::Mutex> l(cTsFile::getInstance()->lock);
	f->active = false;
	f->out.clear();
}
