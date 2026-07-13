/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <sdrplay_api.h>

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    atomic_ullong samples[2];
    atomic_ullong power[2];
    atomic_uint callbacks[2];
    atomic_uint resets[2];
    atomic_uint rf_changed[2];
    atomic_uint gr_changed[2];
    atomic_uint mode_events;
    atomic_uint last_mode_event;
} probe_metrics;

static void collect(probe_metrics *metrics, unsigned int stream, short *xi, short *xq,
                    sdrplay_api_StreamCbParamsT *params, unsigned int samples,
                    unsigned int reset)
{
    unsigned long long power = 0u;
    for (unsigned int index = 0u; index < samples; ++index) {
        long i = xi[index];
        long q = xq[index];
        power += (unsigned long long)(i * i + q * q);
    }
    atomic_fetch_add(&metrics->samples[stream], samples);
    atomic_fetch_add(&metrics->power[stream], power);
    atomic_fetch_add(&metrics->callbacks[stream], 1u);
    if (reset != 0u) atomic_fetch_add(&metrics->resets[stream], 1u);
    if (params->rfChanged != 0) atomic_fetch_add(&metrics->rf_changed[stream], 1u);
    if (params->grChanged != 0) atomic_fetch_add(&metrics->gr_changed[stream], 1u);
}

static void stream_a(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params,
                     unsigned int samples, unsigned int reset, void *opaque)
{
    collect(opaque, 0u, xi, xq, params, samples, reset);
}

static void stream_b(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params,
                     unsigned int samples, unsigned int reset, void *opaque)
{
    collect(opaque, 1u, xi, xq, params, samples, reset);
}

static void event_callback(sdrplay_api_EventT event, sdrplay_api_TunerSelectT tuner,
                           sdrplay_api_EventParamsT *params, void *opaque)
{
    probe_metrics *metrics = opaque;
    if (event == sdrplay_api_RspDuoModeChange && params != NULL) {
        atomic_fetch_add(&metrics->mode_events, 1u);
        atomic_store(&metrics->last_mode_event,
                     (unsigned int)params->rspDuoModeParams.modeChangeType);
        fprintf(stderr, "MODE_EVENT event=%d tuner=%d mode_change=%d\n", event,
                tuner, params->rspDuoModeParams.modeChangeType);
    } else {
        fprintf(stderr, "MODE_EVENT event=%d tuner=%d\n", event, tuner);
    }
}

static void delay_ms(long milliseconds)
{
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000L
    };
    (void)nanosleep(&delay, NULL);
}

static void configure_channel(sdrplay_api_RxChannelParamsT *channel, double rf_hz,
                              int gain_reduction, unsigned char lna_state)
{
    channel->tunerParams.rfFreq.rfHz = rf_hz;
    channel->tunerParams.bwType = sdrplay_api_BW_1_536;
    channel->tunerParams.ifType = sdrplay_api_IF_1_620;
    channel->tunerParams.loMode = sdrplay_api_LO_Auto;
    channel->tunerParams.gain.gRdB = gain_reduction;
    channel->tunerParams.gain.LNAstate = lna_state;
    channel->ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE;
}

static sdrplay_api_RxChannelParamsT *selected_channel(
    sdrplay_api_DeviceParamsT *params, sdrplay_api_TunerSelectT tuner)
{
    return tuner == sdrplay_api_Tuner_B ? params->rxChannelB : params->rxChannelA;
}

static int finish(sdrplay_api_DeviceT *device, int initialized, sdrplay_api_ErrT status)
{
    sdrplay_api_ErrT uninit = initialized ? sdrplay_api_Uninit(device->dev) :
                                           sdrplay_api_Success;
    sdrplay_api_ErrT release = sdrplay_api_ReleaseDevice(device);
    sdrplay_api_ErrT close = sdrplay_api_Close();
    fprintf(stderr, "MODE_CLEANUP status=%d uninit=%d release=%d close=%d\n",
            status, uninit, release, close);
    return status == sdrplay_api_Success && uninit == sdrplay_api_Success &&
           release == sdrplay_api_Success && close == sdrplay_api_Success ?
           EXIT_SUCCESS : EXIT_FAILURE;
}

static int run_dual(sdrplay_api_DeviceT *device, unsigned int update_mask,
                    int test_controls)
{
    double adc_rate = getenv("OPENRSP_PROBE_DUAL_ADC_RATE") != NULL &&
                      strcmp(getenv("OPENRSP_PROBE_DUAL_ADC_RATE"), "8") == 0 ?
                      8000000.0 : 6000000.0;
    device->tuner = sdrplay_api_Tuner_Both;
    device->rspDuoMode = sdrplay_api_RspDuoMode_Dual_Tuner;
    device->rspDuoSampleFreq = adc_rate;
    sdrplay_api_ErrT status = sdrplay_api_SelectDevice(device);
    sdrplay_api_DeviceParamsT *params = NULL;
    if (status == sdrplay_api_Success)
        status = sdrplay_api_GetDeviceParams(device->dev, &params);
    if (status != sdrplay_api_Success || params == NULL || params->devParams == NULL ||
        params->rxChannelA == NULL || params->rxChannelB == NULL) {
        fprintf(stderr, "MODE_DUAL_SETUP status=%d params=%p\n", status, (void *)params);
        return finish(device, 0, status);
    }

    params->devParams->fsFreq.fsHz = adc_rate;
    params->devParams->mode = sdrplay_api_BULK;
    configure_channel(params->rxChannelA, 853712500.0, 40, 0u);
    configure_channel(params->rxChannelB, 853862500.0, 55, 3u);
    if (adc_rate == 8000000.0) {
        params->rxChannelA->tunerParams.ifType = sdrplay_api_IF_2_048;
        params->rxChannelB->tunerParams.ifType = sdrplay_api_IF_2_048;
    }
    probe_metrics metrics = {0};
    sdrplay_api_CallbackFnsT callbacks = {
        .StreamACbFn = stream_a,
        .StreamBCbFn = stream_b,
        .EventCbFn = event_callback
    };
    status = sdrplay_api_Init(device->dev, &callbacks, &metrics);
    if (status != sdrplay_api_Success) {
        fprintf(stderr, "MODE_DUAL_INIT status=%d error=%s\n", status,
                sdrplay_api_GetErrorString(status));
        return finish(device, 0, status);
    }
    delay_ms(2000);
    unsigned long long before_samples_a = atomic_load(&metrics.samples[0]);
    unsigned long long before_samples_b = atomic_load(&metrics.samples[1]);
    unsigned long long before_power_a = atomic_load(&metrics.power[0]);
    unsigned long long before_power_b = atomic_load(&metrics.power[1]);
    double before_mean_a = before_samples_a != 0u ?
        (double)before_power_a / (2.0 * before_samples_a) : 0.0;
    double before_mean_b = before_samples_b != 0u ?
        (double)before_power_b / (2.0 * before_samples_b) : 0.0;
    sdrplay_api_ErrT update_a = sdrplay_api_Success;
    sdrplay_api_ErrT update_b = sdrplay_api_Success;
    sdrplay_api_ErrT controls_a = sdrplay_api_Success;
    sdrplay_api_ErrT controls_b = sdrplay_api_Success;
    if ((update_mask & 1u) != 0u) {
        params->rxChannelA->tunerParams.rfFreq.rfHz = 853812500.0;
        params->rxChannelA->tunerParams.gain.LNAstate = 2u;
        update_a = sdrplay_api_Update(
            device->dev, sdrplay_api_Tuner_A,
            sdrplay_api_Update_Tuner_Frf | sdrplay_api_Update_Tuner_Gr, 0u);
    }
    if ((update_mask & 2u) != 0u) {
        params->rxChannelB->tunerParams.rfFreq.rfHz = 853962500.0;
        params->rxChannelB->tunerParams.gain.LNAstate = 5u;
        update_b = sdrplay_api_Update(
            device->dev, sdrplay_api_Tuner_B,
            sdrplay_api_Update_Tuner_Frf | sdrplay_api_Update_Tuner_Gr, 0u);
    }
    if (test_controls) {
        params->rxChannelA->rspDuoTunerParams.rfNotchEnable = 1u;
        params->rxChannelA->rspDuoTunerParams.rfDabNotchEnable = 1u;
        params->devParams->rspDuoParams.extRefOutputEn = 1;
        controls_a = sdrplay_api_Update(
            device->dev, sdrplay_api_Tuner_A,
            sdrplay_api_Update_RspDuo_RfNotchControl |
            sdrplay_api_Update_RspDuo_RfDabNotchControl |
            sdrplay_api_Update_RspDuo_ExtRefControl, 0u);
        params->rxChannelB->rspDuoTunerParams.biasTEnable = 1u;
        params->rxChannelB->rspDuoTunerParams.rfNotchEnable = 1u;
        params->rxChannelB->rspDuoTunerParams.rfDabNotchEnable = 1u;
        controls_b = sdrplay_api_Update(
            device->dev, sdrplay_api_Tuner_B,
            sdrplay_api_Update_RspDuo_BiasTControl |
            sdrplay_api_Update_RspDuo_RfNotchControl |
            sdrplay_api_Update_RspDuo_RfDabNotchControl, 0u);
    }
    delay_ms(2000);
    unsigned long long samples_a = atomic_load(&metrics.samples[0]);
    unsigned long long samples_b = atomic_load(&metrics.samples[1]);
    unsigned long long after_samples_a = samples_a - before_samples_a;
    unsigned long long after_samples_b = samples_b - before_samples_b;
    unsigned long long after_power_a = atomic_load(&metrics.power[0]) - before_power_a;
    unsigned long long after_power_b = atomic_load(&metrics.power[1]) - before_power_b;
    double after_mean_a = after_samples_a != 0u ?
        (double)after_power_a / (2.0 * after_samples_a) : 0.0;
    double after_mean_b = after_samples_b != 0u ?
        (double)after_power_b / (2.0 * after_samples_b) : 0.0;
    fprintf(stderr,
            "MODE_DUAL_RESULT update_a=%d update_b=%d controls_a=%d controls_b=%d a_samples=%llu a_callbacks=%u "
            "a_resets=%u a_rf=%u a_gr=%u a_before_mean_square=%.1f "
            "a_after_mean_square=%.1f b_samples=%llu b_callbacks=%u b_resets=%u "
            "b_rf=%u b_gr=%u b_before_mean_square=%.1f "
            "b_after_mean_square=%.1f\n",
            update_a, update_b, controls_a, controls_b, samples_a,
            atomic_load(&metrics.callbacks[0]),
            atomic_load(&metrics.resets[0]), atomic_load(&metrics.rf_changed[0]),
            atomic_load(&metrics.gr_changed[0]), before_mean_a, after_mean_a, samples_b,
            atomic_load(&metrics.callbacks[1]), atomic_load(&metrics.resets[1]),
            atomic_load(&metrics.rf_changed[1]), atomic_load(&metrics.gr_changed[1]),
            before_mean_b, after_mean_b);
    if (update_a != sdrplay_api_Success || update_b != sdrplay_api_Success ||
        controls_a != sdrplay_api_Success || controls_b != sdrplay_api_Success ||
        samples_a == 0u || samples_b == 0u) status = sdrplay_api_Fail;
    return finish(device, 1, status);
}

static int run_swap(sdrplay_api_DeviceT *device)
{
    device->tuner = sdrplay_api_Tuner_A;
    device->rspDuoMode = sdrplay_api_RspDuoMode_Single_Tuner;
    sdrplay_api_ErrT status = sdrplay_api_SelectDevice(device);
    sdrplay_api_DeviceParamsT *params = NULL;
    if (status == sdrplay_api_Success)
        status = sdrplay_api_GetDeviceParams(device->dev, &params);
    if (status != sdrplay_api_Success || params == NULL || params->devParams == NULL ||
        params->rxChannelA == NULL) {
        fprintf(stderr, "MODE_SWAP_SETUP status=%d params=%p\n", status, (void *)params);
        return finish(device, 0, status);
    }
    params->devParams->fsFreq.fsHz = 6000000.0;
    params->devParams->mode = sdrplay_api_BULK;
    configure_channel(params->rxChannelA, 853862500.0, 45, 2u);
    probe_metrics metrics = {0};
    sdrplay_api_CallbackFnsT callbacks = {
        .StreamACbFn = stream_a,
        .StreamBCbFn = stream_b,
        .EventCbFn = event_callback
    };
    status = sdrplay_api_Init(device->dev, &callbacks, &metrics);
    if (status != sdrplay_api_Success) return finish(device, 0, status);
    delay_ms(1500);
    fprintf(stderr, "MODE_SWAP_POINTERS stage=before tuner=%d a=%p b=%p\n",
            device->tuner, (void *)params->rxChannelA, (void *)params->rxChannelB);
    sdrplay_api_ErrT swap_b = sdrplay_api_SwapRspDuoActiveTuner(
        device->dev, &device->tuner, sdrplay_api_RspDuo_AMPORT_2);
    delay_ms(1500);
    sdrplay_api_ErrT update_b = sdrplay_api_Fail;
    sdrplay_api_DeviceParamsT *after_swap = NULL;
    sdrplay_api_ErrT get_after_swap = sdrplay_api_GetDeviceParams(device->dev, &after_swap);
    fprintf(stderr,
            "MODE_SWAP_POINTERS stage=after_b tuner=%d get=%d params=%p a=%p b=%p\n",
            device->tuner, get_after_swap, (void *)after_swap,
            after_swap == NULL ? NULL : (void *)after_swap->rxChannelA,
            after_swap == NULL ? NULL : (void *)after_swap->rxChannelB);
    if (swap_b == sdrplay_api_Success && device->tuner == sdrplay_api_Tuner_B &&
        get_after_swap == sdrplay_api_Success && after_swap != NULL &&
        selected_channel(after_swap, device->tuner) != NULL) {
        selected_channel(after_swap, device->tuner)->tunerParams.gain.LNAstate = 4u;
        update_b = sdrplay_api_Update(device->dev, sdrplay_api_Tuner_B,
                                      sdrplay_api_Update_Tuner_Gr, 0u);
    }
    sdrplay_api_ErrT swap_a = sdrplay_api_SwapRspDuoActiveTuner(
        device->dev, &device->tuner, sdrplay_api_RspDuo_AMPORT_2);
    delay_ms(1500);
    fprintf(stderr,
            "MODE_SWAP_RESULT swap_b=%d update_b=%d swap_a=%d current=%d "
            "a_samples=%llu a_callbacks=%u a_resets=%u a_gr=%u b_callbacks=%u\n",
            swap_b, update_b, swap_a, device->tuner,
            atomic_load(&metrics.samples[0]), atomic_load(&metrics.callbacks[0]),
            atomic_load(&metrics.resets[0]), atomic_load(&metrics.gr_changed[0]),
            atomic_load(&metrics.callbacks[1]));
    if (swap_b != sdrplay_api_Success || update_b != sdrplay_api_Success ||
        swap_a != sdrplay_api_Success || device->tuner != sdrplay_api_Tuner_A ||
        atomic_load(&metrics.samples[0]) == 0u ||
        atomic_load(&metrics.gr_changed[0]) == 0u) status = sdrplay_api_Fail;
    return finish(device, 1, status);
}

static int run_dual_rate_swap(sdrplay_api_DeviceT *device)
{
    device->tuner = sdrplay_api_Tuner_Both;
    device->rspDuoMode = sdrplay_api_RspDuoMode_Dual_Tuner;
    device->rspDuoSampleFreq = 6000000.0;
    sdrplay_api_ErrT status = sdrplay_api_SelectDevice(device);
    sdrplay_api_DeviceParamsT *params = NULL;
    if (status == sdrplay_api_Success)
        status = sdrplay_api_GetDeviceParams(device->dev, &params);
    if (status != sdrplay_api_Success || params == NULL || params->devParams == NULL ||
        params->rxChannelA == NULL || params->rxChannelB == NULL)
        return finish(device, 0, status);
    params->devParams->fsFreq.fsHz = 6000000.0;
    params->devParams->mode = sdrplay_api_BULK;
    configure_channel(params->rxChannelA, 853712500.0, 40, 0u);
    configure_channel(params->rxChannelB, 853862500.0, 55, 3u);
    probe_metrics metrics = {0};
    sdrplay_api_CallbackFnsT callbacks = {
        .StreamACbFn = stream_a, .StreamBCbFn = stream_b,
        .EventCbFn = event_callback
    };
    status = sdrplay_api_Init(device->dev, &callbacks, &metrics);
    if (status != sdrplay_api_Success) return finish(device, 0, status);
    delay_ms(1200);
    double current_rate = 6000000.0;
    sdrplay_api_ErrT to_8 = sdrplay_api_SwapRspDuoDualTunerModeSampleRate(
        device->dev, &current_rate, 8000000.0);
    delay_ms(1200);
    double after_8_param = params->devParams->fsFreq.fsHz;
    double after_8_device = device->rspDuoSampleFreq;
    sdrplay_api_ErrT to_6 = sdrplay_api_SwapRspDuoDualTunerModeSampleRate(
        device->dev, &current_rate, 6000000.0);
    delay_ms(1200);
    fprintf(stderr,
            "MODE_DUAL_RATE_RESULT to8=%d to6=%d current=%.0f param8=%.0f "
            "device8=%.0f param6=%.0f device6=%.0f a_samples=%llu b_samples=%llu "
            "a_resets=%u b_resets=%u mode_events=%u last_mode_event=%u\n",
            to_8, to_6, current_rate, after_8_param, after_8_device,
            params->devParams->fsFreq.fsHz, device->rspDuoSampleFreq,
            atomic_load(&metrics.samples[0]), atomic_load(&metrics.samples[1]),
            atomic_load(&metrics.resets[0]), atomic_load(&metrics.resets[1]),
            atomic_load(&metrics.mode_events), atomic_load(&metrics.last_mode_event));
    if (atomic_load(&metrics.samples[0]) == 0u ||
        atomic_load(&metrics.samples[1]) == 0u) status = sdrplay_api_Fail;
    return finish(device, 1, status);
}

static int run_mode_swap(sdrplay_api_DeviceT *device)
{
    device->tuner = sdrplay_api_Tuner_A;
    device->rspDuoMode = sdrplay_api_RspDuoMode_Single_Tuner;
    device->rspDuoSampleFreq = 0.0;
    sdrplay_api_ErrT status = sdrplay_api_SelectDevice(device);
    sdrplay_api_DeviceParamsT *params = NULL;
    if (status == sdrplay_api_Success)
        status = sdrplay_api_GetDeviceParams(device->dev, &params);
    if (status != sdrplay_api_Success || params == NULL || params->devParams == NULL ||
        params->rxChannelA == NULL) return finish(device, 0, status);
    params->devParams->fsFreq.fsHz = 2048000.0;
    params->devParams->mode = sdrplay_api_BULK;
    configure_channel(params->rxChannelA, 853712500.0, 40, 0u);
    params->rxChannelA->tunerParams.ifType = sdrplay_api_IF_Zero;
    probe_metrics metrics = {0};
    sdrplay_api_CallbackFnsT callbacks = {
        .StreamACbFn = stream_a, .StreamBCbFn = stream_b,
        .EventCbFn = event_callback
    };
    status = sdrplay_api_Init(device->dev, &callbacks, &metrics);
    if (status != sdrplay_api_Success) return finish(device, 0, status);
    delay_ms(1200);
    sdrplay_api_ErrT to_dual = sdrplay_api_SwapRspDuoMode(
        device, &params, sdrplay_api_RspDuoMode_Dual_Tuner, 6000000.0,
        sdrplay_api_Tuner_Both, sdrplay_api_BW_1_536, sdrplay_api_IF_1_620,
        sdrplay_api_RspDuo_AMPORT_2);
    delay_ms(1500);
    int dual_mode = device->rspDuoMode;
    int dual_tuner = device->tuner;
    void *dual_a = params == NULL ? NULL : (void *)params->rxChannelA;
    void *dual_b = params == NULL ? NULL : (void *)params->rxChannelB;
    sdrplay_api_ErrT to_single = sdrplay_api_SwapRspDuoMode(
        device, &params, sdrplay_api_RspDuoMode_Single_Tuner, 2048000.0,
        sdrplay_api_Tuner_A, sdrplay_api_BW_1_536, sdrplay_api_IF_Zero,
        sdrplay_api_RspDuo_AMPORT_2);
    delay_ms(1500);
    fprintf(stderr,
            "MODE_TRANSITION_RESULT to_dual=%d to_single=%d dual_mode=%d "
            "dual_tuner=%d dual_a=%p dual_b=%p final_mode=%d final_tuner=%d "
            "final_a=%p final_b=%p a_samples=%llu b_samples=%llu a_resets=%u "
            "b_resets=%u mode_events=%u last_mode_event=%u\n",
            to_dual, to_single, dual_mode, dual_tuner, dual_a, dual_b,
            device->rspDuoMode, device->tuner,
            params == NULL ? NULL : (void *)params->rxChannelA,
            params == NULL ? NULL : (void *)params->rxChannelB,
            atomic_load(&metrics.samples[0]), atomic_load(&metrics.samples[1]),
            atomic_load(&metrics.resets[0]), atomic_load(&metrics.resets[1]),
            atomic_load(&metrics.mode_events), atomic_load(&metrics.last_mode_event));
    if (atomic_load(&metrics.samples[0]) == 0u) status = sdrplay_api_Fail;
    return finish(device, 1, status);
}

static int run_mode_transition_once(sdrplay_api_DeviceT *device, int to_dual)
{
    device->tuner = to_dual ? sdrplay_api_Tuner_A : sdrplay_api_Tuner_Both;
    device->rspDuoMode = to_dual ? sdrplay_api_RspDuoMode_Single_Tuner :
                                   sdrplay_api_RspDuoMode_Dual_Tuner;
    device->rspDuoSampleFreq = to_dual ? 0.0 : 6000000.0;
    sdrplay_api_ErrT status = sdrplay_api_SelectDevice(device);
    sdrplay_api_DeviceParamsT *params = NULL;
    if (status == sdrplay_api_Success)
        status = sdrplay_api_GetDeviceParams(device->dev, &params);
    if (status != sdrplay_api_Success || params == NULL || params->devParams == NULL ||
        params->rxChannelA == NULL || (!to_dual && params->rxChannelB == NULL))
        return finish(device, 0, status);
    params->devParams->fsFreq.fsHz = to_dual ? 2048000.0 : 6000000.0;
    params->devParams->mode = sdrplay_api_BULK;
    configure_channel(params->rxChannelA, 853712500.0, 40, 0u);
    if (to_dual) {
        params->rxChannelA->tunerParams.ifType = sdrplay_api_IF_Zero;
    } else {
        configure_channel(params->rxChannelB, 853862500.0, 55, 3u);
    }
    probe_metrics metrics = {0};
    sdrplay_api_CallbackFnsT callbacks = {
        .StreamACbFn = stream_a, .StreamBCbFn = stream_b,
        .EventCbFn = event_callback
    };
    status = sdrplay_api_Init(device->dev, &callbacks, &metrics);
    if (status != sdrplay_api_Success) return finish(device, 0, status);
    delay_ms(1500);
    unsigned long long before_a = atomic_load(&metrics.samples[0]);
    unsigned long long before_b = atomic_load(&metrics.samples[1]);
    unsigned int resets_a = atomic_load(&metrics.resets[0]);
    unsigned int resets_b = atomic_load(&metrics.resets[1]);
    sdrplay_api_ErrT transition = sdrplay_api_SwapRspDuoMode(
        device, &params,
        to_dual ? sdrplay_api_RspDuoMode_Dual_Tuner :
                  sdrplay_api_RspDuoMode_Single_Tuner,
        to_dual ? 6000000.0 : 2048000.0,
        to_dual ? sdrplay_api_Tuner_Both : sdrplay_api_Tuner_A,
        sdrplay_api_BW_1_536,
        to_dual ? sdrplay_api_IF_1_620 : sdrplay_api_IF_Zero,
        sdrplay_api_RspDuo_AMPORT_2);
    delay_ms(300);
    unsigned long long stopped_a = atomic_load(&metrics.samples[0]);
    unsigned long long stopped_b = atomic_load(&metrics.samples[1]);
    delay_ms(500);
    unsigned long long paused_a = atomic_load(&metrics.samples[0]);
    unsigned long long paused_b = atomic_load(&metrics.samples[1]);
    sdrplay_api_ErrT reinit = sdrplay_api_AlreadyInitialised;
    if (transition == sdrplay_api_Success && paused_a == stopped_a &&
        paused_b == stopped_b)
        reinit = sdrplay_api_Init(device->dev, &callbacks, &metrics);
    delay_ms(2500);
    sdrplay_api_DeviceParamsT *queried = NULL;
    sdrplay_api_ErrT get = sdrplay_api_GetDeviceParams(device->dev, &queried);
    fprintf(stderr,
            "MODE_TRANSITION_ONCE direction=%s result=%d reinit=%d get=%d mode=%d tuner=%d "
            "sample_freq=%.0f param_freq=%.0f passed_params=%p queried_params=%p "
            "a=%p b=%p a_before=%llu a_stopped=%llu a_paused=%llu a_after=%llu "
            "b_before=%llu b_stopped=%llu b_paused=%llu b_after=%llu "
            "a_resets_before=%u a_resets_after=%u b_resets_before=%u "
            "b_resets_after=%u mode_events=%u last_mode_event=%u\n",
            to_dual ? "to-dual" : "to-single", transition, reinit, get,
            device->rspDuoMode, device->tuner, device->rspDuoSampleFreq,
            params == NULL || params->devParams == NULL ? 0.0 :
                params->devParams->fsFreq.fsHz,
            (void *)params, (void *)queried,
            params == NULL ? NULL : (void *)params->rxChannelA,
            params == NULL ? NULL : (void *)params->rxChannelB,
            before_a, stopped_a, paused_a, atomic_load(&metrics.samples[0]), before_b,
            stopped_b, paused_b, atomic_load(&metrics.samples[1]), resets_a,
            atomic_load(&metrics.resets[0]), resets_b,
            atomic_load(&metrics.resets[1]), atomic_load(&metrics.mode_events),
            atomic_load(&metrics.last_mode_event));
    unsigned long long after_a = atomic_load(&metrics.samples[0]);
    unsigned long long after_b = atomic_load(&metrics.samples[1]);
    if (transition != sdrplay_api_Success || get != sdrplay_api_Success ||
        after_a <= before_a || (to_dual && after_b <= before_b) ||
        (!to_dual && after_b != stopped_b))
        status = sdrplay_api_Fail;
    return finish(device, 1, status);
}

int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "dual") != 0 &&
        strcmp(argv[1], "dual-controls") != 0 &&
        strcmp(argv[1], "dual-init") != 0 && strcmp(argv[1], "dual-a") != 0 &&
        strcmp(argv[1], "dual-b") != 0 && strcmp(argv[1], "swap") != 0 &&
        strcmp(argv[1], "dual-rate-swap") != 0 &&
        strcmp(argv[1], "mode-swap") != 0 &&
        strcmp(argv[1], "mode-to-dual") != 0 &&
        strcmp(argv[1], "mode-to-single") != 0)) {
        fprintf(stderr, "usage: %s dual|dual-controls|dual-init|dual-a|dual-b|swap|dual-rate-swap|mode-swap|mode-to-dual|mode-to-single\n", argv[0]);
        return EXIT_FAILURE;
    }
    sdrplay_api_ErrT status = sdrplay_api_Open();
    sdrplay_api_DeviceT devices[SDRPLAY_MAX_DEVICES];
    unsigned int count = 0u;
    if (status == sdrplay_api_Success)
        status = sdrplay_api_GetDevices(devices, &count, SDRPLAY_MAX_DEVICES);
    if (status != sdrplay_api_Success || count == 0u ||
        devices[0].hwVer != SDRPLAY_RSPduo_ID) {
        fprintf(stderr, "MODE_DISCOVERY status=%d count=%u hw=%u\n", status, count,
                count != 0u ? devices[0].hwVer : 0u);
        if (status == sdrplay_api_Success) (void)sdrplay_api_Close();
        return EXIT_FAILURE;
    }
    if (strcmp(argv[1], "swap") == 0) return run_swap(&devices[0]);
    if (strcmp(argv[1], "dual-rate-swap") == 0)
        return run_dual_rate_swap(&devices[0]);
    if (strcmp(argv[1], "mode-swap") == 0) return run_mode_swap(&devices[0]);
    if (strcmp(argv[1], "mode-to-dual") == 0)
        return run_mode_transition_once(&devices[0], 1);
    if (strcmp(argv[1], "mode-to-single") == 0)
        return run_mode_transition_once(&devices[0], 0);
    unsigned int update_mask = strcmp(argv[1], "dual-init") == 0 ? 0u :
                               strcmp(argv[1], "dual-a") == 0 ? 1u :
                               strcmp(argv[1], "dual-b") == 0 ? 2u : 3u;
    return run_dual(&devices[0], update_mask,
                    strcmp(argv[1], "dual-controls") == 0);
}
