/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <sdrplay_api.h>

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    atomic_ullong samples;
    atomic_uint callbacks;
    atomic_uint resets;
} gain_metrics;

static void stream_a(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params,
                     unsigned int samples, unsigned int reset, void *opaque)
{
    (void)xi;
    (void)xq;
    (void)params;
    gain_metrics *metrics = opaque;
    atomic_fetch_add(&metrics->samples, samples);
    atomic_fetch_add(&metrics->callbacks, 1u);
    if (reset != 0u) atomic_fetch_add(&metrics->resets, 1u);
}

static void delay_ms(long milliseconds)
{
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000L
    };
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
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

static unsigned int environment_flag(const char *name)
{
    const char *value = getenv(name);
    if (value == NULL || strcmp(value, "0") == 0) return 0u;
    if (strcmp(value, "1") == 0) return 1u;
    fprintf(stderr, "GAIN_PROBE_INVALID_FLAG name=%s value=%s\n", name, value);
    exit(EXIT_FAILURE);
}

static unsigned int maximum_lna_state(uint64_t frequency,
                                      unsigned int am_port_1)
{
    if (frequency < 60000000u && am_port_1 != 0u) return 4u;
    if (frequency < 60000000u) return 6u;
    if (frequency < 420000000u) return 9u;
    if (frequency < 1000000000u) return 9u;
    return 8u;
}

static unsigned long long realtime_microseconds(void)
{
    struct timespec now = {0};
    (void)clock_gettime(CLOCK_REALTIME, &now);
    return (unsigned long long)now.tv_sec * 1000000u +
           (unsigned long long)now.tv_nsec / 1000u;
}

static int cleanup(sdrplay_api_DeviceT *device, int initialized,
                   sdrplay_api_ErrT status)
{
    sdrplay_api_ErrT uninit = initialized ? sdrplay_api_Uninit(device->dev) :
                                           sdrplay_api_Success;
    sdrplay_api_ErrT release = sdrplay_api_ReleaseDevice(device);
    sdrplay_api_ErrT close = sdrplay_api_Close();
    fprintf(stderr, "GAIN_PROBE_CLEANUP status=%d uninit=%d release=%d close=%d\n",
            status, uninit, release, close);
    return status == sdrplay_api_Success && uninit == sdrplay_api_Success &&
           release == sdrplay_api_Success && close == sdrplay_api_Success ?
           EXIT_SUCCESS : EXIT_FAILURE;
}

int main(int argc, char **argv)
{
    if (argc != 4 || (strcmp(argv[1], "A") != 0 && strcmp(argv[1], "B") != 0)) {
        fprintf(stderr, "usage: %s A|B frequency_hz lna_state|all\n", argv[0]);
        return EXIT_FAILURE;
    }
    char *frequency_end = NULL;
    char *state_end = NULL;
    unsigned long long frequency = strtoull(argv[2], &frequency_end, 10);
    int sweep = strcmp(argv[3], "all") == 0;
    unsigned long state = sweep ? 0u : strtoul(argv[3], &state_end, 10);
    if (frequency_end == argv[2] || *frequency_end != '\0' ||
        frequency < 1000u || frequency > UINT32_MAX ||
        (!sweep && (state_end == argv[3] || *state_end != '\0' ||
                    state > 255u))) {
        fputs("invalid frequency or LNA state\n", stderr);
        return EXIT_FAILURE;
    }

    sdrplay_api_TunerSelectT tuner = strcmp(argv[1], "B") == 0 ?
        sdrplay_api_Tuner_B : sdrplay_api_Tuner_A;
    sdrplay_api_ErrT status = sdrplay_api_Open();
    sdrplay_api_DeviceT devices[SDRPLAY_MAX_DEVICES];
    unsigned int count = 0u;
    if (status == sdrplay_api_Success)
        status = sdrplay_api_GetDevices(devices, &count, SDRPLAY_MAX_DEVICES);
    if (status != sdrplay_api_Success || count == 0u ||
        devices[0].hwVer != SDRPLAY_RSPduo_ID) {
        fprintf(stderr, "GAIN_PROBE_DISCOVERY status=%d count=%u\n", status, count);
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
    channel->tunerParams.rfFreq.rfHz = (double)frequency;
    channel->tunerParams.bwType = sdrplay_api_BW_1_536;
    channel->tunerParams.ifType = sdrplay_api_IF_Zero;
    channel->tunerParams.loMode = sdrplay_api_LO_Auto;
    channel->tunerParams.gain.gRdB = 45;
    channel->tunerParams.gain.LNAstate = 0u;
    channel->ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE;
    unsigned int rf_notch = environment_flag("OPENRSP_GAIN_PROBE_RF_NOTCH");
    unsigned int dab_notch = environment_flag("OPENRSP_GAIN_PROBE_DAB_NOTCH");
    unsigned int external_reference =
        environment_flag("OPENRSP_GAIN_PROBE_EXTERNAL_REFERENCE");
    unsigned int bias_tee = environment_flag("OPENRSP_GAIN_PROBE_BIAS_TEE");
    unsigned int am_port_1 = environment_flag("OPENRSP_GAIN_PROBE_AM_PORT_1");
    unsigned int am_notch = environment_flag("OPENRSP_GAIN_PROBE_AM_NOTCH");
    channel->rspDuoTunerParams.rfNotchEnable = (unsigned char)rf_notch;
    channel->rspDuoTunerParams.rfDabNotchEnable = (unsigned char)dab_notch;
    channel->rspDuoTunerParams.biasTEnable = (unsigned char)bias_tee;
    channel->rspDuoTunerParams.tuner1AmPortSel = am_port_1 != 0u ?
        sdrplay_api_RspDuo_AMPORT_1 : sdrplay_api_RspDuo_AMPORT_2;
    channel->rspDuoTunerParams.tuner1AmNotchEnable = (unsigned char)am_notch;
    params->devParams->rspDuoParams.extRefOutputEn = (int)external_reference;

    gain_metrics metrics = {0};
    sdrplay_api_CallbackFnsT callbacks = {.StreamACbFn = stream_a};
    status = sdrplay_api_Init(devices[0].dev, &callbacks, &metrics);
    if (status != sdrplay_api_Success) return cleanup(&devices[0], 0, status);
    delay_ms(environment_delay_ms("OPENRSP_GAIN_PROBE_SETTLE_MS", 2000));
    fprintf(stderr, "GAIN_PROBE_INIT_END timestamp_us=%llu\n",
            realtime_microseconds());
    fflush(stderr);
    unsigned int first_state = sweep ? 0u : (unsigned int)state;
    unsigned int last_state = sweep ? maximum_lna_state(frequency, am_port_1) :
                                      (unsigned int)state;
    sdrplay_api_ErrT update = sdrplay_api_Success;
    for (unsigned int current = first_state; current <= last_state; current++) {
        channel->tunerParams.gain.LNAstate = (unsigned char)current;
        fprintf(stderr, "GAIN_PROBE_UPDATE_BEGIN timestamp_us=%llu lna=%u\n",
                realtime_microseconds(), current);
        fflush(stderr);
        update = sdrplay_api_Update(devices[0].dev, tuner,
                                    sdrplay_api_Update_Tuner_Gr, 0u);
        fprintf(stderr,
                "GAIN_PROBE_UPDATE_END timestamp_us=%llu lna=%u result=%d\n",
                realtime_microseconds(), current, update);
        fflush(stderr);
        if (update != sdrplay_api_Success) break;
        delay_ms(environment_delay_ms("OPENRSP_GAIN_PROBE_HOLD_MS", 500));
    }
    fprintf(stderr,
            "GAIN_PROBE_RESULT tuner=%s frequency=%llu lna=%lu update=%d "
            "rf_notch=%u dab_notch=%u external_reference=%u "
            "bias_tee=%u am_port_1=%u am_notch=%u "
            "samples=%llu callbacks=%u resets=%u\n",
            argv[1], frequency, sweep ? last_state : state, update,
            rf_notch, dab_notch, external_reference,
            bias_tee, am_port_1, am_notch,
            atomic_load(&metrics.samples), atomic_load(&metrics.callbacks),
            atomic_load(&metrics.resets));
    if (update != sdrplay_api_Success || atomic_load(&metrics.samples) == 0u)
        status = sdrplay_api_Fail;
    return cleanup(&devices[0], 1, status);
}
