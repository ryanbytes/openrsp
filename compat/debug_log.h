/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OPENRSP_DEBUG_LOG_H
#define OPENRSP_DEBUG_LOG_H

#include "sdrplay_api_compat.h"

void openrsp_debug_reset(void);
void openrsp_debug_set_level(sdrplay_api_DbgLvl_t level);
int openrsp_debug_enabled(sdrplay_api_DbgLvl_t message_level);
void openrsp_debug_log(sdrplay_api_DbgLvl_t message_level,
                       const char *format, ...);

#endif
