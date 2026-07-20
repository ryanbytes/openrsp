/*
 * Copyright (C) 2013 by Miroslav Slugen <thunder.m@email.cz
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
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

#include <stdint.h>

#define DEFAULT_ISO_BUFFER      1024
#define DEFAULT_ISO_BUFFERS     3
#define DEFAULT_ISO_PACKETS     8
#define DEFAULT_ISO_TIMEOUT     1000

#define DEFAULT_BULK_BUFFER     16384
#define DEFAULT_BULK_TIMEOUT    1000

#define DEFAULT_BUF_NUMBER      32

/* Only endpoint stalls are safe to clear and resubmit.  An overflow means the
 * device sent more data than the host requested; retrying it can escalate a
 * USB babble fault into a reset of the whole upstream hub on macOS. */
int mirisdr_rspduo_bulk_status_requires_restart(int status);

/* Completed transfers may be resubmitted during startup and normal streaming,
 * but never after cancellation or failure has started. */
int mirisdr_async_status_allows_resubmit(int status);

/* Wall-clock time for correlating transfer failures with daemon recovery. */
uint64_t mirisdr_wall_clock_milliseconds(void);
