/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <sdrplay_api.h>
#include <sdrplay_api_callback.h>
#include <sdrplay_api_control.h>
#include <sdrplay_api_dev.h>
#include <sdrplay_api_rsp1a.h>
#include <sdrplay_api_rsp2.h>
#include <sdrplay_api_rspDuo.h>
#include <sdrplay_api_rspDx.h>
#include <sdrplay_api_rx_channel.h>
#include <sdrplay_api_tuner.h>

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

_Static_assert(sizeof(sdrplay_api_DeviceT) == 96, "DeviceT ABI drift");
_Static_assert(sizeof(sdrplay_api_DevParamsT) == 64, "DevParamsT ABI drift");
_Static_assert(sizeof(sdrplay_api_RxChannelParamsT) == 144,
               "RxChannelParamsT ABI drift");
_Static_assert(sizeof(sdrplay_api_Rsp1aParamsT) == 2, "Rsp1aParamsT ABI drift");
_Static_assert(sizeof(sdrplay_api_Rsp2ParamsT) == 1, "Rsp2ParamsT ABI drift");
_Static_assert(sizeof(sdrplay_api_RspDxParamsT) == 12, "RspDxParamsT ABI drift");
_Static_assert(sizeof(sdrplay_api_Rsp2TunerParamsT) == 16,
               "Rsp2TunerParamsT ABI drift");
_Static_assert(sizeof(sdrplay_api_RspDuoTunerParamsT) == 16,
               "RspDuoTunerParamsT ABI drift");

int main(void)
{
    sdrplay_api_DevParamsT device = {0};
    sdrplay_api_RxChannelParamsT channel = {0};
    device.rsp1aParams.rfNotchEnable = 1u;
    device.rsp2Params.extRefOutputEn = 1u;
    device.rspDxParams.hdrEnable = 1u;
    device.rspDxParams.antennaSel = sdrplay_api_RspDx_ANTENNA_C;
    channel.tunerParams.loMode = sdrplay_api_LO_144MHz;
    channel.ctrlParams.adsbMode = sdrplay_api_ADSB_NO_DECIMATION_BANDPASS_3MHZ;
    channel.rsp1aTunerParams.biasTEnable = 1u;
    channel.rsp2TunerParams.amPortSel = sdrplay_api_Rsp2_AMPORT_1;
    channel.rsp2TunerParams.antennaSel = sdrplay_api_Rsp2_ANTENNA_B;
    channel.rspDxTunerParams.hdrBw = sdrplay_api_RspDx_HDRMODE_BW_1_700;
    if (device.rsp1aParams.rfNotchEnable != 1u ||
        device.rsp2Params.extRefOutputEn != 1u ||
        device.rspDxParams.antennaSel != sdrplay_api_RspDx_ANTENNA_C ||
        channel.tunerParams.loMode != sdrplay_api_LO_144MHz ||
        channel.ctrlParams.adsbMode !=
            sdrplay_api_ADSB_NO_DECIMATION_BANDPASS_3MHZ ||
        channel.rsp2TunerParams.antennaSel != sdrplay_api_Rsp2_ANTENNA_B ||
        channel.rspDxTunerParams.hdrBw != sdrplay_api_RspDx_HDRMODE_BW_1_700)
        return 1;
    sdrplay_api_Open_t open_api = sdrplay_api_Open;
    sdrplay_api_Update_t update_api = sdrplay_api_Update;
    assert(open_api != NULL && update_api != NULL);
    assert(SDRPLAY_MAX_TUNERS_PER_DEVICE == 2u);
    assert(SDRPLAY_RSPduo_ID == 3u && SDRPLAY_RSPdxR2_ID == 7u);
    puts("SDRPLAY_HEADER_COMPAT_OK");
    return 0;
}
