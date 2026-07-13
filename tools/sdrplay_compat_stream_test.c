/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "sdrplay_api_compat.h"

#include <stdatomic.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    short *capture;
    size_t capture_capacity;
    atomic_size_t capture_samples;
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
    if (result->capture != NULL) {
        size_t stored = atomic_load(&result->capture_samples);
        size_t count = stored < result->capture_capacity ?
                       result->capture_capacity - stored : 0u;
        if (count > num_samples) count = num_samples;
        for (size_t index = 0u; index < count; ++index) {
            result->capture[(stored + index) * 2u] = xi[index];
            result->capture[(stored + index) * 2u + 1u] = xq[index];
        }
        atomic_store(&result->capture_samples, stored + count);
    }
    atomic_fetch_add(&result->samples, num_samples);
    atomic_fetch_add(&result->power, power);
    atomic_fetch_add(&result->callbacks, 1u);
    if (reset != 0u) atomic_fetch_add(&result->resets, 1u);
    if (params->rfChanged != 0) atomic_fetch_add(&result->rf_changed, 1u);
    if (params->fsChanged != 0) atomic_fetch_add(&result->fs_changed, 1u);
    if (params->grChanged != 0) atomic_fetch_add(&result->gr_changed, 1u);
    if (params->numSamples != num_samples) abort();
}

static double callback_sample_rate(double adc_rate_hz, long if_frequency_khz)
{
    if (adc_rate_hz == 6000000.0 && if_frequency_khz == 1620)
        return adc_rate_hz / 3.0;
    if (adc_rate_hz == 8000000.0 && if_frequency_khz == 2048)
        return adc_rate_hz / 4.0;
    return adc_rate_hz;
}

int main(int argc, char **argv)
{
    double initial_rf_hz = 100000000.0;
    double updated_rf_hz = 100100000.0;
    long run_seconds = 5;
    double sample_rate_hz = 2048000.0;
    double updated_sample_rate_hz = 0.0;
    unsigned int update_count = 1u;
    long if_frequency_khz = 0;
    const char *capture_path = NULL;
    long bandwidth_khz = 1536;
    long initial_gain_reduction = 40;
    unsigned long initial_lna_state = 3u;
    if (argc > 1) initial_rf_hz = strtod(argv[1], NULL);
    if (argc > 2) updated_rf_hz = strtod(argv[2], NULL);
    if (argc > 3) run_seconds = strtol(argv[3], NULL, 10);
    if (argc > 4) sample_rate_hz = strtod(argv[4], NULL);
    if (argc > 5) updated_sample_rate_hz = strtod(argv[5], NULL);
    if (argc > 6) update_count = (unsigned int)strtoul(argv[6], NULL, 10);
    if (argc > 7) if_frequency_khz = strtol(argv[7], NULL, 10);
    if (argc > 8) capture_path = argv[8];
    if (argc > 9) bandwidth_khz = strtol(argv[9], NULL, 10);
    if (argc > 10) initial_gain_reduction = strtol(argv[10], NULL, 10);
    if (argc > 11) initial_lna_state = strtoul(argv[11], NULL, 10);
    if (argc > 12 || !isfinite(initial_rf_hz) || !isfinite(updated_rf_hz) ||
        initial_rf_hz <= 0.0 || updated_rf_hz <= 0.0 || run_seconds < 2 ||
        !isfinite(sample_rate_hz) || sample_rate_hz <= 0.0 || update_count == 0u ||
        (if_frequency_khz != 0 && if_frequency_khz != 450 &&
         if_frequency_khz != 1620 && if_frequency_khz != 2048) ||
        (bandwidth_khz != 200 && bandwidth_khz != 300 && bandwidth_khz != 600 &&
         bandwidth_khz != 1536 && bandwidth_khz != 5000 && bandwidth_khz != 6000 &&
         bandwidth_khz != 7000 && bandwidth_khz != 8000) ||
        initial_gain_reduction < 20 || initial_gain_reduction > 59 ||
        initial_lna_state > 9u) {
        fputs("usage: sdrplay-compat-stream-test [initial-rf-hz [updated-rf-hz [seconds [sample-rate-hz [updated-sample-rate-hz [update-count [if-frequency-khz [capture.iq [bandwidth-khz [gain-reduction [lna-state]]]]]]]]]]\n",
              stderr);
        return EXIT_FAILURE;
    }
    sdrplay_api_DeviceT devices[SDRPLAY_MAX_DEVICES];
    unsigned int count = 0;
    sdrplay_api_DeviceParamsT *params = NULL;
    metrics result = {0};
    if (capture_path != NULL) {
        result.capture_capacity = (size_t)fmin(sample_rate_hz, 4000000.0);
        result.capture = calloc(result.capture_capacity * 2u, sizeof(*result.capture));
        if (result.capture == NULL) {
            fprintf(stderr, "capture allocation failed for %zu IQ samples\n",
                    result.capture_capacity);
            return EXIT_FAILURE;
        }
    }
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
        if (select == sdrplay_api_Success) (void)sdrplay_api_ReleaseDevice(&devices[0]);
        if (open == sdrplay_api_Success) (void)sdrplay_api_Close();
        free(result.capture);
        return EXIT_FAILURE;
    }
    params->devParams->fsFreq.fsHz = sample_rate_hz;
    params->devParams->mode = sdrplay_api_BULK;
    params->rxChannelA->tunerParams.rfFreq.rfHz = initial_rf_hz;
    params->rxChannelA->tunerParams.bwType = (sdrplay_api_Bw_MHzT)bandwidth_khz;
    params->rxChannelA->tunerParams.ifType = (sdrplay_api_If_kHzT)if_frequency_khz;
    params->rxChannelA->tunerParams.gain.gRdB = (int)initial_gain_reduction;
    params->rxChannelA->tunerParams.gain.LNAstate = (unsigned char)initial_lna_state;
    params->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE;
    sdrplay_api_ErrT init = sdrplay_api_Init(devices[0].dev, &callbacks, &result);
    if (init != sdrplay_api_Success) {
        fprintf(stderr, "COMPAT_INIT_FAIL status=%d error=%s\n", init, sdrplay_api_GetErrorString(init));
        (void)sdrplay_api_ReleaseDevice(&devices[0]);
        (void)sdrplay_api_Close();
        free(result.capture);
        return EXIT_FAILURE;
    }
    const struct timespec before_update = {.tv_sec = run_seconds / 2, .tv_nsec = 0};
    const long after_update_seconds = run_seconds - run_seconds / 2;
    const uint64_t update_interval_ns =
        (uint64_t)after_update_seconds * 1000000000u / update_count;
    const struct timespec update_interval = {
        .tv_sec = (time_t)(update_interval_ns / 1000000000u),
        .tv_nsec = (long)(update_interval_ns % 1000000000u)
    };
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
        (void)nanosleep(&update_interval, NULL);
    }
    sdrplay_api_ErrT uninit = sdrplay_api_Uninit(devices[0].dev);
    sdrplay_api_ReleaseDevice(&devices[0]);
    sdrplay_api_Close();
    int capture_ok = 1;
    size_t captured = atomic_load(&result.capture_samples);
    if (capture_path != NULL) {
        FILE *capture_file = fopen(capture_path, "wb");
        if (capture_file == NULL ||
            fwrite(result.capture, sizeof(*result.capture), captured * 2u,
                   capture_file) != captured * 2u) {
            fprintf(stderr, "failed to write IQ capture: %s\n", capture_path);
            capture_ok = 0;
        }
        if (capture_file != NULL && fclose(capture_file) != 0) capture_ok = 0;
        fprintf(stderr, "COMPAT_CAPTURE path=%s samples=%zu bytes=%zu\n",
                capture_path, captured, captured * 2u * sizeof(*result.capture));
        free(result.capture);
    }
    unsigned long long samples = atomic_load(&result.samples);
    const double first_rate_seconds = (double)(run_seconds / 2);
    const double second_rate_seconds = (double)(run_seconds - run_seconds / 2);
    const double updated_adc_rate = updated_sample_rate_hz > 0.0 ?
                                    updated_sample_rate_hz : sample_rate_hz;
    const double expected_samples =
        callback_sample_rate(sample_rate_hz, if_frequency_khz) * first_rate_seconds +
        callback_sample_rate(updated_adc_rate, if_frequency_khz) * second_rate_seconds;
    const double sample_error = expected_samples > 0.0 ?
        fabs((double)samples - expected_samples) / expected_samples : 1.0;
    double rms = samples == 0u ? 0.0 :
                 sqrt((double)atomic_load(&result.power) / ((double)samples * 2.0));
    printf("COMPAT_STREAM_RESULT samples=%llu sample_error=%.4f%% callbacks=%u resets=%u rf_changed=%u fs_changed=%u gr_changed=%u peak=%d rms=%.2f updates=%u update=%d uninit=%d\n",
           samples, sample_error * 100.0, atomic_load(&result.callbacks),
           atomic_load(&result.resets),
           atomic_load(&result.rf_changed), atomic_load(&result.fs_changed),
           atomic_load(&result.gr_changed), atomic_load(&result.peak), rms,
           update_count, update, uninit);
    return capture_ok && samples > 1000000u && sample_error <= 0.05 &&
           atomic_load(&result.callbacks) > 0u &&
           atomic_load(&result.resets) == 1u &&
           atomic_load(&result.rf_changed) == update_count &&
           atomic_load(&result.gr_changed) == update_count &&
           atomic_load(&result.fs_changed) ==
               (updated_sample_rate_hz > 0.0 ? update_count : 0u) &&
           update == sdrplay_api_Success &&
           uninit == sdrplay_api_Success
               ? EXIT_SUCCESS : EXIT_FAILURE;
}
