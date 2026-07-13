/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sdrplay_api_compat.h"
#include "daemon_backend.h"


#include <pthread.h>
#include <stdatomic.h>
#include <math.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int api_open;
static pthread_mutex_t hardware_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int selected;
    int initialized;
    sdrplay_api_DevParamsT dev_params;
    sdrplay_api_RxChannelParamsT channel_a;
    sdrplay_api_RxChannelParamsT channel_b;
    sdrplay_api_DeviceParamsT params;
    openrsp_daemon_backend *backend;
    pthread_t agc_thread;
    int thread_started;
    int agc_thread_started;
    sdrplay_api_CallbackFnsT callbacks;
    void *callback_context;
    unsigned int sample_number;
    unsigned int first_callback;
    atomic_uint pending_gr_changed;
    atomic_uint pending_rf_changed;
    atomic_uint pending_fs_changed;
    atomic_int stream_state;
    atomic_uint callback_samples;
    atomic_uint agc_peak;
    atomic_int agc_stop;
} compat_device_context;

static compat_device_context rspduo;
static sdrplay_api_ErrorInfoT last_error;

static int consume_one(atomic_uint *pending)
{
    unsigned int value = atomic_load(pending);
    while (value > 0u) {
        if (atomic_compare_exchange_weak(pending, &value, value - 1u)) return 1;
    }
    return 0;
}

static int clamp_int(int value, int minimum, int maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

/* RSPduo 50-ohm LNA gain-reduction tables from the SDRplay API specification. */
static int rspduo_lna_gain_reduction(double rf_hz, unsigned int state, int *reduction_db)
{
    static const int below_60mhz[] = {0, 6, 12, 18, 37, 42, 61};
    static const int below_420mhz[] = {0, 6, 12, 18, 20, 26, 32, 38, 57, 62};
    static const int below_1ghz[] = {0, 7, 13, 19, 20, 27, 33, 39, 45, 64};
    static const int below_2ghz[] = {0, 6, 12, 20, 26, 32, 38, 43, 62};
    const int *table;
    size_t count;

    if (rf_hz < 60000000.0) {
        table = below_60mhz;
        count = sizeof(below_60mhz) / sizeof(below_60mhz[0]);
    } else if (rf_hz < 420000000.0) {
        table = below_420mhz;
        count = sizeof(below_420mhz) / sizeof(below_420mhz[0]);
    } else if (rf_hz < 1000000000.0) {
        table = below_1ghz;
        count = sizeof(below_1ghz) / sizeof(below_1ghz[0]);
    } else if (rf_hz <= 2000000000.0) {
        table = below_2ghz;
        count = sizeof(below_2ghz) / sizeof(below_2ghz[0]);
    } else {
        return -1;
    }
    if (state >= count) return -1;
    *reduction_db = table[state];
    return 0;
}

static void fill_radio_config(const compat_device_context *device, openrsp_radio_config *config)
{
    double correction = 1.0 + device->dev_params.ppm / 1000000.0;
    memset(config, 0, sizeof(*config));
    config->sample_rate_hz = (uint32_t)device->dev_params.fsFreq.fsHz;
    config->center_frequency_hz = (uint32_t)(device->channel_a.tunerParams.rfFreq.rfHz /
                                             correction);
    config->bandwidth_hz = (uint32_t)device->channel_a.tunerParams.bwType * 1000u;
    config->if_frequency_hz = (int32_t)device->channel_a.tunerParams.ifType * 1000;
    config->gain_reduction_db = device->channel_a.tunerParams.gain.gRdB;
    config->lna_state = device->channel_a.tunerParams.gain.LNAstate;
    config->agc_mode = device->channel_a.ctrlParams.agc.enable;
    config->agc_setpoint_dbfs = device->channel_a.ctrlParams.agc.setPoint_dBfs;
}

static uint32_t protocol_change_flags(sdrplay_api_ReasonForUpdateT reason)
{
    uint32_t flags = 0u;
    if ((reason & sdrplay_api_Update_Dev_Fs) != 0u) flags |= OPENRSP_CHANGE_SAMPLE_RATE;
    if ((reason & (sdrplay_api_Update_Tuner_Frf | sdrplay_api_Update_Dev_Ppm)) != 0u)
        flags |= OPENRSP_CHANGE_RF;
    if ((reason & sdrplay_api_Update_Tuner_BwType) != 0u) flags |= OPENRSP_CHANGE_BANDWIDTH;
    if ((reason & sdrplay_api_Update_Tuner_IfType) != 0u) flags |= OPENRSP_CHANGE_IF;
    if ((reason & sdrplay_api_Update_Tuner_Gr) != 0u) flags |= OPENRSP_CHANGE_GAIN;
    if ((reason & sdrplay_api_Update_Ctrl_Agc) != 0u) flags |= OPENRSP_CHANGE_AGC;
    return flags;
}

static sdrplay_api_ErrT validate_update(const compat_device_context *device,
                                        sdrplay_api_ReasonForUpdateT reason,
                                        sdrplay_api_ReasonForUpdateExtension1T extension)
{
    const uint32_t other_models =
        sdrplay_api_Update_Rsp1a_BiasTControl |
        sdrplay_api_Update_Rsp1a_RfNotchControl |
        sdrplay_api_Update_Rsp1a_RfDabNotchControl |
        sdrplay_api_Update_Rsp2_BiasTControl |
        sdrplay_api_Update_Rsp2_AmPortSelect |
        sdrplay_api_Update_Rsp2_AntennaControl |
        sdrplay_api_Update_Rsp2_RfNotchControl |
        sdrplay_api_Update_Rsp2_ExtRefControl;
    const uint32_t unsupported_duo_hardware =
        sdrplay_api_Update_RspDuo_ExtRefControl |
        sdrplay_api_Update_RspDuo_BiasTControl |
        sdrplay_api_Update_RspDuo_AmPortSelect |
        sdrplay_api_Update_RspDuo_Tuner1AmNotchControl |
        sdrplay_api_Update_RspDuo_RfNotchControl |
        sdrplay_api_Update_RspDuo_RfDabNotchControl;

    if ((extension & ~0x7fu) != 0u) return sdrplay_api_InvalidParam;
    if (extension != sdrplay_api_Update_Ext1_None)
        return sdrplay_api_InvalidMode;
    if ((reason & other_models) != 0u) return sdrplay_api_HwVerError;
    if ((reason & unsupported_duo_hardware) != 0u)
        return sdrplay_api_InvalidMode;
    if ((reason & (sdrplay_api_Update_Master_Spare_1 |
                   sdrplay_api_Update_Master_Spare_2)) != 0u)
        return sdrplay_api_InvalidParam;
    if ((reason & sdrplay_api_Update_Dev_Ppm) != 0u &&
        (!isfinite(device->dev_params.ppm) || device->dev_params.ppm < -300.0 ||
         device->dev_params.ppm > 300.0))
        return sdrplay_api_OutOfRange;
    if ((reason & sdrplay_api_Update_Tuner_LoMode) != 0u &&
        device->channel_a.tunerParams.loMode != sdrplay_api_LO_Auto)
        return sdrplay_api_InvalidMode;
    if ((reason & sdrplay_api_Update_Ctrl_Decimation) != 0u) {
        const sdrplay_api_DecimationT *decimation = &device->channel_a.ctrlParams.decimation;
        if (decimation->enable != 0u || decimation->decimationFactor != 1u)
            return sdrplay_api_InvalidMode;
    }
    if ((reason & sdrplay_api_Update_Ctrl_AdsbMode) != 0u &&
        device->channel_a.ctrlParams.adsbMode != sdrplay_api_ADSB_DECIMATION)
        return sdrplay_api_InvalidMode;
    return sdrplay_api_Success;
}

static int apply_rspduo_gain_locked(compat_device_context *device)
{
    sdrplay_api_GainT *gain = &device->channel_a.tunerParams.gain;
    int lna_reduction;
    if (gain->gRdB < 20 || gain->gRdB > 59 ||
        rspduo_lna_gain_reduction(device->channel_a.tunerParams.rfFreq.rfHz,
                                  gain->LNAstate, &lna_reduction) < 0) return -1;

    int total_reduction = gain->gRdB + lna_reduction;
    int current_gain = clamp_int(105 - total_reduction, -20, 105);
    openrsp_radio_config config;
    fill_radio_config(device, &config);
    int result = openrsp_daemon_backend_update(device->backend, &config,
                                                OPENRSP_CHANGE_GAIN);
    if (result >= 0) {
        gain->gainVals.curr = (float)current_gain;
        gain->gainVals.max = 85.0f;
        gain->gainVals.min = -18.0f;
    }
    return result;
}

static unsigned int agc_period_ms(sdrplay_api_AgcControlT mode)
{
    if (mode == sdrplay_api_AGC_100HZ) return 10u;
    if (mode == sdrplay_api_AGC_50HZ || mode == sdrplay_api_AGC_CTRL_EN) return 20u;
    return 200u;
}

static void *agc_thread_main(void *opaque)
{
    compat_device_context *device = opaque;
    while (!atomic_load(&device->agc_stop)) {
        sdrplay_api_AgcControlT mode = device->channel_a.ctrlParams.agc.enable;
        unsigned int period_ms = agc_period_ms(mode);
        const struct timespec delay = {
            .tv_sec = period_ms / 1000u,
            .tv_nsec = (long)(period_ms % 1000u) * 1000000L
        };
        nanosleep(&delay, NULL);
        unsigned int peak = atomic_exchange(&device->agc_peak, 0u);
        if (mode == sdrplay_api_AGC_DISABLE || peak == 0u || !device->initialized) continue;

        double level_dbfs = 20.0 * log10((double)peak / 32767.0);
        int target = clamp_int(device->channel_a.ctrlParams.agc.setPoint_dBfs, -60, -20);
        double error = level_dbfs - (double)target;
        int step = 0;
        if (error > 6.0) step = 3;
        else if (error > 1.5) step = 1;
        else if (error < -6.0) step = -2;
        else if (error < -1.5) step = -1;
        if (step == 0) continue;

        pthread_mutex_lock(&hardware_lock);
        int old_reduction = device->channel_a.tunerParams.gain.gRdB;
        int new_reduction = clamp_int(old_reduction + step, 20, 59);
        if (new_reduction != old_reduction) {
            device->channel_a.tunerParams.gain.gRdB = new_reduction;
            if (apply_rspduo_gain_locked(device) >= 0) {
                atomic_fetch_add(&device->pending_gr_changed, 1u);
            } else {
                device->channel_a.tunerParams.gain.gRdB = old_reduction;
            }
        }
        pthread_mutex_unlock(&hardware_lock);
    }
    return NULL;
}

static void daemon_stream_callback(const int16_t *interleaved, size_t samples, void *opaque)
{
    compat_device_context *device = opaque;
    atomic_store(&device->stream_state, 1);
    short *xi = malloc((size_t)samples * sizeof(*xi));
    short *xq = malloc((size_t)samples * sizeof(*xq));
    if (xi == NULL || xq == NULL) {
        free(xi);
        free(xq);
        atomic_store(&device->stream_state, -1);
        return;
    }
    unsigned int peak = 0u;
    for (size_t index = 0; index < samples; ++index) {
        xi[index] = interleaved[index * 2u];
        xq[index] = interleaved[index * 2u + 1u];
        unsigned int abs_i = (unsigned int)abs(xi[index]);
        unsigned int abs_q = (unsigned int)abs(xq[index]);
        unsigned int sample_peak = abs_i > abs_q ? abs_i : abs_q;
        if (sample_peak > peak) peak = sample_peak;
    }
    unsigned int observed = atomic_load(&device->agc_peak);
    while (peak > observed &&
           !atomic_compare_exchange_weak(&device->agc_peak, &observed, peak)) {}
    int gr_changed = consume_one(&device->pending_gr_changed);
    int rf_changed = consume_one(&device->pending_rf_changed);
    int fs_changed = consume_one(&device->pending_fs_changed);
    for (size_t offset = 0; offset < samples;) {
        unsigned int chunk = (unsigned int)(samples - offset);
        unsigned int callback_samples = atomic_load(&device->callback_samples);
        if (chunk > callback_samples) chunk = callback_samples;
        sdrplay_api_StreamCbParamsT params = {
            .firstSampleNum = device->sample_number,
            .grChanged = offset == 0u ? gr_changed : 0,
            .rfChanged = offset == 0u ? rf_changed : 0,
            .fsChanged = offset == 0u ? fs_changed : 0,
            .numSamples = chunk
        };
        device->callbacks.StreamACbFn(xi + offset, xq + offset, &params, chunk,
                                      device->first_callback, device->callback_context);
        device->first_callback = 0;
        device->sample_number += chunk;
        offset += chunk;
    }
    free(xi);
    free(xq);
}

static void emit_update_ack(compat_device_context *device, int fs_changed,
                            int rf_changed, int gr_changed)
{
    short empty_sample = 0;
    sdrplay_api_StreamCbParamsT params = {
        .firstSampleNum = device->sample_number,
        .grChanged = gr_changed,
        .rfChanged = rf_changed,
        .fsChanged = fs_changed,
        .numSamples = 0u
    };
    device->callbacks.StreamACbFn(&empty_sample, &empty_sample, &params, 0u, 0u,
                                  device->callback_context);
}

_Static_assert(sizeof(sdrplay_api_DeviceT) == 96, "DeviceT ABI mismatch");
_Static_assert(sizeof(sdrplay_api_DevParamsT) == 64, "DevParamsT ABI mismatch");
_Static_assert(sizeof(sdrplay_api_TunerParamsT) == 72, "TunerParamsT ABI mismatch");
_Static_assert(sizeof(sdrplay_api_ControlParamsT) == 32, "ControlParamsT ABI mismatch");
_Static_assert(sizeof(sdrplay_api_RxChannelParamsT) == 144, "RxChannelParamsT ABI mismatch");
_Static_assert(sizeof(sdrplay_api_DeviceParamsT) == 24, "DeviceParamsT ABI mismatch");
_Static_assert(sizeof(sdrplay_api_CallbackFnsT) == 24, "CallbackFnsT ABI mismatch");

static void reset_parameters(void)
{
    memset(&rspduo, 0, sizeof(rspduo));
    memset(&last_error, 0, sizeof(last_error));
    atomic_init(&rspduo.pending_gr_changed, 0u);
    atomic_init(&rspduo.pending_rf_changed, 0u);
    atomic_init(&rspduo.pending_fs_changed, 0u);
    atomic_init(&rspduo.stream_state, 0);
    atomic_init(&rspduo.agc_peak, 0u);
    atomic_init(&rspduo.agc_stop, 0);
    rspduo.params.devParams = &rspduo.dev_params;
    rspduo.params.rxChannelA = &rspduo.channel_a;
    rspduo.params.rxChannelB = &rspduo.channel_b;
    rspduo.dev_params.fsFreq.fsHz = 2000000.0;
    rspduo.dev_params.mode = sdrplay_api_ISOCH;
    rspduo.channel_a.tunerParams.bwType = sdrplay_api_BW_0_200;
    rspduo.channel_a.tunerParams.ifType = sdrplay_api_IF_Zero;
    rspduo.channel_a.tunerParams.loMode = sdrplay_api_LO_Auto;
    rspduo.channel_a.tunerParams.gain.gRdB = 50;
    rspduo.channel_a.tunerParams.gain.LNAstate = 0;
    rspduo.channel_a.tunerParams.gain.gainVals.curr = 52.0f;
    rspduo.channel_a.tunerParams.gain.gainVals.max = 102.0f;
    rspduo.channel_a.tunerParams.gain.gainVals.min = 0.0f;
    rspduo.channel_a.tunerParams.gain.minGr = sdrplay_api_NORMAL_MIN_GR;
    rspduo.channel_a.tunerParams.rfFreq.rfHz = 200000000.0;
    rspduo.channel_a.ctrlParams.dcOffset.DCenable = 1;
    rspduo.channel_a.ctrlParams.dcOffset.IQenable = 1;
    rspduo.channel_a.ctrlParams.decimation.decimationFactor = 1;
    rspduo.channel_a.ctrlParams.agc.enable = sdrplay_api_AGC_CTRL_EN;
    rspduo.channel_a.ctrlParams.agc.setPoint_dBfs = -60;
    rspduo.channel_b = rspduo.channel_a;
}

sdrplay_api_ErrT sdrplay_api_Open(void)
{
    if (api_open) {
        return sdrplay_api_AlreadyInitialised;
    }
    reset_parameters();
    api_open = 1;
    return sdrplay_api_Success;
}

sdrplay_api_ErrT sdrplay_api_Close(void)
{
    api_open = 0;
    return sdrplay_api_Success;
}

sdrplay_api_ErrT sdrplay_api_ApiVersion(float *apiVer)
{
    if (apiVer == NULL) {
        return sdrplay_api_InvalidParam;
    }
    *apiVer = SDRPLAY_API_VERSION;
    return sdrplay_api_Success;
}

sdrplay_api_ErrT sdrplay_api_LockDeviceApi(void)
{
    return api_open ? sdrplay_api_Success : sdrplay_api_NotInitialised;
}

sdrplay_api_ErrT sdrplay_api_UnlockDeviceApi(void)
{
    return api_open ? sdrplay_api_Success : sdrplay_api_NotInitialised;
}

sdrplay_api_ErrT sdrplay_api_GetDevices(sdrplay_api_DeviceT *devices,
                                         unsigned int *numDevs,
                                         unsigned int maxDevs)
{
    if (!api_open) {
        return sdrplay_api_NotInitialised;
    }
    if (numDevs == NULL || (devices == NULL && maxDevs != 0)) {
        return sdrplay_api_InvalidParam;
    }

    int count = openrsp_daemon_backend_list(NULL, 0);
    if (count < 0) {
        return sdrplay_api_HwError;
    }
    openrsp_device_record *found = count == 0 ? NULL : calloc((size_t)count, sizeof(*found));
    if (count > 0 && found == NULL) {
        return sdrplay_api_OutOfMemError;
    }
    int discovered = openrsp_daemon_backend_list(found, (size_t)count);
    if (discovered < 0) {
        free(found);
        return sdrplay_api_HwError;
    }

    unsigned int written = 0;
    for (int index = 0; index < discovered && written < maxDevs; ++index) {
        if (found[index].product_id != 0x3020u) {
            continue;
        }
        sdrplay_api_DeviceT *device = &devices[written++];
        memset(device, 0, sizeof(*device));
        const char *override = getenv("OPENRSP_SERIAL");
        const char *identity = override != NULL && override[0] != '\0' ? override :
                               (found[index].serial[0] == '\0' ? "OPENRSP0" : found[index].serial);
        (void)snprintf(device->SerNo, sizeof(device->SerNo), "%s", identity);
        device->hwVer = 3u;
        device->tuner = sdrplay_api_Tuner_A;
        device->rspDuoMode = sdrplay_api_RspDuoMode_Single_Tuner;
        device->valid = 1u;
    }
    free(found);
    *numDevs = written;
    return sdrplay_api_Success;
}

const char *sdrplay_api_GetErrorString(sdrplay_api_ErrT err)
{
    static const char *const names[] = {
        "Success", "Fail", "Invalid Param", "Out Of Range", "Gain Update Error",
        "RF Update Error", "Sample Rate Update Error", "Hardware Error", "Aliasing Error",
        "Already Initialised", "Not Initialised", "Not Enabled", "Hardware Version Error",
        "Out Of Memory", "Service Not Responding", "Start Pending", "Stop Pending", "Invalid Mode",
        "Failed Verification 1", "Failed Verification 2", "Failed Verification 3",
        "Failed Verification 4", "Failed Verification 5", "Failed Verification 6",
        "Invalid Service Version"
    };
    return (unsigned int)err < sizeof(names) / sizeof(names[0]) ? names[err] : "Unknown Error";
}

sdrplay_api_ErrorInfoT *sdrplay_api_GetLastError(sdrplay_api_DeviceT *device)
{
    (void)device;
    return &last_error;
}

sdrplay_api_ErrorInfoT *sdrplay_api_GetLastErrorByType(sdrplay_api_DeviceT *device,
                                                        int type, unsigned long long *time)
{
    (void)device;
    (void)type;
    if (time) *time = 0u;
    return &last_error;
}

sdrplay_api_ErrT sdrplay_api_DisableHeartbeat(void)
{
    return api_open ? sdrplay_api_Success : sdrplay_api_NotInitialised;
}

sdrplay_api_ErrT sdrplay_api_DebugEnable(HANDLE dev, sdrplay_api_DbgLvl_t level)
{
    if (!api_open) return sdrplay_api_NotInitialised;
    if (dev != &rspduo || !rspduo.selected) return sdrplay_api_InvalidParam;
    if (level < sdrplay_api_DbgLvl_Disable || level > sdrplay_api_DbgLvl_Message)
        return sdrplay_api_OutOfRange;
    return sdrplay_api_Success;
}

sdrplay_api_ErrT sdrplay_api_SelectDevice(sdrplay_api_DeviceT *device)
{
    if (!api_open) return sdrplay_api_NotInitialised;
    if (device == NULL || device->hwVer != 3u || device->tuner != sdrplay_api_Tuner_A) {
        return sdrplay_api_InvalidParam;
    }
    if (rspduo.selected) return sdrplay_api_AlreadyInitialised;
    rspduo.selected = 1;
    device->dev = &rspduo;
    return sdrplay_api_Success;
}

sdrplay_api_ErrT sdrplay_api_ReleaseDevice(sdrplay_api_DeviceT *device)
{
    if (!api_open) return sdrplay_api_NotInitialised;
    if (device == NULL || device->dev != &rspduo) return sdrplay_api_InvalidParam;
    if (rspduo.initialized) return sdrplay_api_AlreadyInitialised;
    device->dev = NULL;
    rspduo.selected = 0;
    return sdrplay_api_Success;
}

sdrplay_api_ErrT sdrplay_api_GetDeviceParams(HANDLE dev, sdrplay_api_DeviceParamsT **params)
{
    if (!api_open) return sdrplay_api_NotInitialised;
    if (dev != &rspduo || params == NULL || !rspduo.selected) return sdrplay_api_InvalidParam;
    *params = &rspduo.params;
    return sdrplay_api_Success;
}

sdrplay_api_ErrT sdrplay_api_Init(HANDLE dev, sdrplay_api_CallbackFnsT *callbacks, void *context)
{
    if (dev != &rspduo || !rspduo.selected) return sdrplay_api_InvalidParam;
    if (callbacks == NULL || callbacks->StreamACbFn == NULL) return sdrplay_api_InvalidParam;
    if (rspduo.initialized) return sdrplay_api_AlreadyInitialised;
    int result = openrsp_daemon_backend_open(&rspduo.backend, 0u);
    if (result < 0 || rspduo.backend == NULL) return sdrplay_api_HwError;
    openrsp_radio_config config;
    fill_radio_config(&rspduo, &config);
    result = openrsp_daemon_backend_configure(rspduo.backend, &config);
    if (result < 0) {
        openrsp_daemon_backend_close(rspduo.backend);
        rspduo.backend = NULL;
        return sdrplay_api_HwError;
    }
    rspduo.callbacks = *callbacks;
    rspduo.callback_context = context;
    rspduo.sample_number = 0;
    atomic_store(&rspduo.stream_state, 0);
    atomic_store(&rspduo.agc_stop, 0);
    atomic_store(&rspduo.agc_peak, 0u);
    atomic_store(&rspduo.callback_samples,
                 rspduo.dev_params.fsFreq.fsHz > 9216000.0 ? 2016u : 1344u);
    rspduo.first_callback = 1;
    rspduo.initialized = 1;
    if (openrsp_daemon_backend_start(rspduo.backend, daemon_stream_callback, &rspduo) != 0) {
        rspduo.initialized = 0;
        openrsp_daemon_backend_close(rspduo.backend);
        rspduo.backend = NULL;
        return sdrplay_api_StartPending;
    }
    rspduo.thread_started = 1;
    if (pthread_create(&rspduo.agc_thread, NULL, agc_thread_main, &rspduo) != 0) {
        (void)openrsp_daemon_backend_stop(rspduo.backend);
        openrsp_daemon_backend_close(rspduo.backend);
        rspduo.backend = NULL;
        rspduo.thread_started = 0;
        rspduo.initialized = 0;
        return sdrplay_api_StartPending;
    }
    rspduo.agc_thread_started = 1;
    const struct timespec startup_poll = {.tv_sec = 0, .tv_nsec = 10000000L};
    for (int attempt = 0; attempt < 200 && atomic_load(&rspduo.stream_state) == 0; ++attempt) {
        nanosleep(&startup_poll, NULL);
    }
    if (atomic_load(&rspduo.stream_state) != 1) {
        (void)openrsp_daemon_backend_stop(rspduo.backend);
        atomic_store(&rspduo.agc_stop, 1);
        pthread_join(rspduo.agc_thread, NULL);
        openrsp_daemon_backend_close(rspduo.backend);
        rspduo.backend = NULL;
        rspduo.thread_started = 0;
        rspduo.agc_thread_started = 0;
        rspduo.initialized = 0;
        return sdrplay_api_HwError;
    }
    /* The tested RSPduo only starts its bulk endpoint reliably in automatic
     * gain mode.  Apply the caller's requested gain after the first IQ block,
     * when the endpoint is demonstrably running. */
    fill_radio_config(&rspduo, &config);
    if (openrsp_daemon_backend_update(rspduo.backend, &config,
                                      OPENRSP_CHANGE_GAIN) < 0) {
        (void)openrsp_daemon_backend_stop(rspduo.backend);
        atomic_store(&rspduo.agc_stop, 1);
        pthread_join(rspduo.agc_thread, NULL);
        openrsp_daemon_backend_close(rspduo.backend);
        rspduo.backend = NULL;
        rspduo.thread_started = 0;
        rspduo.agc_thread_started = 0;
        rspduo.initialized = 0;
        return sdrplay_api_HwError;
    }
    return sdrplay_api_Success;
}

sdrplay_api_ErrT sdrplay_api_Uninit(HANDLE dev)
{
    if (dev != &rspduo || !rspduo.initialized) return sdrplay_api_NotInitialised;
    atomic_store(&rspduo.agc_stop, 1);
    if (rspduo.agc_thread_started) pthread_join(rspduo.agc_thread, NULL);
    if (rspduo.thread_started) (void)openrsp_daemon_backend_stop(rspduo.backend);
    openrsp_daemon_backend_close(rspduo.backend);
    rspduo.backend = NULL;
    rspduo.thread_started = 0;
    rspduo.agc_thread_started = 0;
    rspduo.initialized = 0;
    return sdrplay_api_Success;
}

sdrplay_api_ErrT sdrplay_api_Update(HANDLE dev, sdrplay_api_TunerSelectT tuner,
                                     sdrplay_api_ReasonForUpdateT reason,
                                     sdrplay_api_ReasonForUpdateExtension1T extension)
{
    if (dev != &rspduo || tuner != sdrplay_api_Tuner_A) return sdrplay_api_InvalidParam;
    if (!rspduo.initialized || rspduo.backend == NULL) return sdrplay_api_NotInitialised;
    sdrplay_api_ErrT validation = validate_update(&rspduo, reason, extension);
    if (validation != sdrplay_api_Success) return validation;
    int result = 0;
    /* The vendor API reports a PPM update through fsChanged, even though the
     * correction also requires retuning the synthesizer in our backend. */
    int fs_changed = (reason & (sdrplay_api_Update_Dev_Fs |
                                sdrplay_api_Update_Dev_Ppm)) != 0u;
    int rf_changed = (reason & sdrplay_api_Update_Tuner_Frf) != 0u;
    int gr_changed = (reason & sdrplay_api_Update_Tuner_Gr) != 0u || rf_changed;
    pthread_mutex_lock(&hardware_lock);
    if (fs_changed) {
        atomic_store(&rspduo.callback_samples,
                     rspduo.dev_params.fsFreq.fsHz > 9216000.0 ? 2016u : 1344u);
    }
    if ((reason & sdrplay_api_Update_Tuner_BwType) != 0u) {
    }
    if ((reason & sdrplay_api_Update_Tuner_IfType) != 0u) {
    }
    if ((reason & sdrplay_api_Update_Ctrl_Agc) != 0u) {
        sdrplay_api_AgcControlT mode = rspduo.channel_a.ctrlParams.agc.enable;
        if (mode < sdrplay_api_AGC_DISABLE || mode > sdrplay_api_AGC_CTRL_EN ||
            rspduo.channel_a.ctrlParams.agc.setPoint_dBfs < -60 ||
            rspduo.channel_a.ctrlParams.agc.setPoint_dBfs > -20) {
            result = -1;
        } else {
            atomic_store(&rspduo.agc_peak, 0u);
        }
    }
    if (result >= 0) {
        openrsp_radio_config config;
        fill_radio_config(&rspduo, &config);
        result = openrsp_daemon_backend_update(rspduo.backend, &config,
                                                protocol_change_flags(reason));
    }
    if (result >= 0) {
        if (gr_changed) {
            int lna_reduction = 0;
            sdrplay_api_GainT *gain = &rspduo.channel_a.tunerParams.gain;
            if (rspduo_lna_gain_reduction(
                    rspduo.channel_a.tunerParams.rfFreq.rfHz,
                    gain->LNAstate, &lna_reduction) == 0) {
                gain->gainVals.curr = (float)clamp_int(
                    105 - gain->gRdB - lna_reduction, -20, 105);
                gain->gainVals.max = 85.0f;
                gain->gainVals.min = -18.0f;
            }
        }
        if (result > 0) {
            emit_update_ack(&rspduo, fs_changed, rf_changed, gr_changed);
        } else {
            if (fs_changed) atomic_fetch_add(&rspduo.pending_fs_changed, 1u);
            if (rf_changed) atomic_fetch_add(&rspduo.pending_rf_changed, 1u);
            if (gr_changed) atomic_fetch_add(&rspduo.pending_gr_changed, 1u);
        }
    }
    pthread_mutex_unlock(&hardware_lock);
    return result < 0 ? sdrplay_api_HwError : sdrplay_api_Success;
}

sdrplay_api_ErrT sdrplay_api_SwapRspDuoActiveTuner(
    HANDLE dev, sdrplay_api_TunerSelectT *current_tuner,
    sdrplay_api_RspDuo_AmPortSelectT tuner1_am_port)
{
    (void)tuner1_am_port;
    if (dev != &rspduo || current_tuner == NULL) return sdrplay_api_InvalidParam;
    return sdrplay_api_InvalidMode;
}

sdrplay_api_ErrT sdrplay_api_SwapRspDuoDualTunerModeSampleRate(
    HANDLE dev, double *current_sample_rate, double new_sample_rate)
{
    (void)new_sample_rate;
    if (dev != &rspduo || current_sample_rate == NULL) return sdrplay_api_InvalidParam;
    return sdrplay_api_InvalidMode;
}

sdrplay_api_ErrT sdrplay_api_SwapRspDuoMode(
    sdrplay_api_DeviceT *current_device, sdrplay_api_DeviceParamsT **device_params,
    sdrplay_api_RspDuoModeT mode, double sample_rate, sdrplay_api_TunerSelectT tuner,
    sdrplay_api_Bw_MHzT bandwidth, sdrplay_api_If_kHzT if_type,
    sdrplay_api_RspDuo_AmPortSelectT tuner1_am_port)
{
    (void)mode;
    (void)sample_rate;
    (void)tuner;
    (void)bandwidth;
    (void)if_type;
    (void)tuner1_am_port;
    if (current_device == NULL || device_params == NULL) return sdrplay_api_InvalidParam;
    return sdrplay_api_InvalidMode;
}
