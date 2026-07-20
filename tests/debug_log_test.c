/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "debug_log.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    openrsp_debug_reset();
    assert(!openrsp_debug_enabled(sdrplay_api_DbgLvl_Message));
    assert(!openrsp_debug_enabled(sdrplay_api_DbgLvl_Warning));
    assert(!openrsp_debug_enabled(sdrplay_api_DbgLvl_Error));

    openrsp_debug_set_level(sdrplay_api_DbgLvl_Error);
    assert(!openrsp_debug_enabled(sdrplay_api_DbgLvl_Message));
    assert(!openrsp_debug_enabled(sdrplay_api_DbgLvl_Warning));
    assert(openrsp_debug_enabled(sdrplay_api_DbgLvl_Error));

    openrsp_debug_set_level(sdrplay_api_DbgLvl_Warning);
    assert(!openrsp_debug_enabled(sdrplay_api_DbgLvl_Message));
    assert(openrsp_debug_enabled(sdrplay_api_DbgLvl_Warning));
    assert(openrsp_debug_enabled(sdrplay_api_DbgLvl_Error));

    openrsp_debug_set_level(sdrplay_api_DbgLvl_Message);
    assert(openrsp_debug_enabled(sdrplay_api_DbgLvl_Message));
    assert(openrsp_debug_enabled(sdrplay_api_DbgLvl_Warning));
    assert(openrsp_debug_enabled(sdrplay_api_DbgLvl_Error));

    openrsp_debug_set_level(sdrplay_api_DbgLvl_Verbose);
    assert(openrsp_debug_enabled(sdrplay_api_DbgLvl_Message));
    assert(openrsp_debug_enabled(sdrplay_api_DbgLvl_Warning));
    assert(openrsp_debug_enabled(sdrplay_api_DbgLvl_Error));

    puts("DEBUG_LOG_OK");
    return 0;
}
