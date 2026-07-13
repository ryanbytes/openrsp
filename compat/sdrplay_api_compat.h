/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OPENRSP_SDRPLAY_API_COMPAT_H
#define OPENRSP_SDRPLAY_API_COMPAT_H

#include <stdint.h>

#if defined(_WIN32)
#define OPENRSP_API __declspec(dllexport)
#else
#define OPENRSP_API __attribute__((visibility("default")))
#endif

#define SDRPLAY_API_VERSION 3.15f
#define SDRPLAY_MAX_DEVICES 16u
#define SDRPLAY_MAX_SER_NO_LEN 64u

typedef void *HANDLE;

typedef enum {
    sdrplay_api_Success = 0,
    sdrplay_api_Fail = 1,
    sdrplay_api_InvalidParam = 2,
    sdrplay_api_OutOfRange = 3,
    sdrplay_api_GainUpdateError = 4,
    sdrplay_api_RfUpdateError = 5,
    sdrplay_api_FsUpdateError = 6,
    sdrplay_api_HwError = 7,
    sdrplay_api_AliasingError = 8,
    sdrplay_api_AlreadyInitialised = 9,
    sdrplay_api_NotInitialised = 10,
    sdrplay_api_NotEnabled = 11,
    sdrplay_api_HwVerError = 12,
    sdrplay_api_OutOfMemError = 13,
    sdrplay_api_ServiceNotResponding = 14,
    sdrplay_api_StartPending = 15,
    sdrplay_api_StopPending = 16,
    sdrplay_api_InvalidMode = 17,
    sdrplay_api_FailedVerification1 = 18,
    sdrplay_api_FailedVerification2 = 19,
    sdrplay_api_FailedVerification3 = 20,
    sdrplay_api_FailedVerification4 = 21,
    sdrplay_api_FailedVerification5 = 22,
    sdrplay_api_FailedVerification6 = 23,
    sdrplay_api_InvalidServiceVersion = 24
} sdrplay_api_ErrT;

typedef enum {
    sdrplay_api_Tuner_Neither = 0,
    sdrplay_api_Tuner_A = 1,
    sdrplay_api_Tuner_B = 2,
    sdrplay_api_Tuner_Both = 3
} sdrplay_api_TunerSelectT;

typedef enum {
    sdrplay_api_RspDuoMode_Unknown = 0,
    sdrplay_api_RspDuoMode_Single_Tuner = 1,
    sdrplay_api_RspDuoMode_Dual_Tuner = 2,
    sdrplay_api_RspDuoMode_Master = 4,
    sdrplay_api_RspDuoMode_Slave = 8
} sdrplay_api_RspDuoModeT;

typedef struct {
    char SerNo[SDRPLAY_MAX_SER_NO_LEN];
    unsigned char hwVer;
    sdrplay_api_TunerSelectT tuner;
    sdrplay_api_RspDuoModeT rspDuoMode;
    unsigned char valid;
    double rspDuoSampleFreq;
    HANDLE dev;
} sdrplay_api_DeviceT;

typedef enum { sdrplay_api_ISOCH = 0, sdrplay_api_BULK = 1 } sdrplay_api_TransferModeT;
typedef enum {
    sdrplay_api_BW_Undefined = 0, sdrplay_api_BW_0_200 = 200, sdrplay_api_BW_0_300 = 300,
    sdrplay_api_BW_0_600 = 600, sdrplay_api_BW_1_536 = 1536, sdrplay_api_BW_5_000 = 5000,
    sdrplay_api_BW_6_000 = 6000, sdrplay_api_BW_7_000 = 7000, sdrplay_api_BW_8_000 = 8000
} sdrplay_api_Bw_MHzT;
typedef enum {
    sdrplay_api_IF_Undefined = -1, sdrplay_api_IF_Zero = 0, sdrplay_api_IF_0_450 = 450,
    sdrplay_api_IF_1_620 = 1620, sdrplay_api_IF_2_048 = 2048
} sdrplay_api_If_kHzT;
typedef enum { sdrplay_api_LO_Undefined = 0, sdrplay_api_LO_Auto = 1 } sdrplay_api_LoModeT;
typedef enum { sdrplay_api_EXTENDED_MIN_GR = 0, sdrplay_api_NORMAL_MIN_GR = 20 } sdrplay_api_MinGainReductionT;
typedef enum {
    sdrplay_api_AGC_DISABLE = 0, sdrplay_api_AGC_100HZ = 1, sdrplay_api_AGC_50HZ = 2,
    sdrplay_api_AGC_5HZ = 3, sdrplay_api_AGC_CTRL_EN = 4
} sdrplay_api_AgcControlT;
typedef enum { sdrplay_api_ADSB_DECIMATION = 0 } sdrplay_api_AdsbModeT;

typedef struct { double fsHz; unsigned char syncUpdate; unsigned char reCal; } sdrplay_api_FsFreqT;
typedef struct { unsigned int sampleNum; unsigned int period; } sdrplay_api_SyncUpdateT;
typedef struct { unsigned char resetGainUpdate, resetRfUpdate, resetFsUpdate; } sdrplay_api_ResetFlagsT;
typedef struct { int extRefOutputEn; } sdrplay_api_RspDuoParamsT;
typedef struct { unsigned char bytes[2]; } sdrplay_api_Rsp1aParamsT;
typedef struct { unsigned char bytes[1]; } sdrplay_api_Rsp2ParamsT;
typedef struct { unsigned int words[3]; } sdrplay_api_RspDxParamsT;
typedef struct {
    double ppm;
    sdrplay_api_FsFreqT fsFreq;
    sdrplay_api_SyncUpdateT syncUpdate;
    sdrplay_api_ResetFlagsT resetFlags;
    sdrplay_api_TransferModeT mode;
    unsigned int samplesPerPkt;
    sdrplay_api_Rsp1aParamsT rsp1aParams;
    sdrplay_api_Rsp2ParamsT rsp2Params;
    sdrplay_api_RspDuoParamsT rspDuoParams;
    sdrplay_api_RspDxParamsT rspDxParams;
} sdrplay_api_DevParamsT;

typedef struct { float curr, max, min; } sdrplay_api_GainValuesT;
typedef struct {
    int gRdB; unsigned char LNAstate; unsigned char syncUpdate;
    sdrplay_api_MinGainReductionT minGr; sdrplay_api_GainValuesT gainVals;
} sdrplay_api_GainT;
typedef struct { double rfHz; unsigned char syncUpdate; } sdrplay_api_RfFreqT;
typedef struct { unsigned char dcCal, speedUp; int trackTime, refreshRateTime; } sdrplay_api_DcOffsetTunerT;
typedef struct {
    sdrplay_api_Bw_MHzT bwType; sdrplay_api_If_kHzT ifType; sdrplay_api_LoModeT loMode;
    sdrplay_api_GainT gain; sdrplay_api_RfFreqT rfFreq; sdrplay_api_DcOffsetTunerT dcOffsetTuner;
} sdrplay_api_TunerParamsT;
typedef struct { unsigned char DCenable, IQenable; } sdrplay_api_DcOffsetT;
typedef struct { unsigned char enable, decimationFactor, wideBandSignal; } sdrplay_api_DecimationT;
typedef struct {
    sdrplay_api_AgcControlT enable; int setPoint_dBfs; unsigned short attack_ms, decay_ms;
    unsigned short decay_delay_ms, decay_threshold_dB; int syncUpdate;
} sdrplay_api_AgcT;
typedef struct {
    sdrplay_api_DcOffsetT dcOffset; sdrplay_api_DecimationT decimation;
    sdrplay_api_AgcT agc; sdrplay_api_AdsbModeT adsbMode;
} sdrplay_api_ControlParamsT;
typedef struct { unsigned char bytes[1]; } sdrplay_api_Rsp1aTunerParamsT;
typedef struct { unsigned int words[4]; } sdrplay_api_Rsp2TunerParamsT;
typedef enum { sdrplay_api_RspDuo_AMPORT_2 = 0, sdrplay_api_RspDuo_AMPORT_1 = 1 } sdrplay_api_RspDuo_AmPortSelectT;
typedef struct { unsigned char resetGainUpdate, resetRfUpdate; } sdrplay_api_RspDuo_ResetSlaveFlagsT;
typedef struct {
    unsigned char biasTEnable; sdrplay_api_RspDuo_AmPortSelectT tuner1AmPortSel;
    unsigned char tuner1AmNotchEnable, rfNotchEnable, rfDabNotchEnable;
    sdrplay_api_RspDuo_ResetSlaveFlagsT resetSlaveFlags;
} sdrplay_api_RspDuoTunerParamsT;
typedef struct { unsigned int word; } sdrplay_api_RspDxTunerParamsT;
typedef struct {
    sdrplay_api_TunerParamsT tunerParams; sdrplay_api_ControlParamsT ctrlParams;
    sdrplay_api_Rsp1aTunerParamsT rsp1aTunerParams; sdrplay_api_Rsp2TunerParamsT rsp2TunerParams;
    sdrplay_api_RspDuoTunerParamsT rspDuoTunerParams; sdrplay_api_RspDxTunerParamsT rspDxTunerParams;
} sdrplay_api_RxChannelParamsT;
typedef struct {
    sdrplay_api_DevParamsT *devParams;
    sdrplay_api_RxChannelParamsT *rxChannelA;
    sdrplay_api_RxChannelParamsT *rxChannelB;
} sdrplay_api_DeviceParamsT;

typedef enum { sdrplay_api_GainChange = 0, sdrplay_api_PowerOverloadChange = 1,
               sdrplay_api_DeviceRemoved = 2, sdrplay_api_RspDuoModeChange = 3,
               sdrplay_api_DeviceFailure = 4 } sdrplay_api_EventT;
typedef union { unsigned char bytes[24]; } sdrplay_api_EventParamsT;
typedef struct {
    unsigned int firstSampleNum; int grChanged, rfChanged, fsChanged; unsigned int numSamples;
} sdrplay_api_StreamCbParamsT;
typedef void (*sdrplay_api_StreamCallback_t)(short *, short *, sdrplay_api_StreamCbParamsT *,
                                             unsigned int, unsigned int, void *);
typedef void (*sdrplay_api_EventCallback_t)(sdrplay_api_EventT, sdrplay_api_TunerSelectT,
                                            sdrplay_api_EventParamsT *, void *);
typedef struct {
    sdrplay_api_StreamCallback_t StreamACbFn;
    sdrplay_api_StreamCallback_t StreamBCbFn;
    sdrplay_api_EventCallback_t EventCbFn;
} sdrplay_api_CallbackFnsT;

typedef uint32_t sdrplay_api_ReasonForUpdateT;
typedef uint32_t sdrplay_api_ReasonForUpdateExtension1T;
#define sdrplay_api_Update_Tuner_Gr 0x00008000u
#define sdrplay_api_Update_Tuner_Frf 0x00020000u
#define sdrplay_api_Update_Tuner_BwType 0x00040000u
#define sdrplay_api_Update_Tuner_IfType 0x00080000u
#define sdrplay_api_Update_Dev_Fs 0x00000001u
#define sdrplay_api_Update_Ctrl_Agc 0x01000000u

OPENRSP_API sdrplay_api_ErrT sdrplay_api_Open(void);
OPENRSP_API sdrplay_api_ErrT sdrplay_api_Close(void);
OPENRSP_API sdrplay_api_ErrT sdrplay_api_ApiVersion(float *apiVer);
OPENRSP_API sdrplay_api_ErrT sdrplay_api_LockDeviceApi(void);
OPENRSP_API sdrplay_api_ErrT sdrplay_api_UnlockDeviceApi(void);
OPENRSP_API sdrplay_api_ErrT sdrplay_api_GetDevices(sdrplay_api_DeviceT *devices,
                                                     unsigned int *numDevs,
                                                     unsigned int maxDevs);
OPENRSP_API const char *sdrplay_api_GetErrorString(sdrplay_api_ErrT err);
OPENRSP_API sdrplay_api_ErrT sdrplay_api_DisableHeartbeat(void);
OPENRSP_API sdrplay_api_ErrT sdrplay_api_SelectDevice(sdrplay_api_DeviceT *device);
OPENRSP_API sdrplay_api_ErrT sdrplay_api_ReleaseDevice(sdrplay_api_DeviceT *device);
OPENRSP_API sdrplay_api_ErrT sdrplay_api_GetDeviceParams(HANDLE dev, sdrplay_api_DeviceParamsT **params);
OPENRSP_API sdrplay_api_ErrT sdrplay_api_Init(HANDLE dev, sdrplay_api_CallbackFnsT *callbacks, void *context);
OPENRSP_API sdrplay_api_ErrT sdrplay_api_Uninit(HANDLE dev);
OPENRSP_API sdrplay_api_ErrT sdrplay_api_Update(HANDLE dev, sdrplay_api_TunerSelectT tuner,
                                                sdrplay_api_ReasonForUpdateT reason,
                                                sdrplay_api_ReasonForUpdateExtension1T extension);

#endif
