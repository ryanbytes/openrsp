/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <sdrplay_api.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

static atomic_ullong sum_squares;
static atomic_ullong sample_count;
static atomic_uint peak_sample;

static void stream_callback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params,
                            unsigned int samples, unsigned int reset, void *context)
{
    (void)params; (void)reset; (void)context;
    unsigned long long squares = 0;
    unsigned int peak = 0;
    for (unsigned int index = 0; index < samples; ++index) {
        int i = xi[index];
        int q = xq[index];
        unsigned int magnitude = (unsigned int)(abs(i) > abs(q) ? abs(i) : abs(q));
        if (magnitude > peak) peak = magnitude;
        squares += (unsigned long long)((long long)i * i + (long long)q * q);
    }
    atomic_fetch_add(&sum_squares, squares);
    atomic_fetch_add(&sample_count, samples * 2u);
    unsigned int observed = atomic_load(&peak_sample);
    while (peak > observed && !atomic_compare_exchange_weak(&peak_sample, &observed, peak)) {}
}

static void pause_ms(long milliseconds)
{
    const struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000L
    };
    nanosleep(&delay, NULL);
}

int main(int argc, char **argv)
{
    sdrplay_api_TunerSelectT tuner = sdrplay_api_Tuner_A;
    if (argc == 2 && strcmp(argv[1], "B") == 0)
        tuner = sdrplay_api_Tuner_B;
    else if (argc == 2 && strcmp(argv[1], "A") != 0) {
        fputs("usage: sdrplay-gain-sweep-test [A|B]\n", stderr);
        return EXIT_FAILURE;
    } else if (argc > 2) {
        fputs("usage: sdrplay-gain-sweep-test [A|B]\n", stderr);
        return EXIT_FAILURE;
    }
    sdrplay_api_DeviceT devices[SDRPLAY_MAX_DEVICES];
    unsigned int count = 0;
    sdrplay_api_DeviceParamsT *params = NULL;
    sdrplay_api_CallbackFnsT callbacks = {.StreamACbFn = stream_callback};
    if (sdrplay_api_Open() != sdrplay_api_Success ||
        sdrplay_api_GetDevices(devices, &count, SDRPLAY_MAX_DEVICES) != sdrplay_api_Success ||
        count == 0u) return EXIT_FAILURE;
    devices[0].tuner = tuner;
    if (devices[0].hwVer == SDRPLAY_RSPduo_ID)
        devices[0].rspDuoMode = sdrplay_api_RspDuoMode_Single_Tuner;
    if (sdrplay_api_SelectDevice(&devices[0]) != sdrplay_api_Success ||
        sdrplay_api_GetDeviceParams(devices[0].dev, &params) != sdrplay_api_Success)
        return EXIT_FAILURE;
    sdrplay_api_RxChannelParamsT *channel =
        tuner == sdrplay_api_Tuner_B ? params->rxChannelB : params->rxChannelA;
    if (channel == NULL) return EXIT_FAILURE;

    params->devParams->fsFreq.fsHz = 10000000.0;
    params->devParams->mode = sdrplay_api_BULK;
    channel->tunerParams.rfFreq.rfHz = 853912500.0;
    channel->tunerParams.bwType = sdrplay_api_BW_8_000;
    channel->tunerParams.ifType = sdrplay_api_IF_Zero;
    channel->tunerParams.gain.gRdB = 55;
    channel->tunerParams.gain.LNAstate = 0;
    channel->ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE;
    if (sdrplay_api_Init(devices[0].dev, &callbacks, NULL) != sdrplay_api_Success) return EXIT_FAILURE;
    pause_ms(500);

    for (unsigned int state = 0; state < 10u; ++state) {
        channel->tunerParams.gain.gRdB = 55;
        channel->tunerParams.gain.LNAstate = (unsigned char)state;
        sdrplay_api_ErrT result = sdrplay_api_Update(devices[0].dev, tuner,
                                                     sdrplay_api_Update_Tuner_Gr, 0u);
        pause_ms(100);
        atomic_store(&sum_squares, 0u);
        atomic_store(&sample_count, 0u);
        atomic_store(&peak_sample, 0u);
        pause_ms(400);
        unsigned long long count = atomic_load(&sample_count);
        double mean_square = count == 0u ? 0.0 : (double)atomic_load(&sum_squares) / (double)count;
        printf("GAIN_SWEEP lna=%u gr=55 result=%d samples=%llu rms_squared=%.1f peak=%u current=%.1f\n",
               state, result, count / 2u, mean_square, atomic_load(&peak_sample),
               channel->tunerParams.gain.gainVals.curr);
    }
    const int reductions[] = {20, 30, 40, 50, 59};
    const unsigned int lna_states[] = {0, 5};
    for (unsigned int lna_index = 0; lna_index < 2u; ++lna_index) {
        for (unsigned int gr_index = 0; gr_index < 5u; ++gr_index) {
            channel->tunerParams.gain.gRdB = reductions[gr_index];
            channel->tunerParams.gain.LNAstate = (unsigned char)lna_states[lna_index];
            sdrplay_api_ErrT result = sdrplay_api_Update(devices[0].dev, tuner,
                                                         sdrplay_api_Update_Tuner_Gr, 0u);
            printf("GR_SWEEP lna=%u gr=%d result=%d current=%.1f\n",
                   lna_states[lna_index], reductions[gr_index], result,
                   channel->tunerParams.gain.gainVals.curr);
            pause_ms(150);
        }
    }
    channel->tunerParams.gain.gRdB = 20;
    channel->tunerParams.gain.LNAstate = 5;
    (void)sdrplay_api_Update(devices[0].dev, tuner,
                             sdrplay_api_Update_Tuner_Gr, 0u);
    channel->ctrlParams.agc.setPoint_dBfs = -30;
    channel->ctrlParams.agc.enable = sdrplay_api_AGC_50HZ;
    sdrplay_api_ErrT agc_result = sdrplay_api_Update(devices[0].dev, tuner,
                                                     sdrplay_api_Update_Ctrl_Agc, 0u);
    pause_ms(2000);
    int converged_reduction = channel->tunerParams.gain.gRdB;
    printf("AGC_TEST enabled_result=%d start_gr=20 converged_gr=%d current=%.1f\n",
           agc_result, converged_reduction,
           channel->tunerParams.gain.gainVals.curr);
    channel->tunerParams.gain.gRdB = 59;
    (void)sdrplay_api_Update(devices[0].dev, tuner,
                             sdrplay_api_Update_Tuner_Gr, 0u);
    pause_ms(2000);
    printf("AGC_TEST recovery_start_gr=59 recovered_gr=%d current=%.1f\n",
           channel->tunerParams.gain.gRdB,
           channel->tunerParams.gain.gainVals.curr);
    converged_reduction = channel->tunerParams.gain.gRdB;
    channel->ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE;
    sdrplay_api_ErrT disable_result = sdrplay_api_Update(devices[0].dev, tuner,
                                                         sdrplay_api_Update_Ctrl_Agc, 0u);
    pause_ms(500);
    printf("AGC_TEST disabled_result=%d held_gr=%d expected_gr=%d\n", disable_result,
           channel->tunerParams.gain.gRdB, converged_reduction);
    sdrplay_api_Uninit(devices[0].dev);
    sdrplay_api_ReleaseDevice(&devices[0]);
    sdrplay_api_Close();
    return EXIT_SUCCESS;
}
