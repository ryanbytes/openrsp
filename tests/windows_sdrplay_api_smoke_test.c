/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sdrplay_api.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void)
{
    float version = 0.0f;
    unsigned int count = 0u;
    assert(sdrplay_api_ApiVersion(&version) == sdrplay_api_Success);
    assert(fabsf(version - SDRPLAY_API_VERSION) < 0.001f);
    assert(sdrplay_api_Open() == sdrplay_api_Success);
    assert(sdrplay_api_GetDevices(NULL, &count, 0u) == sdrplay_api_Success);
    assert(count == 0u);
    assert(sdrplay_api_LockDeviceApi() == sdrplay_api_Success);
    assert(sdrplay_api_UnlockDeviceApi() == sdrplay_api_Success);
    assert(sdrplay_api_Close() == sdrplay_api_Success);
    puts("WINDOWS_SDRPLAY_API_SMOKE_OK");
    return 0;
}
