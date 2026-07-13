/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include <stdio.h>

#ifdef OPENRSP_USE_OFFICIAL_HEADER
#include <sdrplay_api.h>
#else
#include "sdrplay_api_compat.h"
#endif

#ifndef OPENRSP_USE_OFFICIAL_HEADER
_Static_assert(sizeof(sdrplay_api_DeviceT) == 96, "DeviceT ABI drift");
_Static_assert(sizeof(sdrplay_api_DevParamsT) == 64, "DevParamsT ABI drift");
_Static_assert(sizeof(sdrplay_api_RxChannelParamsT) == 144, "RxChannelParamsT ABI drift");
_Static_assert(sizeof(sdrplay_api_TunerParamsT) == 72, "TunerParamsT ABI drift");
_Static_assert(sizeof(sdrplay_api_ControlParamsT) == 32, "ControlParamsT ABI drift");
_Static_assert(sizeof(sdrplay_api_CallbackFnsT) == 24, "CallbackFnsT ABI drift");
_Static_assert(sizeof(sdrplay_api_StreamCbParamsT) == 20, "StreamCbParamsT ABI drift");
_Static_assert(sizeof(sdrplay_api_EventParamsT) == 16, "EventParamsT ABI drift");
_Static_assert(_Alignof(sdrplay_api_EventParamsT) == 8, "EventParamsT alignment drift");
_Static_assert(sizeof(sdrplay_api_ErrorInfoT) == 1540, "ErrorInfoT ABI drift");
#endif

#define TYPE(name) printf("TYPE %-38s size=%zu align=%zu\n", #name, sizeof(name), _Alignof(name))
#define FIELD(type, field) printf("FIELD %-37s offset=%zu size=%zu\n", #type "." #field, \
                                  offsetof(type, field), sizeof(((type *)0)->field))

int main(void)
{
    TYPE(sdrplay_api_DeviceT);
    FIELD(sdrplay_api_DeviceT, hwVer);
    FIELD(sdrplay_api_DeviceT, tuner);
    FIELD(sdrplay_api_DeviceT, rspDuoMode);
    FIELD(sdrplay_api_DeviceT, valid);
    FIELD(sdrplay_api_DeviceT, rspDuoSampleFreq);
    FIELD(sdrplay_api_DeviceT, dev);

    TYPE(sdrplay_api_DevParamsT);
    FIELD(sdrplay_api_DevParamsT, ppm);
    FIELD(sdrplay_api_DevParamsT, fsFreq);
    FIELD(sdrplay_api_DevParamsT, mode);
    FIELD(sdrplay_api_DevParamsT, samplesPerPkt);
    FIELD(sdrplay_api_DevParamsT, rspDuoParams);
    FIELD(sdrplay_api_DevParamsT, rspDxParams);

    TYPE(sdrplay_api_Rsp1aParamsT);
    FIELD(sdrplay_api_Rsp1aParamsT, rfNotchEnable);
    FIELD(sdrplay_api_Rsp1aParamsT, rfDabNotchEnable);
    TYPE(sdrplay_api_Rsp2ParamsT);
    FIELD(sdrplay_api_Rsp2ParamsT, extRefOutputEn);
    TYPE(sdrplay_api_RspDuoParamsT);
    FIELD(sdrplay_api_RspDuoParamsT, extRefOutputEn);
    TYPE(sdrplay_api_RspDxParamsT);
    FIELD(sdrplay_api_RspDxParamsT, hdrEnable);
    FIELD(sdrplay_api_RspDxParamsT, biasTEnable);
    FIELD(sdrplay_api_RspDxParamsT, antennaSel);
    FIELD(sdrplay_api_RspDxParamsT, rfNotchEnable);
    FIELD(sdrplay_api_RspDxParamsT, rfDabNotchEnable);

    TYPE(sdrplay_api_RxChannelParamsT);
    FIELD(sdrplay_api_RxChannelParamsT, tunerParams);
    FIELD(sdrplay_api_RxChannelParamsT, ctrlParams);
    FIELD(sdrplay_api_RxChannelParamsT, rspDuoTunerParams);
    FIELD(sdrplay_api_RxChannelParamsT, rspDxTunerParams);

    TYPE(sdrplay_api_Rsp1aTunerParamsT);
    FIELD(sdrplay_api_Rsp1aTunerParamsT, biasTEnable);
    TYPE(sdrplay_api_Rsp2TunerParamsT);
    FIELD(sdrplay_api_Rsp2TunerParamsT, biasTEnable);
    FIELD(sdrplay_api_Rsp2TunerParamsT, amPortSel);
    FIELD(sdrplay_api_Rsp2TunerParamsT, antennaSel);
    FIELD(sdrplay_api_Rsp2TunerParamsT, rfNotchEnable);
    TYPE(sdrplay_api_RspDuoTunerParamsT);
    FIELD(sdrplay_api_RspDuoTunerParamsT, biasTEnable);
    FIELD(sdrplay_api_RspDuoTunerParamsT, tuner1AmPortSel);
    FIELD(sdrplay_api_RspDuoTunerParamsT, tuner1AmNotchEnable);
    FIELD(sdrplay_api_RspDuoTunerParamsT, rfNotchEnable);
    FIELD(sdrplay_api_RspDuoTunerParamsT, rfDabNotchEnable);
    FIELD(sdrplay_api_RspDuoTunerParamsT, resetSlaveFlags);
    TYPE(sdrplay_api_RspDxTunerParamsT);
    FIELD(sdrplay_api_RspDxTunerParamsT, hdrBw);

    TYPE(sdrplay_api_TunerParamsT);
    FIELD(sdrplay_api_TunerParamsT, gain);
    FIELD(sdrplay_api_TunerParamsT, rfFreq);
    TYPE(sdrplay_api_ControlParamsT);
    FIELD(sdrplay_api_ControlParamsT, agc);
    TYPE(sdrplay_api_CallbackFnsT);
    TYPE(sdrplay_api_StreamCbParamsT);
    TYPE(sdrplay_api_EventParamsT);
    TYPE(sdrplay_api_ErrorInfoT);
    return 0;
}
