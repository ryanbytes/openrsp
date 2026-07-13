/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <sdrplay_api.h>

#include <cassert>
#include <iostream>

int main()
{
    sdrplay_api_Open_t open_api = sdrplay_api_Open;
    sdrplay_api_GetDevices_t get_devices = sdrplay_api_GetDevices;
    sdrplay_api_RxChannelParamsT channel{};
    channel.tunerParams.loMode = sdrplay_api_LO_168MHz;
    channel.rspDxTunerParams.hdrBw = sdrplay_api_RspDx_HDRMODE_BW_0_500;
    if (open_api == nullptr || get_devices == nullptr) return 1;
    if (channel.tunerParams.loMode != sdrplay_api_LO_168MHz ||
        channel.rspDxTunerParams.hdrBw != sdrplay_api_RspDx_HDRMODE_BW_0_500)
        return 1;
    std::cout << "SDRPLAY_HEADER_CPP_OK\n";
    return 0;
}
