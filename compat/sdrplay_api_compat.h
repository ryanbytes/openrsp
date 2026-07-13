/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OPENRSP_SDRPLAY_API_COMPAT_H
#define OPENRSP_SDRPLAY_API_COMPAT_H

#include <stdint.h>

#if defined(_WIN32)
#if defined(OPENRSP_SDRPLAY_API_BUILD)
#define OPENRSP_API __declspec(dllexport)
#else
#define OPENRSP_API __declspec(dllimport)
#endif
#else
#define OPENRSP_API __attribute__((visibility("default")))
#endif

#define SDRPLAY_API_VERSION 3.15f
#define SDRPLAY_MAX_DEVICES 16u
#define SDRPLAY_MAX_TUNERS_PER_DEVICE 2u
#define SDRPLAY_MAX_SER_NO_LEN 64u
#define SDRPLAY_MAX_ROOT_NM_LEN 32u

#define SDRPLAY_RSP1_ID 1u
#define SDRPLAY_RSP1A_ID 255u
#define SDRPLAY_RSP2_ID 2u
#define SDRPLAY_RSPduo_ID 3u
#define SDRPLAY_RSPdx_ID 4u
#define SDRPLAY_RSP1B_ID 6u
#define SDRPLAY_RSPdxR2_ID 7u
#define MAX_BB_GR 59

#define RSPIA_NUM_LNA_STATES 10
#define RSPIA_NUM_LNA_STATES_AM 7
#define RSPIA_NUM_LNA_STATES_LBAND 9
#define RSPII_NUM_LNA_STATES 9
#define RSPII_NUM_LNA_STATES_AMPORT 5
#define RSPII_NUM_LNA_STATES_420MHZ 6
#define RSPDUO_NUM_LNA_STATES 10
#define RSPDUO_NUM_LNA_STATES_AMPORT 5
#define RSPDUO_NUM_LNA_STATES_AM 7
#define RSPDUO_NUM_LNA_STATES_LBAND 9
#define RSPDX_NUM_LNA_STATES 28
#define RSPDX_NUM_LNA_STATES_AMPORT2_0_12 19
#define RSPDX_NUM_LNA_STATES_AMPORT2_12_50 20
#define RSPDX_NUM_LNA_STATES_AMPORT2_50_60 25
#define RSPDX_NUM_LNA_STATES_VHF_BAND3 27
#define RSPDX_NUM_LNA_STATES_420MHZ 21
#define RSPDX_NUM_LNA_STATES_LBAND 19
#define RSPDX_NUM_LNA_STATES_DX 22

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
typedef enum {
    sdrplay_api_LO_Undefined = 0, sdrplay_api_LO_Auto = 1,
    sdrplay_api_LO_120MHz = 2, sdrplay_api_LO_144MHz = 3,
    sdrplay_api_LO_168MHz = 4
} sdrplay_api_LoModeT;
typedef enum { sdrplay_api_EXTENDED_MIN_GR = 0, sdrplay_api_NORMAL_MIN_GR = 20 } sdrplay_api_MinGainReductionT;
typedef enum {
    sdrplay_api_AGC_DISABLE = 0, sdrplay_api_AGC_100HZ = 1, sdrplay_api_AGC_50HZ = 2,
    sdrplay_api_AGC_5HZ = 3, sdrplay_api_AGC_CTRL_EN = 4
} sdrplay_api_AgcControlT;
typedef enum {
    sdrplay_api_ADSB_DECIMATION = 0,
    sdrplay_api_ADSB_NO_DECIMATION_LOWPASS = 1,
    sdrplay_api_ADSB_NO_DECIMATION_BANDPASS_2MHZ = 2,
    sdrplay_api_ADSB_NO_DECIMATION_BANDPASS_3MHZ = 3
} sdrplay_api_AdsbModeT;

typedef struct { double fsHz; unsigned char syncUpdate; unsigned char reCal; } sdrplay_api_FsFreqT;
typedef struct { unsigned int sampleNum; unsigned int period; } sdrplay_api_SyncUpdateT;
typedef struct { unsigned char resetGainUpdate, resetRfUpdate, resetFsUpdate; } sdrplay_api_ResetFlagsT;
typedef struct { int extRefOutputEn; } sdrplay_api_RspDuoParamsT;
typedef struct {
    unsigned char rfNotchEnable;
    unsigned char rfDabNotchEnable;
} sdrplay_api_Rsp1aParamsT;
typedef struct { unsigned char extRefOutputEn; } sdrplay_api_Rsp2ParamsT;
typedef enum {
    sdrplay_api_RspDx_ANTENNA_A = 0,
    sdrplay_api_RspDx_ANTENNA_B = 1,
    sdrplay_api_RspDx_ANTENNA_C = 2
} sdrplay_api_RspDx_AntennaSelectT;
typedef enum {
    sdrplay_api_RspDx_HDRMODE_BW_0_200 = 0,
    sdrplay_api_RspDx_HDRMODE_BW_0_500 = 1,
    sdrplay_api_RspDx_HDRMODE_BW_1_200 = 2,
    sdrplay_api_RspDx_HDRMODE_BW_1_700 = 3
} sdrplay_api_RspDx_HdrModeBwT;
typedef struct {
    unsigned char hdrEnable;
    unsigned char biasTEnable;
    sdrplay_api_RspDx_AntennaSelectT antennaSel;
    unsigned char rfNotchEnable;
    unsigned char rfDabNotchEnable;
} sdrplay_api_RspDxParamsT;
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
typedef struct { unsigned char biasTEnable; } sdrplay_api_Rsp1aTunerParamsT;
typedef enum {
    sdrplay_api_Rsp2_ANTENNA_A = 5,
    sdrplay_api_Rsp2_ANTENNA_B = 6
} sdrplay_api_Rsp2_AntennaSelectT;
typedef enum {
    sdrplay_api_Rsp2_AMPORT_1 = 1,
    sdrplay_api_Rsp2_AMPORT_2 = 0
} sdrplay_api_Rsp2_AmPortSelectT;
typedef struct {
    unsigned char biasTEnable;
    sdrplay_api_Rsp2_AmPortSelectT amPortSel;
    sdrplay_api_Rsp2_AntennaSelectT antennaSel;
    unsigned char rfNotchEnable;
} sdrplay_api_Rsp2TunerParamsT;
typedef enum { sdrplay_api_RspDuo_AMPORT_2 = 0, sdrplay_api_RspDuo_AMPORT_1 = 1 } sdrplay_api_RspDuo_AmPortSelectT;
typedef struct { unsigned char resetGainUpdate, resetRfUpdate; } sdrplay_api_RspDuo_ResetSlaveFlagsT;
typedef struct {
    unsigned char biasTEnable; sdrplay_api_RspDuo_AmPortSelectT tuner1AmPortSel;
    unsigned char tuner1AmNotchEnable, rfNotchEnable, rfDabNotchEnable;
    sdrplay_api_RspDuo_ResetSlaveFlagsT resetSlaveFlags;
} sdrplay_api_RspDuoTunerParamsT;
typedef struct { sdrplay_api_RspDx_HdrModeBwT hdrBw; } sdrplay_api_RspDxTunerParamsT;
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
typedef enum { sdrplay_api_Overload_Detected = 0,
               sdrplay_api_Overload_Corrected = 1 } sdrplay_api_PowerOverloadCbEventIdT;
typedef enum { sdrplay_api_MasterInitialised = 0, sdrplay_api_SlaveAttached = 1,
               sdrplay_api_SlaveDetached = 2, sdrplay_api_SlaveInitialised = 3,
               sdrplay_api_SlaveUninitialised = 4,
               sdrplay_api_MasterDllDisappeared = 5,
               sdrplay_api_SlaveDllDisappeared = 6 } sdrplay_api_RspDuoModeCbEventIdT;
typedef struct { unsigned int gRdB, lnaGRdB; double currGain; } sdrplay_api_GainCbParamT;
typedef struct { sdrplay_api_PowerOverloadCbEventIdT powerOverloadChangeType; }
    sdrplay_api_PowerOverloadCbParamT;
typedef struct { sdrplay_api_RspDuoModeCbEventIdT modeChangeType; }
    sdrplay_api_RspDuoModeCbParamT;
typedef union {
    sdrplay_api_GainCbParamT gainParams;
    sdrplay_api_PowerOverloadCbParamT powerOverloadParams;
    sdrplay_api_RspDuoModeCbParamT rspDuoModeParams;
} sdrplay_api_EventParamsT;
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
#define sdrplay_api_Update_None 0x00000000u
#define sdrplay_api_Update_Dev_Fs 0x00000001u
#define sdrplay_api_Update_Dev_Ppm 0x00000002u
#define sdrplay_api_Update_Dev_SyncUpdate 0x00000004u
#define sdrplay_api_Update_Dev_ResetFlags 0x00000008u
#define sdrplay_api_Update_Rsp1a_BiasTControl 0x00000010u
#define sdrplay_api_Update_Rsp1a_RfNotchControl 0x00000020u
#define sdrplay_api_Update_Rsp1a_RfDabNotchControl 0x00000040u
#define sdrplay_api_Update_Rsp2_BiasTControl 0x00000080u
#define sdrplay_api_Update_Rsp2_AmPortSelect 0x00000100u
#define sdrplay_api_Update_Rsp2_AntennaControl 0x00000200u
#define sdrplay_api_Update_Rsp2_RfNotchControl 0x00000400u
#define sdrplay_api_Update_Rsp2_ExtRefControl 0x00000800u
#define sdrplay_api_Update_RspDuo_ExtRefControl 0x00001000u
#define sdrplay_api_Update_Master_Spare_1 0x00002000u
#define sdrplay_api_Update_Master_Spare_2 0x00004000u
#define sdrplay_api_Update_Tuner_Gr 0x00008000u
#define sdrplay_api_Update_Tuner_GrLimits 0x00010000u
#define sdrplay_api_Update_Tuner_Frf 0x00020000u
#define sdrplay_api_Update_Tuner_BwType 0x00040000u
#define sdrplay_api_Update_Tuner_IfType 0x00080000u
#define sdrplay_api_Update_Tuner_DcOffset 0x00100000u
#define sdrplay_api_Update_Tuner_LoMode 0x00200000u
#define sdrplay_api_Update_Ctrl_DCoffsetIQimbalance 0x00400000u
#define sdrplay_api_Update_Ctrl_Decimation 0x00800000u
#define sdrplay_api_Update_Ctrl_Agc 0x01000000u
#define sdrplay_api_Update_Ctrl_AdsbMode 0x02000000u
#define sdrplay_api_Update_Ctrl_OverloadMsgAck 0x04000000u
#define sdrplay_api_Update_RspDuo_BiasTControl 0x08000000u
#define sdrplay_api_Update_RspDuo_AmPortSelect 0x10000000u
#define sdrplay_api_Update_RspDuo_Tuner1AmNotchControl 0x20000000u
#define sdrplay_api_Update_RspDuo_RfNotchControl 0x40000000u
#define sdrplay_api_Update_RspDuo_RfDabNotchControl 0x80000000u

#define sdrplay_api_Update_Ext1_None 0x00000000u
#define sdrplay_api_Update_RspDx_HdrEnable 0x00000001u
#define sdrplay_api_Update_RspDx_BiasTControl 0x00000002u
#define sdrplay_api_Update_RspDx_AntennaControl 0x00000004u
#define sdrplay_api_Update_RspDx_RfNotchControl 0x00000008u
#define sdrplay_api_Update_RspDx_RfDabNotchControl 0x00000010u
#define sdrplay_api_Update_RspDx_HdrBw 0x00000020u
#define sdrplay_api_Update_RspDuo_ResetSlaveFlags 0x00000040u

typedef enum {
    sdrplay_api_DbgLvl_Disable = 0, sdrplay_api_DbgLvl_Verbose = 1,
    sdrplay_api_DbgLvl_Warning = 2, sdrplay_api_DbgLvl_Error = 3,
    sdrplay_api_DbgLvl_Message = 4
} sdrplay_api_DbgLvl_t;

typedef struct {
    char file[256];
    char function[256];
    int line;
    char message[1024];
} sdrplay_api_ErrorInfoT;

typedef sdrplay_api_ErrT (*sdrplay_api_Open_t)(void);
typedef sdrplay_api_ErrT (*sdrplay_api_Close_t)(void);
typedef sdrplay_api_ErrT (*sdrplay_api_ApiVersion_t)(float *apiVer);
typedef sdrplay_api_ErrT (*sdrplay_api_LockDeviceApi_t)(void);
typedef sdrplay_api_ErrT (*sdrplay_api_UnlockDeviceApi_t)(void);
typedef sdrplay_api_ErrT (*sdrplay_api_GetDevices_t)(sdrplay_api_DeviceT *devices,
                                                     unsigned int *numDevs,
                                                     unsigned int maxDevs);
typedef sdrplay_api_ErrT (*sdrplay_api_SelectDevice_t)(sdrplay_api_DeviceT *device);
typedef sdrplay_api_ErrT (*sdrplay_api_ReleaseDevice_t)(sdrplay_api_DeviceT *device);
typedef const char *(*sdrplay_api_GetErrorString_t)(sdrplay_api_ErrT err);
typedef sdrplay_api_ErrorInfoT *(*sdrplay_api_GetLastError_t)(sdrplay_api_DeviceT *device);
typedef sdrplay_api_ErrorInfoT *(*sdrplay_api_GetLastErrorByType_t)(
    sdrplay_api_DeviceT *device, int type, unsigned long long *time);
typedef sdrplay_api_ErrT (*sdrplay_api_DisableHeartbeat_t)(void);
typedef sdrplay_api_ErrT (*sdrplay_api_DebugEnable_t)(HANDLE dev,
                                                      sdrplay_api_DbgLvl_t level);
typedef sdrplay_api_ErrT (*sdrplay_api_GetDeviceParams_t)(
    HANDLE dev, sdrplay_api_DeviceParamsT **params);
typedef sdrplay_api_ErrT (*sdrplay_api_Init_t)(HANDLE dev,
                                               sdrplay_api_CallbackFnsT *callbacks,
                                               void *context);
typedef sdrplay_api_ErrT (*sdrplay_api_Uninit_t)(HANDLE dev);
typedef sdrplay_api_ErrT (*sdrplay_api_Update_t)(
    HANDLE dev, sdrplay_api_TunerSelectT tuner,
    sdrplay_api_ReasonForUpdateT reason,
    sdrplay_api_ReasonForUpdateExtension1T extension);
typedef sdrplay_api_ErrT (*sdrplay_api_SwapRspDuoActiveTuner_t)(
    HANDLE dev, sdrplay_api_TunerSelectT *current_tuner,
    sdrplay_api_RspDuo_AmPortSelectT tuner1_am_port);
/* Kept source-compatible with SDRplay's 3.15 public typedef, whose function
 * pointer omits the HANDLE present in the exported function declaration. */
typedef sdrplay_api_ErrT (*sdrplay_api_SwapRspDuoDualTunerModeSampleRate_t)(
    double *current_sample_rate, double new_sample_rate);
typedef sdrplay_api_ErrT (*sdrplay_api_SwapRspDuoMode_t)(
    sdrplay_api_DeviceT *current_device, sdrplay_api_DeviceParamsT **device_params,
    sdrplay_api_RspDuoModeT mode, double sample_rate, sdrplay_api_TunerSelectT tuner,
    sdrplay_api_Bw_MHzT bandwidth, sdrplay_api_If_kHzT if_type,
    sdrplay_api_RspDuo_AmPortSelectT tuner1_am_port);

#ifdef __cplusplus
extern "C" {
#endif

OPENRSP_API sdrplay_api_ErrT sdrplay_api_Open(void);
OPENRSP_API sdrplay_api_ErrT sdrplay_api_Close(void);
OPENRSP_API sdrplay_api_ErrT sdrplay_api_ApiVersion(float *apiVer);
OPENRSP_API sdrplay_api_ErrT sdrplay_api_LockDeviceApi(void);
OPENRSP_API sdrplay_api_ErrT sdrplay_api_UnlockDeviceApi(void);
OPENRSP_API sdrplay_api_ErrT sdrplay_api_GetDevices(sdrplay_api_DeviceT *devices,
                                                     unsigned int *numDevs,
                                                     unsigned int maxDevs);
OPENRSP_API const char *sdrplay_api_GetErrorString(sdrplay_api_ErrT err);
OPENRSP_API sdrplay_api_ErrorInfoT *sdrplay_api_GetLastError(sdrplay_api_DeviceT *device);
OPENRSP_API sdrplay_api_ErrorInfoT *sdrplay_api_GetLastErrorByType(
    sdrplay_api_DeviceT *device, int type, unsigned long long *time);
OPENRSP_API sdrplay_api_ErrT sdrplay_api_DisableHeartbeat(void);
OPENRSP_API sdrplay_api_ErrT sdrplay_api_DebugEnable(HANDLE dev, sdrplay_api_DbgLvl_t level);
OPENRSP_API sdrplay_api_ErrT sdrplay_api_SelectDevice(sdrplay_api_DeviceT *device);
OPENRSP_API sdrplay_api_ErrT sdrplay_api_ReleaseDevice(sdrplay_api_DeviceT *device);
OPENRSP_API sdrplay_api_ErrT sdrplay_api_GetDeviceParams(HANDLE dev, sdrplay_api_DeviceParamsT **params);
OPENRSP_API sdrplay_api_ErrT sdrplay_api_Init(HANDLE dev, sdrplay_api_CallbackFnsT *callbacks, void *context);
OPENRSP_API sdrplay_api_ErrT sdrplay_api_Uninit(HANDLE dev);
OPENRSP_API sdrplay_api_ErrT sdrplay_api_Update(HANDLE dev, sdrplay_api_TunerSelectT tuner,
                                                sdrplay_api_ReasonForUpdateT reason,
                                                sdrplay_api_ReasonForUpdateExtension1T extension);
OPENRSP_API sdrplay_api_ErrT sdrplay_api_SwapRspDuoActiveTuner(
    HANDLE dev, sdrplay_api_TunerSelectT *current_tuner,
    sdrplay_api_RspDuo_AmPortSelectT tuner1_am_port);
OPENRSP_API sdrplay_api_ErrT sdrplay_api_SwapRspDuoDualTunerModeSampleRate(
    HANDLE dev, double *current_sample_rate, double new_sample_rate);
OPENRSP_API sdrplay_api_ErrT sdrplay_api_SwapRspDuoMode(
    sdrplay_api_DeviceT *current_device, sdrplay_api_DeviceParamsT **device_params,
    sdrplay_api_RspDuoModeT mode, double sample_rate, sdrplay_api_TunerSelectT tuner,
    sdrplay_api_Bw_MHzT bandwidth, sdrplay_api_If_kHzT if_type,
    sdrplay_api_RspDuo_AmPortSelectT tuner1_am_port);

#ifdef __cplusplus
}
#endif

#endif
