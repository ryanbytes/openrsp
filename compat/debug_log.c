/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "debug_log.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>

static atomic_int debug_level;

void openrsp_debug_reset(void)
{
    atomic_store(&debug_level, sdrplay_api_DbgLvl_Disable);
}

void openrsp_debug_set_level(sdrplay_api_DbgLvl_t level)
{
    atomic_store(&debug_level, level);
}

int openrsp_debug_enabled(sdrplay_api_DbgLvl_t message_level)
{
    sdrplay_api_DbgLvl_t configured =
        (sdrplay_api_DbgLvl_t)atomic_load(&debug_level);
    if (configured == sdrplay_api_DbgLvl_Disable) return 0;
    if (configured == sdrplay_api_DbgLvl_Verbose ||
        configured == sdrplay_api_DbgLvl_Message) return 1;
    if (configured == sdrplay_api_DbgLvl_Warning)
        return message_level == sdrplay_api_DbgLvl_Warning ||
               message_level == sdrplay_api_DbgLvl_Error;
    return configured == sdrplay_api_DbgLvl_Error &&
           message_level == sdrplay_api_DbgLvl_Error;
}

void openrsp_debug_log(sdrplay_api_DbgLvl_t message_level,
                       const char *format, ...)
{
    if (!openrsp_debug_enabled(message_level)) return;
    va_list arguments;
    va_start(arguments, format);
    (void)vfprintf(stderr, format, arguments);
    va_end(arguments);
}
