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

typedef struct {
    atomic_ullong samples;
} rate_metrics;

static void stream_callback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params,
                            unsigned int num_samples, unsigned int reset, void *opaque)
{
    (void)xi;
    (void)xq;
    (void)params;
    (void)reset;
    rate_metrics *metrics = opaque;
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
    printf("rate requested=%.0f measured=%.3f error=%.4f%% samples=%llu\n",
           requested, measured, error * 100.0, samples);
    if (error > RATE_ERROR_LIMIT) {
        fprintf(stderr, "rate %.0f differs from wall clock by more than 5%%\n",
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
    params->devParams->fsFreq.fsHz = 2000000.0;
    params->devParams->mode = sdrplay_api_BULK;
    params->rxChannelA->tunerParams.rfFreq.rfHz = 101100000.0;
    params->rxChannelA->tunerParams.bwType = sdrplay_api_BW_1_536;
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
