/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "sdrplay_api_compat.h"

#include <stdatomic.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
    atomic_ullong samples;
    atomic_uint callbacks;
    atomic_uint resets;
    atomic_uint rf_changed;
    atomic_uint fs_changed;
    atomic_uint gr_changed;
    atomic_int peak;
    atomic_ullong power;
} metrics;

static void stream_callback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params,
                            unsigned int num_samples, unsigned int reset, void *opaque)
{
    metrics *result = opaque;
    int peak = atomic_load(&result->peak);
    unsigned long long power = 0;
    for (unsigned int index = 0; index < num_samples; ++index) {
        int i = xi[index] < 0 ? -xi[index] : xi[index];
        int q = xq[index] < 0 ? -xq[index] : xq[index];
        if (i > peak) peak = i;
        if (q > peak) peak = q;
        power += (unsigned long long)i * (unsigned long long)i;
        power += (unsigned long long)q * (unsigned long long)q;
    }
    atomic_store(&result->peak, peak);
    atomic_fetch_add(&result->samples, num_samples);
    atomic_fetch_add(&result->power, power);
    atomic_fetch_add(&result->callbacks, 1u);
    if (reset != 0u) atomic_fetch_add(&result->resets, 1u);
    if (params->rfChanged != 0) atomic_fetch_add(&result->rf_changed, 1u);
    if (params->fsChanged != 0) atomic_fetch_add(&result->fs_changed, 1u);
    if (params->grChanged != 0) atomic_fetch_add(&result->gr_changed, 1u);
    if (params->numSamples != num_samples) abort();
}

int main(int argc, char **argv)
{
    double initial_rf_hz = 100000000.0;
    double updated_rf_hz = 100100000.0;
    long run_seconds = 5;
    double sample_rate_hz = 2048000.0;
    double updated_sample_rate_hz = 0.0;
    unsigned int update_count = 1u;
    if (argc > 1) initial_rf_hz = strtod(argv[1], NULL);
    if (argc > 2) updated_rf_hz = strtod(argv[2], NULL);
    if (argc > 3) run_seconds = strtol(argv[3], NULL, 10);
    if (argc > 4) sample_rate_hz = strtod(argv[4], NULL);
    if (argc > 5) updated_sample_rate_hz = strtod(argv[5], NULL);
    if (argc > 6) update_count = (unsigned int)strtoul(argv[6], NULL, 10);
    if (initial_rf_hz <= 0.0 || updated_rf_hz <= 0.0 || run_seconds < 2 ||
        sample_rate_hz <= 0.0 || update_count == 0u) {
        fputs("usage: sdrplay-compat-stream-test [initial-rf-hz [updated-rf-hz [seconds [sample-rate-hz [updated-sample-rate-hz [update-count]]]]]]\n",
              stderr);
        return EXIT_FAILURE;
    }
    sdrplay_api_DeviceT devices[SDRPLAY_MAX_DEVICES];
    unsigned int count = 0;
    sdrplay_api_DeviceParamsT *params = NULL;
    metrics result = {0};
    sdrplay_api_CallbackFnsT callbacks = {.StreamACbFn = stream_callback};
    sdrplay_api_ErrT open = sdrplay_api_Open();
    sdrplay_api_ErrT list = open == sdrplay_api_Success ?
                             sdrplay_api_GetDevices(devices, &count, SDRPLAY_MAX_DEVICES) : open;
    sdrplay_api_ErrT select = list == sdrplay_api_Success && count != 0u ?
                               sdrplay_api_SelectDevice(&devices[0]) : list;
    sdrplay_api_ErrT get_params = select == sdrplay_api_Success ?
                                   sdrplay_api_GetDeviceParams(devices[0].dev, &params) : select;
    if (open != sdrplay_api_Success || list != sdrplay_api_Success || count == 0u ||
        select != sdrplay_api_Success || get_params != sdrplay_api_Success) {
        fprintf(stderr, "COMPAT_SETUP_FAIL open=%d list=%d count=%u select=%d params=%d\n",
                open, list, count, select, get_params);
        return EXIT_FAILURE;
    }
    params->devParams->fsFreq.fsHz = sample_rate_hz;
    params->devParams->mode = sdrplay_api_BULK;
    params->rxChannelA->tunerParams.rfFreq.rfHz = initial_rf_hz;
    params->rxChannelA->tunerParams.bwType = sdrplay_api_BW_1_536;
    params->rxChannelA->tunerParams.ifType = sdrplay_api_IF_Zero;
    params->rxChannelA->tunerParams.gain.gRdB = 40;
    params->rxChannelA->tunerParams.gain.LNAstate = 3;
    params->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE;
    sdrplay_api_ErrT init = sdrplay_api_Init(devices[0].dev, &callbacks, &result);
    if (init != sdrplay_api_Success) {
        fprintf(stderr, "COMPAT_INIT_FAIL status=%d error=%s\n", init, sdrplay_api_GetErrorString(init));
        return EXIT_FAILURE;
    }
    const struct timespec before_update = {.tv_sec = run_seconds / 2, .tv_nsec = 0};
    const struct timespec after_update = {.tv_sec = run_seconds - run_seconds / 2, .tv_nsec = 0};
    nanosleep(&before_update, NULL);
    sdrplay_api_ReasonForUpdateT reason = sdrplay_api_Update_Tuner_Frf |
                                          sdrplay_api_Update_Tuner_Gr;
    if (updated_sample_rate_hz > 0.0) {
        params->devParams->fsFreq.fsHz = updated_sample_rate_hz;
        reason |= sdrplay_api_Update_Dev_Fs;
    }
    sdrplay_api_ErrT update = sdrplay_api_Success;
    for (unsigned int index = 0; index < update_count; ++index) {
        params->rxChannelA->tunerParams.rfFreq.rfHz =
            (index & 1u) == 0u ? updated_rf_hz : initial_rf_hz;
        params->rxChannelA->tunerParams.gain.gRdB = 20 + (int)(index % 40u);
        sdrplay_api_ErrT current = sdrplay_api_Update(
            devices[0].dev, sdrplay_api_Tuner_A, reason, 0u);
        if (current != sdrplay_api_Success) update = current;
    }
    nanosleep(&after_update, NULL);
    sdrplay_api_ErrT uninit = sdrplay_api_Uninit(devices[0].dev);
    sdrplay_api_ReleaseDevice(&devices[0]);
    sdrplay_api_Close();
    unsigned long long samples = atomic_load(&result.samples);
    double rms = samples == 0u ? 0.0 :
                 sqrt((double)atomic_load(&result.power) / ((double)samples * 2.0));
    printf("COMPAT_STREAM_RESULT samples=%llu callbacks=%u resets=%u rf_changed=%u fs_changed=%u gr_changed=%u peak=%d rms=%.2f updates=%u update=%d uninit=%d\n",
           samples, atomic_load(&result.callbacks), atomic_load(&result.resets),
           atomic_load(&result.rf_changed), atomic_load(&result.fs_changed),
           atomic_load(&result.gr_changed), atomic_load(&result.peak), rms,
           update_count, update, uninit);
    return samples > 1000000u && atomic_load(&result.callbacks) > 0u &&
           atomic_load(&result.rf_changed) == update_count &&
           atomic_load(&result.gr_changed) == update_count &&
           atomic_load(&result.fs_changed) == (updated_sample_rate_hz > 0.0 ? 1u : 0u) &&
           update == sdrplay_api_Success &&
           uninit == sdrplay_api_Success
               ? EXIT_SUCCESS : EXIT_FAILURE;
}
