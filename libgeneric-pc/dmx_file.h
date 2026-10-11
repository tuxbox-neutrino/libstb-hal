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

#ifndef __DMX_FILE_H__
#define __DMX_FILE_H__

#include <stdint.h>

/*
 * export HAL_TSDMX_FILE=/path/to/file.ts and every cDemux reads from that
 * file instead of /dev/dvb/adapter0/demux0. The file is played in a loop at
 * the pace of its PCR, so that section filters, PES filters and transport
 * stream taps see what a tuned transponder would deliver. This is what lets
 * live TV be tested on a machine without a tuner.
 * HAL_TSDMX_RATE=1.001 plays it that much faster, as a sender whose clock
 * drifts against ours.
 *
 * A filter hands its data over through a socket pair, so the reader keeps
 * polling and reading a file descriptor as it does with the kernel demux.
 */
struct TsFileFilter;

bool tsfile_enabled(void);
/* sections: one section per read; otherwise a byte stream. Returns the
 * filter and in *fd the descriptor to read from. */
TsFileFilter *tsfile_open(bool sections, int bufsize, int *fd);
void tsfile_close(TsFileFilter *f);
void tsfile_set_section(TsFileFilter *f, uint16_t pid, const uint8_t *filter,
			const uint8_t *mask, const uint8_t *mode, int len, bool check_crc);
/* payload: PES data without the transport packet around it */
void tsfile_set_pid(TsFileFilter *f, uint16_t pid, bool payload);
void tsfile_add_pid(TsFileFilter *f, uint16_t pid);
void tsfile_remove_pid(TsFileFilter *f, uint16_t pid);
void tsfile_start(TsFileFilter *f);
void tsfile_stop(TsFileFilter *f);

#endif // __DMX_FILE_H__
