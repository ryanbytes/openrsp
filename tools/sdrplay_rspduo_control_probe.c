/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <sdrplay_api.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    atomic_ullong samples;
    atomic_uint callbacks;
    atomic_uint resets;
} control_metrics;

static void stream_a(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params,
                     unsigned int samples, unsigned int reset, void *opaque)
{
    (void)xi;
    (void)xq;
    (void)params;
    control_metrics *metrics = opaque;
    atomic_fetch_add(&metrics->samples, samples);
    atomic_fetch_add(&metrics->callbacks, 1u);
    if (reset != 0u) atomic_fetch_add(&metrics->resets, 1u);
}

static void event_callback(sdrplay_api_EventT event, sdrplay_api_TunerSelectT tuner,
                           sdrplay_api_EventParamsT *params, void *opaque)
{
    (void)params;
    (void)opaque;
    fprintf(stderr, "CONTROL_EVENT event=%d tuner=%d\n", event, tuner);
}

static void delay_ms(long milliseconds)
{
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000L
    };
    while (nanosleep(&delay, &delay) != 0) {}
}

static long environment_delay_ms(const char *name, long fallback)
{
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') return fallback;
    char *end = NULL;
    long milliseconds = strtol(value, &end, 10);
    return end != value && *end == '\0' && milliseconds >= 0 &&
           milliseconds <= 30000 ? milliseconds : fallback;
}

static int set_control(sdrplay_api_DeviceParamsT *params,
                       sdrplay_api_RxChannelParamsT *channel,
                       const char *control, unsigned int enabled,
                       sdrplay_api_ReasonForUpdateT *reason)
{
    if (strcmp(control, "bias") == 0) {
        channel->rspDuoTunerParams.biasTEnable = (unsigned char)enabled;
        *reason = sdrplay_api_Update_RspDuo_BiasTControl;
    } else if (strcmp(control, "rf-notch") == 0) {
        channel->rspDuoTunerParams.rfNotchEnable = (unsigned char)enabled;
        *reason = sdrplay_api_Update_RspDuo_RfNotchControl;
    } else if (strcmp(control, "dab-notch") == 0) {
        channel->rspDuoTunerParams.rfDabNotchEnable = (unsigned char)enabled;
        *reason = sdrplay_api_Update_RspDuo_RfDabNotchControl;
    } else if (strcmp(control, "extref") == 0) {
        params->devParams->rspDuoParams.extRefOutputEn = (int)enabled;
        *reason = sdrplay_api_Update_RspDuo_ExtRefControl;
    } else if (strcmp(control, "am-port") == 0) {
        channel->rspDuoTunerParams.tuner1AmPortSel = enabled != 0u ?
            sdrplay_api_RspDuo_AMPORT_1 : sdrplay_api_RspDuo_AMPORT_2;
        *reason = sdrplay_api_Update_RspDuo_AmPortSelect;
    } else if (strcmp(control, "am-notch") == 0) {
        channel->rspDuoTunerParams.tuner1AmNotchEnable = (unsigned char)enabled;
        *reason = sdrplay_api_Update_RspDuo_Tuner1AmNotchControl;
    } else {
        return -1;
    }
    return 0;
}

static int cleanup(sdrplay_api_DeviceT *device, int initialized,
                   sdrplay_api_ErrT status)
{
    sdrplay_api_ErrT uninit = initialized ? sdrplay_api_Uninit(device->dev) :
                                           sdrplay_api_Success;
    sdrplay_api_ErrT release = sdrplay_api_ReleaseDevice(device);
    sdrplay_api_ErrT close = sdrplay_api_Close();
    fprintf(stderr, "CONTROL_CLEANUP status=%d uninit=%d release=%d close=%d\n",
            status, uninit, release, close);
    return status == sdrplay_api_Success && uninit == sdrplay_api_Success &&
           release == sdrplay_api_Success && close == sdrplay_api_Success ?
           EXIT_SUCCESS : EXIT_FAILURE;
}

int main(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[2], "A") != 0 && strcmp(argv[2], "B") != 0)) {
        fprintf(stderr,
                "usage: %s bias|rf-notch|dab-notch|extref|am-port|am-notch A|B\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    sdrplay_api_TunerSelectT tuner = strcmp(argv[2], "B") == 0 ?
        sdrplay_api_Tuner_B : sdrplay_api_Tuner_A;
    sdrplay_api_ErrT status = sdrplay_api_Open();
    sdrplay_api_DeviceT devices[SDRPLAY_MAX_DEVICES];
    unsigned int count = 0u;
    if (status == sdrplay_api_Success)
        status = sdrplay_api_GetDevices(devices, &count, SDRPLAY_MAX_DEVICES);
    if (status != sdrplay_api_Success || count == 0u ||
        devices[0].hwVer != SDRPLAY_RSPduo_ID) {
        fprintf(stderr, "CONTROL_DISCOVERY status=%d count=%u\n", status, count);
        if (status == sdrplay_api_Success) (void)sdrplay_api_Close();
        return EXIT_FAILURE;
    }
    devices[0].tuner = tuner;
    devices[0].rspDuoMode = sdrplay_api_RspDuoMode_Single_Tuner;
    status = sdrplay_api_SelectDevice(&devices[0]);
    sdrplay_api_DeviceParamsT *params = NULL;
    if (status == sdrplay_api_Success)
        status = sdrplay_api_GetDeviceParams(devices[0].dev, &params);
    sdrplay_api_RxChannelParamsT *channel = params == NULL ? NULL :
        (tuner == sdrplay_api_Tuner_B ? params->rxChannelB : params->rxChannelA);
    if (status != sdrplay_api_Success || params == NULL ||
        params->devParams == NULL || channel == NULL)
        return cleanup(&devices[0], 0, status);

    params->devParams->fsFreq.fsHz = 2048000.0;
    params->devParams->mode = sdrplay_api_BULK;
    int am_control = strcmp(argv[1], "am-port") == 0 ||
                     strcmp(argv[1], "am-notch") == 0;
    channel->tunerParams.rfFreq.rfHz = am_control ? 10000000.0 : 853862500.0;
    channel->tunerParams.bwType = sdrplay_api_BW_1_536;
    channel->tunerParams.ifType = sdrplay_api_IF_Zero;
    channel->tunerParams.loMode = sdrplay_api_LO_Auto;
    channel->tunerParams.gain.gRdB = 45;
    channel->tunerParams.gain.LNAstate = 2u;
    channel->ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE;
    sdrplay_api_ReasonForUpdateT reason = 0u;
    if (set_control(params, channel, argv[1], 0u, &reason) != 0) {
        fprintf(stderr, "CONTROL_UNKNOWN name=%s\n", argv[1]);
        return cleanup(&devices[0], 0, sdrplay_api_InvalidParam);
    }
    control_metrics metrics = {0};
    sdrplay_api_CallbackFnsT callbacks = {
        .StreamACbFn = stream_a, .EventCbFn = event_callback
    };
    status = sdrplay_api_Init(devices[0].dev, &callbacks, &metrics);
    if (status != sdrplay_api_Success) return cleanup(&devices[0], 0, status);
    delay_ms(environment_delay_ms("OPENRSP_CONTROL_PROBE_SETTLE_MS", 500));
    (void)set_control(params, channel, argv[1], 1u, &reason);
    sdrplay_api_ErrT enable = sdrplay_api_Update(devices[0].dev, tuner, reason, 0u);
    delay_ms(environment_delay_ms("OPENRSP_CONTROL_PROBE_UPDATE_HOLD_MS", 500));
    (void)set_control(params, channel, argv[1], 0u, &reason);
    sdrplay_api_ErrT disable = sdrplay_api_Update(devices[0].dev, tuner, reason, 0u);
    delay_ms(500);
    fprintf(stderr,
            "CONTROL_RESULT name=%s tuner=%s enable=%d disable=%d samples=%llu "
            "callbacks=%u resets=%u\n",
            argv[1], argv[2], enable, disable,
            atomic_load(&metrics.samples), atomic_load(&metrics.callbacks),
            atomic_load(&metrics.resets));
    int tuner_b_am_control = tuner == sdrplay_api_Tuner_B &&
        (strcmp(argv[1], "am-port") == 0 || strcmp(argv[1], "am-notch") == 0);
    sdrplay_api_ErrT expected = tuner_b_am_control ? sdrplay_api_OutOfRange :
        (strcmp(argv[1], "bias") == 0 && tuner == sdrplay_api_Tuner_A ?
         sdrplay_api_InvalidParam : sdrplay_api_Success);
    if (enable != expected || disable != expected ||
        atomic_load(&metrics.samples) == 0u) status = sdrplay_api_Fail;
    return cleanup(&devices[0], 1, status);
}
