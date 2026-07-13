/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "sdrplay_api_compat.h"

#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RATE_ERROR_LIMIT 0.05
#define SPECTRUM_BLOCK_SAMPLES 1024u
#define SPECTRUM_MAX_BLOCKS 8u

typedef struct {
    atomic_ullong samples;
    atomic_ullong i_power;
    atomic_ullong q_power;
    atomic_llong iq_cross;
    atomic_uint spectrum_blocks;
    double negative_spectrum_power;
    double positive_spectrum_power;
} rate_metrics;

static void accumulate_spectrum(rate_metrics *metrics, const short *xi, const short *xq,
                                unsigned int num_samples)
{
    static const int bins[] = {64, 128, 192, 256, 320, 384, 448};
    if (num_samples < SPECTRUM_BLOCK_SAMPLES ||
        atomic_load(&metrics->spectrum_blocks) >= SPECTRUM_MAX_BLOCKS) return;
    double negative = 0.0;
    double positive = 0.0;
    const double tau = 2.0 * acos(-1.0);
    for (size_t bin_index = 0u; bin_index < sizeof(bins) / sizeof(bins[0]); ++bin_index) {
        for (int direction = -1; direction <= 1; direction += 2) {
            double real = 0.0;
            double imaginary = 0.0;
            const double step = tau * direction * bins[bin_index] /
                (double)SPECTRUM_BLOCK_SAMPLES;
            for (unsigned int sample = 0u; sample < SPECTRUM_BLOCK_SAMPLES; ++sample) {
                const double window = 0.5 - 0.5 * cos(tau * sample /
                                                       (SPECTRUM_BLOCK_SAMPLES - 1u));
                const double cosine = cos(step * sample);
                const double sine = sin(step * sample);
                real += window * (xi[sample] * cosine + xq[sample] * sine);
                imaginary += window * (xq[sample] * cosine - xi[sample] * sine);
            }
            const double power = real * real + imaginary * imaginary;
            if (direction < 0) negative += power;
            else positive += power;
        }
    }
    metrics->negative_spectrum_power += negative;
    metrics->positive_spectrum_power += positive;
    atomic_fetch_add(&metrics->spectrum_blocks, 1u);
}

static void stream_callback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params,
                            unsigned int num_samples, unsigned int reset, void *opaque)
{
    (void)params;
    (void)reset;
    rate_metrics *metrics = opaque;
    accumulate_spectrum(metrics, xi, xq, num_samples);
    unsigned long long i_power = 0u;
    unsigned long long q_power = 0u;
    long long iq_cross = 0;
    for (unsigned int sample = 0u; sample < num_samples; ++sample) {
        const long long i = xi[sample];
        const long long q = xq[sample];
        i_power += (unsigned long long)(i * i);
        q_power += (unsigned long long)(q * q);
        iq_cross += i * q;
    }
    atomic_fetch_add(&metrics->i_power, i_power);
    atomic_fetch_add(&metrics->q_power, q_power);
    atomic_fetch_add(&metrics->iq_cross, iq_cross);
    atomic_fetch_add(&metrics->samples, num_samples);
}

static int sleep_milliseconds(long milliseconds)
{
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000L
    };
    while (nanosleep(&delay, &delay) < 0) {
        if (errno != EINTR) return -1;
    }
    return 0;
}

static double monotonic_seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1.0;
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static int measure_rate(sdrplay_api_DeviceT *device,
                        sdrplay_api_DeviceParamsT *params,
                        rate_metrics *metrics, double requested)
{
    params->devParams->fsFreq.fsHz = requested;
    sdrplay_api_ErrT update = sdrplay_api_Update(
        device->dev, sdrplay_api_Tuner_A, sdrplay_api_Update_Dev_Fs,
        sdrplay_api_Update_Ext1_None);
    if (update != sdrplay_api_Success) {
        fprintf(stderr, "rate %.0f update failed: %s\n", requested,
                sdrplay_api_GetErrorString(update));
        return -1;
    }
    if (sleep_milliseconds(750) < 0) {
        perror("nanosleep");
        return -1;
    }
    atomic_store(&metrics->samples, 0u);
    atomic_store(&metrics->i_power, 0u);
    atomic_store(&metrics->q_power, 0u);
    atomic_store(&metrics->iq_cross, 0);
    atomic_store(&metrics->spectrum_blocks, 0u);
    metrics->negative_spectrum_power = 0.0;
    metrics->positive_spectrum_power = 0.0;
    const double started = monotonic_seconds();
    if (started < 0.0 || sleep_milliseconds(1500) < 0) {
        perror("rate measurement clock");
        return -1;
    }
    const double elapsed = monotonic_seconds() - started;
    const unsigned long long samples = atomic_load(&metrics->samples);
    if (elapsed <= 0.0 || samples == 0u) {
        fprintf(stderr, "rate %.0f delivered no measurable samples\n", requested);
        return -1;
    }
    const double measured = (double)samples / elapsed;
    const double error = fabs(measured - requested) / requested;
    const double i_rms = sqrt((double)atomic_load(&metrics->i_power) / (double)samples);
    const double q_rms = sqrt((double)atomic_load(&metrics->q_power) / (double)samples);
    const double iq = (double)atomic_load(&metrics->iq_cross) / (double)samples;
    const double correlation = i_rms > 0.0 && q_rms > 0.0 ? iq / (i_rms * q_rms) : 0.0;
    const unsigned int spectrum_blocks = atomic_load_explicit(
        &metrics->spectrum_blocks, memory_order_acquire);
    const double spectrum_ratio_db = spectrum_blocks == SPECTRUM_MAX_BLOCKS &&
        metrics->negative_spectrum_power > 0.0 && metrics->positive_spectrum_power > 0.0 ?
        10.0 * log10(metrics->negative_spectrum_power /
                     metrics->positive_spectrum_power) : 0.0;
    printf("rate requested=%.0f measured=%.3f error=%.4f%% samples=%llu "
           "i_rms=%.3f q_rms=%.3f iq_correlation=%.9f "
           "negative_to_positive_db=%.3f spectrum_blocks=%u\n",
           requested, measured, error * 100.0, samples, i_rms, q_rms, correlation,
           spectrum_ratio_db, spectrum_blocks);
    if (error > RATE_ERROR_LIMIT) {
        fprintf(stderr, "rate %.0f differs from wall clock by more than 5%%\n",
                requested);
        return -1;
    }
    if (requested >= 8000000.0 &&
        (spectrum_blocks != SPECTRUM_MAX_BLOCKS || fabs(spectrum_ratio_db) > 20.0)) {
        fprintf(stderr, "rate %.0f does not populate both complex spectral halves\n",
                requested);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    static const double rates[] = {
        2000000.0, 2048000.0, 3000000.0, 4000000.0, 5000000.0,
        6000000.0, 7000000.0, 8000000.0, 9000000.0, 10000000.0,
        10660000.0
    };
    double single_rate = 0.0;
    if (argc == 3 && strcmp(argv[1], "--rate") == 0) {
        char *end = NULL;
        single_rate = strtod(argv[2], &end);
        if (end == argv[2] || *end != '\0' || !isfinite(single_rate) ||
            single_rate < 2000000.0 || single_rate > 10660000.0) {
            fputs("--rate must be between 2000000 and 10660000 samples/second\n",
                  stderr);
            return EXIT_FAILURE;
        }
    } else if (argc != 1 && !(argc == 2 && strcmp(argv[1], "--rates") == 0)) {
        fprintf(stderr, "usage: %s [--rates | --rate SPS]\n", argv[0]);
        return EXIT_FAILURE;
    }

    sdrplay_api_DeviceT devices[SDRPLAY_MAX_DEVICES];
    unsigned int count = 0u;
    sdrplay_api_DeviceParamsT *params = NULL;
    sdrplay_api_CallbackFnsT callbacks = {.StreamACbFn = stream_callback};
    rate_metrics metrics = {0};
    int opened = 0;
    int selected = 0;
    int initialized = 0;
    int failed = 0;

    sdrplay_api_ErrT status = sdrplay_api_Open();
    if (status != sdrplay_api_Success) {
        fprintf(stderr, "Open failed: %s\n", sdrplay_api_GetErrorString(status));
        return EXIT_FAILURE;
    }
    opened = 1;
    status = sdrplay_api_GetDevices(devices, &count, SDRPLAY_MAX_DEVICES);
    if (status != sdrplay_api_Success || count == 0u) {
        fprintf(stderr, "GetDevices failed: status=%s count=%u\n",
                sdrplay_api_GetErrorString(status), count);
        failed = 1;
        goto cleanup;
    }
    if (devices[0].hwVer == SDRPLAY_RSPduo_ID) {
        devices[0].tuner = sdrplay_api_Tuner_A;
        devices[0].rspDuoMode = sdrplay_api_RspDuoMode_Single_Tuner;
    }
    status = sdrplay_api_SelectDevice(&devices[0]);
    if (status != sdrplay_api_Success) {
        fprintf(stderr, "SelectDevice failed: %s\n", sdrplay_api_GetErrorString(status));
        failed = 1;
        goto cleanup;
    }
    selected = 1;
    status = sdrplay_api_GetDeviceParams(devices[0].dev, &params);
    if (status != sdrplay_api_Success || params == NULL) {
        fprintf(stderr, "GetDeviceParams failed: %s\n", sdrplay_api_GetErrorString(status));
        failed = 1;
        goto cleanup;
    }
    params->devParams->fsFreq.fsHz = single_rate != 0.0 ? single_rate : 2000000.0;
    params->devParams->mode = sdrplay_api_BULK;
    params->rxChannelA->tunerParams.rfFreq.rfHz = 101100000.0;
    params->rxChannelA->tunerParams.bwType = single_rate >= 8000000.0 ?
        sdrplay_api_BW_8_000 : sdrplay_api_BW_1_536;
    params->rxChannelA->tunerParams.ifType = sdrplay_api_IF_Zero;
    params->rxChannelA->tunerParams.gain.gRdB = 40;
    params->rxChannelA->tunerParams.gain.LNAstate = 5;
    params->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE;
    status = sdrplay_api_Init(devices[0].dev, &callbacks, &metrics);
    if (status != sdrplay_api_Success) {
        fprintf(stderr, "Init failed: %s\n", sdrplay_api_GetErrorString(status));
        failed = 1;
        goto cleanup;
    }
    initialized = 1;

    if (single_rate != 0.0) {
        failed = measure_rate(&devices[0], params, &metrics, single_rate) < 0;
    } else {
        for (size_t index = 0u; index < sizeof(rates) / sizeof(rates[0]); ++index) {
            if (measure_rate(&devices[0], params, &metrics, rates[index]) < 0) {
                failed = 1;
                break;
            }
        }
    }

cleanup:
    if (initialized) {
        params->devParams->fsFreq.fsHz = 2000000.0;
        sdrplay_api_ErrT restore_rate = sdrplay_api_Update(
            devices[0].dev, sdrplay_api_Tuner_A, sdrplay_api_Update_Dev_Fs,
            sdrplay_api_Update_Ext1_None);
        params->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_CTRL_EN;
        params->rxChannelA->ctrlParams.agc.setPoint_dBfs = -30;
        sdrplay_api_ErrT restore_agc = sdrplay_api_Update(
            devices[0].dev, sdrplay_api_Tuner_A, sdrplay_api_Update_Ctrl_Agc,
            sdrplay_api_Update_Ext1_None);
        if (restore_rate != sdrplay_api_Success || restore_agc != sdrplay_api_Success) {
            fprintf(stderr, "cleanup warning: restore rate=%s AGC=%s\n",
                    sdrplay_api_GetErrorString(restore_rate),
                    sdrplay_api_GetErrorString(restore_agc));
            failed = 1;
        }
        if (sdrplay_api_Uninit(devices[0].dev) != sdrplay_api_Success) failed = 1;
    }
    if (selected && sdrplay_api_ReleaseDevice(&devices[0]) != sdrplay_api_Success)
        failed = 1;
    if (opened && sdrplay_api_Close() != sdrplay_api_Success) failed = 1;
    if (!failed)
        puts("PASS: native SDRplay-compatible API rates matched wall-clock output; 2 MS/s and AGC restored");
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
