/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sdrplay_api_compat.h"
#include "daemon_backend.h"
#include "low_if_dsp.h"


#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static atomic_int api_open;
static pthread_mutex_t hardware_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t decimation_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t device_api_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned int device_api_lock_depth;
static openrsp_daemon_api_lock *daemon_api_lock;

typedef struct {
    openrsp_acquire_request identity;
    unsigned char hw_version;
    int valid;
} compat_discovery_handle;

static compat_discovery_handle discovery_handles[SDRPLAY_MAX_DEVICES];
static unsigned int discovery_handle_count;

#define OPENRSP_EVENT_QUEUE_CAPACITY 32u
#define OPENRSP_STREAM_QUEUE_CAPACITY 8u

typedef struct {
    sdrplay_api_EventT event;
    sdrplay_api_TunerSelectT tuner;
    sdrplay_api_EventParamsT params;
} compat_api_event;

typedef struct {
    size_t samples;
    unsigned int first_sample;
    unsigned int reset;
    int gr_changed;
    int rf_changed;
    int fs_changed;
    short xi[OPENRSP_MAX_IQ_SAMPLES];
    short xq[OPENRSP_MAX_IQ_SAMPLES];
} compat_stream_frame;

typedef struct {
    int selected;
    int initialized;
    openrsp_acquire_request identity;
    uint16_t product_id;
    unsigned char hw_version;
    sdrplay_api_DevParamsT dev_params;
    sdrplay_api_RxChannelParamsT channel_a;
    sdrplay_api_RxChannelParamsT channel_b;
    sdrplay_api_DeviceParamsT params;
    openrsp_daemon_backend *backend;
    short *scratch_i;
    short *scratch_q;
    short *callback_i;
    short *callback_q;
    size_t scratch_capacity;
    compat_stream_frame *stream_queue;
    pthread_t stream_callback_thread;
    int stream_callback_thread_started;
    pthread_mutex_t stream_callback_lock;
    pthread_cond_t stream_callback_ready;
    int stream_callback_stop;
    unsigned int stream_callback_head;
    unsigned int stream_callback_count;
    unsigned int stream_callback_drop_count;
    unsigned int stream_callback_next_sample;
    int stream_callback_seen;
    pthread_t agc_thread;
    int thread_started;
    int agc_thread_started;
    sdrplay_api_CallbackFnsT callbacks;
    void *callback_context;
    atomic_uint sample_number;
    uint32_t last_iq_sequence;
    int iq_sequence_seen;
    unsigned int first_callback;
    atomic_uint pending_gr_changed;
    atomic_uint pending_rf_changed;
    atomic_uint pending_fs_changed;
    atomic_int stream_state;
    atomic_uint callback_samples;
    atomic_uint agc_peak;
    atomic_int agc_stop;
    atomic_int agc_mode;
    atomic_int agc_setpoint;
    atomic_int overload_state;
    atomic_int overload_event_pending;
    atomic_int overload_reported_state;
    atomic_uint decimation_factor;
    openrsp_low_if_dsp low_if_dsp;
    unsigned int decimation_taps;
    unsigned int decimation_position;
    unsigned int decimation_phase;
    double decimation_coefficients[513];
    int16_t decimation_history_i[513];
    int16_t decimation_history_q[513];
    pthread_mutex_t event_lock;
    pthread_cond_t event_ready;
    pthread_cond_t event_idle;
    pthread_t event_thread;
    int event_thread_started;
    int event_stop;
    int event_dispatching;
    unsigned int event_head;
    unsigned int event_count;
    compat_api_event event_queue[OPENRSP_EVENT_QUEUE_CAPACITY];
} compat_device_context;

static compat_device_context rspduo;
static sdrplay_api_ErrorInfoT last_error;
static unsigned long long last_error_time;
static _Thread_local sdrplay_api_ErrorInfoT last_error_view;
static pthread_mutex_t last_error_lock = PTHREAD_MUTEX_INITIALIZER;

static int allocate_stream_buffers(compat_device_context *device)
{
    device->scratch_i = malloc(OPENRSP_MAX_IQ_SAMPLES * sizeof(*device->scratch_i));
    device->scratch_q = malloc(OPENRSP_MAX_IQ_SAMPLES * sizeof(*device->scratch_q));
    device->callback_i = malloc(OPENRSP_MAX_IQ_SAMPLES * sizeof(*device->callback_i));
    device->callback_q = malloc(OPENRSP_MAX_IQ_SAMPLES * sizeof(*device->callback_q));
    device->stream_queue = calloc(OPENRSP_STREAM_QUEUE_CAPACITY,
                                  sizeof(*device->stream_queue));
    if (device->scratch_i == NULL || device->scratch_q == NULL ||
        device->callback_i == NULL || device->callback_q == NULL ||
        device->stream_queue == NULL) {
        free(device->scratch_i);
        free(device->scratch_q);
        free(device->callback_i);
        free(device->callback_q);
        free(device->stream_queue);
        device->scratch_i = NULL;
        device->scratch_q = NULL;
        device->callback_i = NULL;
        device->callback_q = NULL;
        device->stream_queue = NULL;
        device->scratch_capacity = 0u;
        return -1;
    }
    device->scratch_capacity = OPENRSP_MAX_IQ_SAMPLES;
    return 0;
}

static void free_stream_buffers(compat_device_context *device)
{
    free(device->scratch_i);
    free(device->scratch_q);
    free(device->callback_i);
    free(device->callback_q);
    free(device->stream_queue);
    device->scratch_i = NULL;
    device->scratch_q = NULL;
    device->callback_i = NULL;
    device->callback_q = NULL;
    device->stream_queue = NULL;
    device->scratch_capacity = 0u;
}

static void *event_thread_main(void *opaque)
{
    compat_device_context *device = opaque;
    (void)pthread_mutex_lock(&device->event_lock);
    for (;;) {
        while (device->event_count == 0u && !device->event_stop)
            (void)pthread_cond_wait(&device->event_ready, &device->event_lock);
        if (device->event_count == 0u && device->event_stop) break;
        compat_api_event event = device->event_queue[device->event_head];
        device->event_head = (device->event_head + 1u) % OPENRSP_EVENT_QUEUE_CAPACITY;
        --device->event_count;
        device->event_dispatching = 1;
        sdrplay_api_EventCallback_t callback = device->callbacks.EventCbFn;
        void *context = device->callback_context;
        (void)pthread_mutex_unlock(&device->event_lock);
        if (callback) callback(event.event, event.tuner, &event.params, context);
        (void)pthread_mutex_lock(&device->event_lock);
        device->event_dispatching = 0;
        if (device->event_count == 0u)
            (void)pthread_cond_broadcast(&device->event_idle);
    }
    (void)pthread_cond_broadcast(&device->event_idle);
    (void)pthread_mutex_unlock(&device->event_lock);
    return NULL;
}

static int queue_api_event(compat_device_context *device, sdrplay_api_EventT event,
                           sdrplay_api_TunerSelectT tuner,
                           const sdrplay_api_EventParamsT *params)
{
    if (!device->event_thread_started) return -1;
    (void)pthread_mutex_lock(&device->event_lock);
    if (device->event_stop || device->event_count == OPENRSP_EVENT_QUEUE_CAPACITY) {
        (void)pthread_mutex_unlock(&device->event_lock);
        return -1;
    }
    unsigned int tail = (device->event_head + device->event_count) %
                        OPENRSP_EVENT_QUEUE_CAPACITY;
    device->event_queue[tail].event = event;
    device->event_queue[tail].tuner = tuner;
    device->event_queue[tail].params = *params;
    ++device->event_count;
    (void)pthread_cond_signal(&device->event_ready);
    (void)pthread_mutex_unlock(&device->event_lock);
    return 0;
}

static void wait_for_event_idle(compat_device_context *device)
{
    if (!device->event_thread_started ||
        pthread_equal(pthread_self(), device->event_thread)) return;
    (void)pthread_mutex_lock(&device->event_lock);
    while (device->event_count != 0u || device->event_dispatching)
        (void)pthread_cond_wait(&device->event_idle, &device->event_lock);
    (void)pthread_mutex_unlock(&device->event_lock);
}

static int start_event_thread(compat_device_context *device)
{
    if (pthread_mutex_init(&device->event_lock, NULL) != 0) return -1;
    if (pthread_cond_init(&device->event_ready, NULL) != 0) {
        (void)pthread_mutex_destroy(&device->event_lock);
        return -1;
    }
    if (pthread_cond_init(&device->event_idle, NULL) != 0) {
        (void)pthread_cond_destroy(&device->event_ready);
        (void)pthread_mutex_destroy(&device->event_lock);
        return -1;
    }
    if (pthread_create(&device->event_thread, NULL, event_thread_main, device) != 0) {
        (void)pthread_cond_destroy(&device->event_idle);
        (void)pthread_cond_destroy(&device->event_ready);
        (void)pthread_mutex_destroy(&device->event_lock);
        return -1;
    }
    device->event_thread_started = 1;
    return 0;
}

static void stop_event_thread(compat_device_context *device)
{
    if (!device->event_thread_started) return;
    (void)pthread_mutex_lock(&device->event_lock);
    device->event_stop = 1;
    (void)pthread_cond_broadcast(&device->event_ready);
    (void)pthread_mutex_unlock(&device->event_lock);
    (void)pthread_join(device->event_thread, NULL);
    device->event_thread_started = 0;
    (void)pthread_cond_destroy(&device->event_idle);
    (void)pthread_cond_destroy(&device->event_ready);
    (void)pthread_mutex_destroy(&device->event_lock);
}

static void record_last_error(const char *function, const char *format, ...)
{
    (void)pthread_mutex_lock(&last_error_lock);
    (void)snprintf(last_error.file, sizeof(last_error.file), "%s",
                   "compat/sdrplay_api_compat.c");
    (void)snprintf(last_error.function, sizeof(last_error.function), "%s", function);
    last_error.line = 0;
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(last_error.message, sizeof(last_error.message), format, arguments);
    va_end(arguments);
    struct timespec recorded_at;
    if (timespec_get(&recorded_at, TIME_UTC) == TIME_UTC) {
        last_error_time = (unsigned long long)recorded_at.tv_sec * 1000000u +
                          (unsigned long long)recorded_at.tv_nsec / 1000u;
    } else {
        last_error_time = 0u;
    }
    (void)pthread_mutex_unlock(&last_error_lock);
}

static sdrplay_api_ErrT update_failure_code(sdrplay_api_ReasonForUpdateT reason)
{
    if ((reason & sdrplay_api_Update_Tuner_Gr) != 0u)
        return sdrplay_api_GainUpdateError;
    if ((reason & sdrplay_api_Update_Tuner_Frf) != 0u)
        return sdrplay_api_RfUpdateError;
    if ((reason & (sdrplay_api_Update_Dev_Fs | sdrplay_api_Update_Dev_Ppm)) != 0u)
        return sdrplay_api_FsUpdateError;
    return sdrplay_api_HwError;
}

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

static int valid_decimation_factor(unsigned int factor)
{
    return factor == 1u || factor == 2u || factor == 4u || factor == 8u ||
           factor == 16u || factor == 32u;
}

static int configure_decimator(compat_device_context *device, unsigned int factor)
{
    const double pi = 3.14159265358979323846;
    if (!valid_decimation_factor(factor)) return -1;
    (void)pthread_mutex_lock(&decimation_lock);
    memset(device->decimation_history_i, 0, sizeof(device->decimation_history_i));
    memset(device->decimation_history_q, 0, sizeof(device->decimation_history_q));
    device->decimation_position = 0u;
    device->decimation_phase = 0u;
    device->decimation_taps = factor == 1u ? 0u : 16u * factor + 1u;
    if (factor > 1u) {
        const double cutoff = 0.45 / (double)factor;
        const double center = ((double)device->decimation_taps - 1.0) / 2.0;
        double sum = 0.0;
        for (unsigned int index = 0u; index < device->decimation_taps; ++index) {
            double offset = (double)index - center;
            double sinc = offset == 0.0 ? 2.0 * cutoff :
                          sin(2.0 * pi * cutoff * offset) / (pi * offset);
            double window = 0.54 - 0.46 * cos(2.0 * pi * (double)index /
                                             (double)(device->decimation_taps - 1u));
            device->decimation_coefficients[index] = sinc * window;
            sum += device->decimation_coefficients[index];
        }
        for (unsigned int index = 0u; index < device->decimation_taps; ++index)
            device->decimation_coefficients[index] /= sum;
    }
    atomic_store(&device->decimation_factor, factor);
    (void)pthread_mutex_unlock(&decimation_lock);
    return 0;
}

static size_t decimate_iq(compat_device_context *device, short *xi, short *xq,
                          size_t samples)
{
    size_t output = 0u;
    (void)pthread_mutex_lock(&decimation_lock);
    unsigned int factor = atomic_load(&device->decimation_factor);
    if (factor == 1u) {
        (void)pthread_mutex_unlock(&decimation_lock);
        return samples;
    }
    for (size_t input = 0u; input < samples; ++input) {
        device->decimation_history_i[device->decimation_position] = xi[input];
        device->decimation_history_q[device->decimation_position] = xq[input];
        device->decimation_position =
            (device->decimation_position + 1u) % device->decimation_taps;
        if (device->decimation_phase == 0u) {
            double sum_i = 0.0;
            double sum_q = 0.0;
            for (unsigned int tap = 0u; tap < device->decimation_taps; ++tap) {
                unsigned int history = (device->decimation_position +
                                        device->decimation_taps - 1u - tap) %
                                       device->decimation_taps;
                sum_i += device->decimation_coefficients[tap] *
                         (double)device->decimation_history_i[history];
                sum_q += device->decimation_coefficients[tap] *
                         (double)device->decimation_history_q[history];
            }
            xi[output] = (short)clamp_int((int)lround(sum_i), -32768, 32767);
            xq[output] = (short)clamp_int((int)lround(sum_q), -32768, 32767);
            ++output;
        }
        device->decimation_phase = (device->decimation_phase + 1u) % factor;
    }
    (void)pthread_mutex_unlock(&decimation_lock);
    return output;
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

static int valid_bandwidth(sdrplay_api_Bw_MHzT bandwidth)
{
    return bandwidth == sdrplay_api_BW_0_200 ||
           bandwidth == sdrplay_api_BW_0_300 ||
           bandwidth == sdrplay_api_BW_0_600 ||
           bandwidth == sdrplay_api_BW_1_536 ||
           bandwidth == sdrplay_api_BW_5_000 ||
           bandwidth == sdrplay_api_BW_6_000 ||
           bandwidth == sdrplay_api_BW_7_000 ||
           bandwidth == sdrplay_api_BW_8_000;
}

static int valid_if(sdrplay_api_If_kHzT if_type)
{
    return if_type == sdrplay_api_IF_Zero || if_type == sdrplay_api_IF_0_450 ||
           if_type == sdrplay_api_IF_1_620 || if_type == sdrplay_api_IF_2_048;
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
    if ((reason & sdrplay_api_Update_Dev_Fs) != 0u &&
        (!isfinite(device->dev_params.fsFreq.fsHz) ||
         device->dev_params.fsFreq.fsHz < 2000000.0 ||
         device->dev_params.fsFreq.fsHz > 10660000.0))
        return sdrplay_api_OutOfRange;
    if ((reason & sdrplay_api_Update_Dev_Ppm) != 0u &&
        (!isfinite(device->dev_params.ppm) || device->dev_params.ppm < -300.0 ||
         device->dev_params.ppm > 300.0))
        return sdrplay_api_OutOfRange;
    if ((reason & sdrplay_api_Update_Tuner_Frf) != 0u &&
        (!isfinite(device->channel_a.tunerParams.rfFreq.rfHz) ||
         device->channel_a.tunerParams.rfFreq.rfHz < 1000.0 ||
         device->channel_a.tunerParams.rfFreq.rfHz > 2000000000.0))
        return sdrplay_api_OutOfRange;
    if ((reason & sdrplay_api_Update_Tuner_BwType) != 0u &&
        !valid_bandwidth(device->channel_a.tunerParams.bwType))
        return sdrplay_api_OutOfRange;
    if ((reason & sdrplay_api_Update_Tuner_IfType) != 0u &&
        !valid_if(device->channel_a.tunerParams.ifType))
        return sdrplay_api_OutOfRange;
    if ((reason & (sdrplay_api_Update_Tuner_BwType |
                   sdrplay_api_Update_Tuner_IfType)) != 0u &&
        device->channel_a.tunerParams.ifType != sdrplay_api_IF_Zero &&
        device->channel_a.tunerParams.bwType > sdrplay_api_BW_1_536)
        return sdrplay_api_InvalidMode;
    if ((reason & sdrplay_api_Update_Tuner_Gr) != 0u) {
        const sdrplay_api_GainT *gain = &device->channel_a.tunerParams.gain;
        int lna_reduction = 0;
        if (gain->gRdB < 20 || gain->gRdB > 59 ||
            rspduo_lna_gain_reduction(device->channel_a.tunerParams.rfFreq.rfHz,
                                      gain->LNAstate, &lna_reduction) < 0)
            return sdrplay_api_OutOfRange;
    }
    if ((reason & sdrplay_api_Update_Tuner_LoMode) != 0u &&
        device->channel_a.tunerParams.loMode != sdrplay_api_LO_Auto)
        return sdrplay_api_InvalidMode;
    if ((reason & sdrplay_api_Update_Ctrl_Decimation) != 0u) {
        const sdrplay_api_DecimationT *decimation = &device->channel_a.ctrlParams.decimation;
        if (decimation->enable > 1u || decimation->wideBandSignal > 1u)
            return sdrplay_api_InvalidParam;
        unsigned int factor = decimation->enable != 0u ? decimation->decimationFactor : 1u;
        if (!valid_decimation_factor(factor)) return sdrplay_api_OutOfRange;
    }
    if ((reason & sdrplay_api_Update_Ctrl_DCoffsetIQimbalance) != 0u &&
        (device->channel_a.ctrlParams.dcOffset.DCenable > 1u ||
         device->channel_a.ctrlParams.dcOffset.IQenable > 1u))
        return sdrplay_api_InvalidParam;
    if ((reason & sdrplay_api_Update_Ctrl_Agc) != 0u &&
        (device->channel_a.ctrlParams.agc.enable < sdrplay_api_AGC_DISABLE ||
         device->channel_a.ctrlParams.agc.enable > sdrplay_api_AGC_CTRL_EN ||
         device->channel_a.ctrlParams.agc.setPoint_dBfs < -60 ||
         device->channel_a.ctrlParams.agc.setPoint_dBfs > -20))
        return sdrplay_api_OutOfRange;
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
        sdrplay_api_AgcControlT mode =
            (sdrplay_api_AgcControlT)atomic_load(&device->agc_mode);
        unsigned int period_ms = agc_period_ms(mode);
        const struct timespec delay = {
            .tv_sec = period_ms / 1000u,
            .tv_nsec = (long)(period_ms % 1000u) * 1000000L
        };
        nanosleep(&delay, NULL);
        mode = (sdrplay_api_AgcControlT)atomic_load(&device->agc_mode);
        unsigned int peak = atomic_exchange(&device->agc_peak, 0u);
        if (mode == sdrplay_api_AGC_DISABLE || peak == 0u || !device->initialized) continue;

        double level_dbfs = 20.0 * log10((double)peak / 32767.0);
        int target = clamp_int(atomic_load(&device->agc_setpoint), -60, -20);
        double error = level_dbfs - (double)target;
        int step = 0;
        if (error > 6.0) step = 3;
        else if (error > 1.5) step = 1;
        else if (error < -6.0) step = -2;
        else if (error < -1.5) step = -1;
        if (step == 0) continue;

        pthread_mutex_lock(&hardware_lock);
        sdrplay_api_EventParamsT event_params;
        int notify_gain = 0;
        int old_reduction = device->channel_a.tunerParams.gain.gRdB;
        int new_reduction = clamp_int(old_reduction + step, 20, 59);
        if (new_reduction != old_reduction) {
            device->channel_a.tunerParams.gain.gRdB = new_reduction;
            if (apply_rspduo_gain_locked(device) >= 0) {
                atomic_fetch_add(&device->pending_gr_changed, 1u);
                int lna_reduction = 0;
                (void)rspduo_lna_gain_reduction(
                    device->channel_a.tunerParams.rfFreq.rfHz,
                    device->channel_a.tunerParams.gain.LNAstate, &lna_reduction);
                memset(&event_params, 0, sizeof(event_params));
                event_params.gainParams.gRdB = (unsigned int)new_reduction;
                event_params.gainParams.lnaGRdB = (unsigned int)lna_reduction;
                event_params.gainParams.currGain =
                    device->channel_a.tunerParams.gain.gainVals.curr;
                notify_gain = 1;
            } else {
                device->channel_a.tunerParams.gain.gRdB = old_reduction;
            }
        }
        pthread_mutex_unlock(&hardware_lock);
        if (notify_gain)
            (void)queue_api_event(device, sdrplay_api_GainChange,
                                  sdrplay_api_Tuner_A, &event_params);
    }
    return NULL;
}

static void emit_overload_event(compat_device_context *device, int overloaded)
{
    sdrplay_api_EventParamsT params;
    memset(&params, 0, sizeof(params));
    params.powerOverloadParams.powerOverloadChangeType =
        overloaded ? sdrplay_api_Overload_Detected : sdrplay_api_Overload_Corrected;
    if (queue_api_event(device, sdrplay_api_PowerOverloadChange,
                        sdrplay_api_Tuner_A, &params) == 0) {
        atomic_store(&device->overload_reported_state, overloaded);
        atomic_store(&device->overload_event_pending, 1);
    } else {
        atomic_store(&device->overload_event_pending, 0);
    }
}

static void *stream_callback_thread_main(void *opaque)
{
    compat_device_context *device = opaque;
    for (;;) {
        (void)pthread_mutex_lock(&device->stream_callback_lock);
        while (device->stream_callback_count == 0u && !device->stream_callback_stop)
            (void)pthread_cond_wait(&device->stream_callback_ready,
                                    &device->stream_callback_lock);
        if (device->stream_callback_stop) {
            (void)pthread_mutex_unlock(&device->stream_callback_lock);
            break;
        }
        compat_stream_frame *frame =
            &device->stream_queue[device->stream_callback_head];
        size_t samples = frame->samples;
        unsigned int first_sample = frame->first_sample;
        unsigned int reset = frame->reset || !device->stream_callback_seen ||
                             first_sample != device->stream_callback_next_sample;
        int gr_changed = frame->gr_changed;
        int rf_changed = frame->rf_changed;
        int fs_changed = frame->fs_changed;
        memcpy(device->callback_i, frame->xi, samples * sizeof(*device->callback_i));
        memcpy(device->callback_q, frame->xq, samples * sizeof(*device->callback_q));
        device->stream_callback_head =
            (device->stream_callback_head + 1u) % OPENRSP_STREAM_QUEUE_CAPACITY;
        --device->stream_callback_count;
        device->stream_callback_seen = 1;
        device->stream_callback_next_sample = first_sample + (unsigned int)samples;
        (void)pthread_mutex_unlock(&device->stream_callback_lock);

        if (samples == 0u) {
            sdrplay_api_StreamCbParamsT params = {
                .firstSampleNum = first_sample,
                .grChanged = gr_changed,
                .rfChanged = rf_changed,
                .fsChanged = fs_changed,
                .numSamples = 0u
            };
            device->callbacks.StreamACbFn(device->callback_i, device->callback_q,
                                          &params, 0u, reset,
                                          device->callback_context);
            continue;
        }
        for (size_t offset = 0u; offset < samples;) {
            unsigned int chunk = (unsigned int)(samples - offset);
            unsigned int callback_samples = atomic_load(&device->callback_samples);
            if (chunk > callback_samples) chunk = callback_samples;
            sdrplay_api_StreamCbParamsT params = {
                .firstSampleNum = first_sample + (unsigned int)offset,
                .grChanged = offset == 0u ? gr_changed : 0,
                .rfChanged = offset == 0u ? rf_changed : 0,
                .fsChanged = offset == 0u ? fs_changed : 0,
                .numSamples = chunk
            };
            device->callbacks.StreamACbFn(device->callback_i + offset,
                                          device->callback_q + offset, &params,
                                          chunk, offset == 0u ? reset : 0u,
                                          device->callback_context);
            offset += chunk;
        }
    }
    return NULL;
}

static int start_stream_callback_thread(compat_device_context *device)
{
    if (pthread_mutex_init(&device->stream_callback_lock, NULL) != 0) return -1;
    if (pthread_cond_init(&device->stream_callback_ready, NULL) != 0) {
        (void)pthread_mutex_destroy(&device->stream_callback_lock);
        return -1;
    }
    device->stream_callback_stop = 0;
    device->stream_callback_head = 0u;
    device->stream_callback_count = 0u;
    device->stream_callback_drop_count = 0u;
    device->stream_callback_seen = 0;
    if (pthread_create(&device->stream_callback_thread, NULL,
                       stream_callback_thread_main, device) != 0) {
        (void)pthread_cond_destroy(&device->stream_callback_ready);
        (void)pthread_mutex_destroy(&device->stream_callback_lock);
        return -1;
    }
    device->stream_callback_thread_started = 1;
    return 0;
}

static void stop_stream_callback_thread(compat_device_context *device)
{
    if (!device->stream_callback_thread_started) return;
    (void)pthread_mutex_lock(&device->stream_callback_lock);
    device->stream_callback_stop = 1;
    device->stream_callback_count = 0u;
    (void)pthread_cond_signal(&device->stream_callback_ready);
    (void)pthread_mutex_unlock(&device->stream_callback_lock);
    (void)pthread_join(device->stream_callback_thread, NULL);
    device->stream_callback_thread_started = 0;
    (void)pthread_cond_destroy(&device->stream_callback_ready);
    (void)pthread_mutex_destroy(&device->stream_callback_lock);
}

static void queue_stream_callback(compat_device_context *device, size_t samples,
                                  unsigned int first_sample, unsigned int reset,
                                  int gr_changed, int rf_changed, int fs_changed)
{
    (void)pthread_mutex_lock(&device->stream_callback_lock);
    unsigned int carried_reset = 0u;
    int carried_gr_changed = 0;
    int carried_rf_changed = 0;
    int carried_fs_changed = 0;
    if (device->stream_callback_count == OPENRSP_STREAM_QUEUE_CAPACITY) {
        compat_stream_frame *dropped =
            &device->stream_queue[device->stream_callback_head];
        carried_reset = dropped->reset;
        carried_gr_changed = dropped->gr_changed;
        carried_rf_changed = dropped->rf_changed;
        carried_fs_changed = dropped->fs_changed;
        device->stream_callback_head =
            (device->stream_callback_head + 1u) % OPENRSP_STREAM_QUEUE_CAPACITY;
        --device->stream_callback_count;
        ++device->stream_callback_drop_count;
        unsigned int drops = device->stream_callback_drop_count;
        if ((drops & (drops - 1u)) == 0u)
            fprintf(stderr, "OPENRSP_API_CALLBACK_DROP count=%u\n", drops);
    }
    unsigned int tail = (device->stream_callback_head +
                         device->stream_callback_count) % OPENRSP_STREAM_QUEUE_CAPACITY;
    compat_stream_frame *frame = &device->stream_queue[tail];
    frame->samples = samples;
    frame->first_sample = first_sample;
    frame->reset = reset || carried_reset;
    frame->gr_changed = gr_changed || carried_gr_changed;
    frame->rf_changed = rf_changed || carried_rf_changed;
    frame->fs_changed = fs_changed || carried_fs_changed;
    memcpy(frame->xi, device->scratch_i, samples * sizeof(*frame->xi));
    memcpy(frame->xq, device->scratch_q, samples * sizeof(*frame->xq));
    ++device->stream_callback_count;
    (void)pthread_cond_signal(&device->stream_callback_ready);
    (void)pthread_mutex_unlock(&device->stream_callback_lock);
}

static void daemon_stream_callback(const int16_t *interleaved, size_t samples,
                                   uint32_t sequence, void *opaque)
{
    compat_device_context *device = opaque;
    atomic_store(&device->stream_state, 1);
    int discontinuity = 0;
    uint32_t missing_frames = 0u;
    if (device->iq_sequence_seen) {
        uint32_t expected = device->last_iq_sequence + 1u;
        if (sequence != expected) {
            uint32_t distance = sequence - expected;
            discontinuity = 1;
            if (distance < 0x80000000u) missing_frames = distance;
            fprintf(stderr,
                    "OPENRSP_API_IQ_GAP expected=%u received=%u missing_frames=%u\n",
                    expected, sequence, missing_frames);
        }
    }
    device->last_iq_sequence = sequence;
    device->iq_sequence_seen = 1;
    if (samples > device->scratch_capacity || device->scratch_i == NULL ||
        device->scratch_q == NULL) {
        atomic_store(&device->stream_state, -1);
        return;
    }
    short *xi = device->scratch_i;
    short *xq = device->scratch_q;
    unsigned int peak = 0u;
    for (size_t index = 0; index < samples; ++index) {
        xi[index] = interleaved[index * 2u];
        xq[index] = interleaved[index * 2u + 1u];
        unsigned int abs_i = (unsigned int)abs(xi[index]);
        unsigned int abs_q = (unsigned int)abs(xq[index]);
        unsigned int sample_peak = abs_i > abs_q ? abs_i : abs_q;
        if (sample_peak > peak) peak = sample_peak;
    }
    int overloaded = atomic_load(&device->overload_state);
    int next_overload = overloaded ? peak > 30000u : peak >= 32700u;
    if (next_overload != overloaded) {
        atomic_store(&device->overload_state, next_overload);
        int expected_pending = 0;
        if (atomic_compare_exchange_strong(&device->overload_event_pending,
                                           &expected_pending, 1))
            emit_overload_event(device, next_overload);
    }
    (void)pthread_mutex_lock(&decimation_lock);
    samples = openrsp_low_if_process(&device->low_if_dsp, xi, xq, samples);
    (void)pthread_mutex_unlock(&decimation_lock);
    samples = decimate_iq(device, xi, xq, samples);
    if (missing_frames != 0u)
        (void)atomic_fetch_add(&device->sample_number,
                               missing_frames * (unsigned int)samples);
    unsigned int observed = atomic_load(&device->agc_peak);
    while (peak > observed &&
           !atomic_compare_exchange_weak(&device->agc_peak, &observed, peak)) {}
    int gr_changed = consume_one(&device->pending_gr_changed);
    int rf_changed = consume_one(&device->pending_rf_changed);
    int fs_changed = consume_one(&device->pending_fs_changed);
    unsigned int first_sample = atomic_fetch_add(&device->sample_number,
                                                  (unsigned int)samples);
    unsigned int reset = device->first_callback || discontinuity;
    queue_stream_callback(device, samples, first_sample, reset,
                          gr_changed, rf_changed, fs_changed);
    device->first_callback = 0;
}

static void daemon_failure_callback(void *opaque)
{
    compat_device_context *device = opaque;
    atomic_store(&device->stream_state, -1);
    sdrplay_api_EventParamsT params;
    memset(&params, 0, sizeof(params));
    (void)queue_api_event(device, sdrplay_api_DeviceFailure,
                          sdrplay_api_Tuner_A, &params);
}

static void emit_update_ack(compat_device_context *device, int fs_changed,
                            int rf_changed, int gr_changed)
{
    queue_stream_callback(device, 0u, atomic_load(&device->sample_number), 0u,
                          gr_changed, rf_changed, fs_changed);
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
    last_error_time = 0u;
    atomic_init(&rspduo.pending_gr_changed, 0u);
    atomic_init(&rspduo.sample_number, 0u);
    atomic_init(&rspduo.pending_rf_changed, 0u);
    atomic_init(&rspduo.pending_fs_changed, 0u);
    atomic_init(&rspduo.stream_state, 0);
    atomic_init(&rspduo.agc_peak, 0u);
    atomic_init(&rspduo.agc_stop, 0);
    atomic_init(&rspduo.agc_mode, sdrplay_api_AGC_CTRL_EN);
    atomic_init(&rspduo.agc_setpoint, -60);
    atomic_init(&rspduo.overload_state, 0);
    atomic_init(&rspduo.overload_event_pending, 0);
    atomic_init(&rspduo.overload_reported_state, 0);
    atomic_init(&rspduo.decimation_factor, 1u);
    (void)openrsp_low_if_configure(&rspduo.low_if_dsp, 2000000.0, 0);
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

static unsigned char api_hardware_version(uint16_t product_id)
{
    switch (product_id) {
    case 0x2500u:
        return SDRPLAY_RSP1_ID;
    case 0x3000u:
        return SDRPLAY_RSP1A_ID;
    case 0x3010u:
        return SDRPLAY_RSP2_ID;
    case 0x3020u:
        return SDRPLAY_RSPduo_ID;
    default:
        return 0u;
    }
}

sdrplay_api_ErrT sdrplay_api_Open(void)
{
    if (device_api_lock_depth != 0u)
        return atomic_load(&api_open) ? sdrplay_api_AlreadyInitialised : sdrplay_api_Fail;
    if (pthread_mutex_lock(&device_api_lock) != 0) return sdrplay_api_Fail;
    if (atomic_load(&api_open)) {
        (void)pthread_mutex_unlock(&device_api_lock);
        return sdrplay_api_AlreadyInitialised;
    }
    reset_parameters();
    if (start_event_thread(&rspduo) != 0) {
        (void)pthread_mutex_unlock(&device_api_lock);
        return sdrplay_api_OutOfMemError;
    }
    atomic_store(&api_open, 1);
    (void)pthread_mutex_unlock(&device_api_lock);
    return sdrplay_api_Success;
}

sdrplay_api_ErrT sdrplay_api_Close(void)
{
    if (device_api_lock_depth != 0u) return sdrplay_api_Fail;
    if (pthread_mutex_lock(&device_api_lock) != 0) return sdrplay_api_Fail;
    if (rspduo.initialized || rspduo.selected ||
        (rspduo.event_thread_started && pthread_equal(pthread_self(), rspduo.event_thread))) {
        (void)pthread_mutex_unlock(&device_api_lock);
        return sdrplay_api_AlreadyInitialised;
    }
    stop_event_thread(&rspduo);
    atomic_store(&api_open, 0);
    (void)pthread_mutex_unlock(&device_api_lock);
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
    if (!atomic_load(&api_open)) return sdrplay_api_NotInitialised;
    if (device_api_lock_depth == 0u) {
        if (pthread_mutex_lock(&device_api_lock) != 0) return sdrplay_api_Fail;
        if (!atomic_load(&api_open)) {
            (void)pthread_mutex_unlock(&device_api_lock);
            return sdrplay_api_NotInitialised;
        }
        int result = openrsp_daemon_api_lock_acquire(&daemon_api_lock);
        if (result != 0) {
            (void)pthread_mutex_unlock(&device_api_lock);
            return result == -(int)OPENRSP_STATUS_BUSY ? sdrplay_api_Fail :
                                                        sdrplay_api_ServiceNotResponding;
        }
    }
    ++device_api_lock_depth;
    return sdrplay_api_Success;
}

sdrplay_api_ErrT sdrplay_api_UnlockDeviceApi(void)
{
    if (device_api_lock_depth == 0u)
        return atomic_load(&api_open) ? sdrplay_api_Fail : sdrplay_api_NotInitialised;
    --device_api_lock_depth;
    if (device_api_lock_depth == 0u) {
        int result = openrsp_daemon_api_lock_release(daemon_api_lock);
        daemon_api_lock = NULL;
        if (pthread_mutex_unlock(&device_api_lock) != 0) return sdrplay_api_Fail;
        if (result != 0) return sdrplay_api_ServiceNotResponding;
    }
    return sdrplay_api_Success;
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

    openrsp_device_record found[SDRPLAY_MAX_DEVICES];
    memset(found, 0, sizeof(found));
    int discovered = device_api_lock_depth != 0u && daemon_api_lock != NULL ?
                     openrsp_daemon_api_lock_list(daemon_api_lock, found,
                                                  SDRPLAY_MAX_DEVICES) :
                     openrsp_daemon_backend_list(found, SDRPLAY_MAX_DEVICES);
    if (discovered < 0) return sdrplay_api_HwError;
    if (discovered > (int)SDRPLAY_MAX_DEVICES)
        discovered = (int)SDRPLAY_MAX_DEVICES;

    unsigned int written = 0;
    memset(discovery_handles, 0, sizeof(discovery_handles));
    discovery_handle_count = 0u;
    for (int index = 0; index < discovered && written < maxDevs; ++index) {
        if (found[index].vendor_id != 0x1df7u) continue;
        unsigned char hw_version = api_hardware_version(found[index].product_id);
        if (hw_version == 0u || written >= SDRPLAY_MAX_DEVICES) continue;
        compat_discovery_handle *handle = &discovery_handles[written];
        handle->identity.device_index = found[index].device_index;
        handle->identity.vendor_id = found[index].vendor_id;
        handle->identity.product_id = found[index].product_id;
        (void)snprintf(handle->identity.serial, sizeof(handle->identity.serial),
                       "%s", found[index].serial);
        handle->hw_version = hw_version;
        handle->valid = 1;
        sdrplay_api_DeviceT *device = &devices[written++];
        memset(device, 0, sizeof(*device));
        const char *override = getenv("OPENRSP_SERIAL");
        const char *identity = discovered == 1 && override != NULL && override[0] != '\0' ?
                               override :
                               (found[index].serial[0] == '\0' ? "OPENRSP0" : found[index].serial);
        (void)snprintf(device->SerNo, sizeof(device->SerNo), "%s", identity);
        device->hwVer = hw_version;
        device->tuner = sdrplay_api_Tuner_A;
        device->rspDuoMode = found[index].product_id == 0x3020u ?
                             sdrplay_api_RspDuoMode_Single_Tuner :
                             sdrplay_api_RspDuoMode_Unknown;
        device->valid = 1u;
        device->dev = handle;
    }
    discovery_handle_count = written;
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
    (void)pthread_mutex_lock(&last_error_lock);
    last_error_view = last_error;
    (void)pthread_mutex_unlock(&last_error_lock);
    return &last_error_view;
}

sdrplay_api_ErrorInfoT *sdrplay_api_GetLastErrorByType(sdrplay_api_DeviceT *device,
                                                        int type, unsigned long long *time)
{
    (void)device;
    /* API 3.15 defines 0 as a DLL error and 1--3 as DLL-device,
     * service, and service-device errors. OpenRSP currently records only
     * in-process compatibility-layer errors. Preserve the caller's timestamp
     * when the requested category has no record, matching the vendor ABI. */
    if (type != 0) return NULL;
    (void)pthread_mutex_lock(&last_error_lock);
    last_error_view = last_error;
    if (time != NULL) *time = last_error_time;
    (void)pthread_mutex_unlock(&last_error_lock);
    return &last_error_view;
}

sdrplay_api_ErrT sdrplay_api_DisableHeartbeat(void)
{
    if (!atomic_load(&api_open)) return sdrplay_api_NotInitialised;
    if (device_api_lock_depth == 0u || rspduo.selected) return sdrplay_api_Fail;
    return sdrplay_api_Success;
}

sdrplay_api_ErrT sdrplay_api_DebugEnable(HANDLE dev, sdrplay_api_DbgLvl_t level)
{
    if (!api_open) return sdrplay_api_NotInitialised;
    if (level < sdrplay_api_DbgLvl_Disable || level > sdrplay_api_DbgLvl_Message)
        return sdrplay_api_OutOfRange;
    if (dev == NULL) return sdrplay_api_Success;
    if (dev != &rspduo || !rspduo.selected) return sdrplay_api_InvalidParam;
    return sdrplay_api_Success;
}

sdrplay_api_ErrT sdrplay_api_SelectDevice(sdrplay_api_DeviceT *device)
{
    if (!api_open) return sdrplay_api_NotInitialised;
    if (device == NULL || device->tuner != sdrplay_api_Tuner_A || !device->valid)
        return sdrplay_api_InvalidParam;
    if (rspduo.selected) return sdrplay_api_AlreadyInitialised;
    compat_discovery_handle *handle = NULL;
    for (unsigned int index = 0u; index < discovery_handle_count; ++index) {
        if (device->dev == &discovery_handles[index]) {
            handle = &discovery_handles[index];
            break;
        }
    }
    if (handle == NULL || !handle->valid || device->hwVer != handle->hw_version)
        return sdrplay_api_InvalidParam;
    rspduo.selected = 1;
    rspduo.identity = handle->identity;
    rspduo.product_id = handle->identity.product_id;
    rspduo.hw_version = handle->hw_version;
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
    memset(&rspduo.identity, 0, sizeof(rspduo.identity));
    rspduo.product_id = 0u;
    rspduo.hw_version = 0u;
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
    if (allocate_stream_buffers(&rspduo) != 0) return sdrplay_api_HwError;
    const sdrplay_api_DecimationT *decimation = &rspduo.channel_a.ctrlParams.decimation;
    unsigned int factor = decimation->enable != 0u ? decimation->decimationFactor : 1u;
    (void)pthread_mutex_lock(&decimation_lock);
    int dsp_result = openrsp_low_if_configure(
        &rspduo.low_if_dsp, rspduo.dev_params.fsFreq.fsHz,
        (int)rspduo.channel_a.tunerParams.ifType * 1000);
    (void)pthread_mutex_unlock(&decimation_lock);
    if (dsp_result != 0 || configure_decimator(&rspduo, factor) != 0) {
        free_stream_buffers(&rspduo);
        return sdrplay_api_OutOfRange;
    }
    int result = openrsp_daemon_backend_open(&rspduo.backend, &rspduo.identity);
    if (result < 0 || rspduo.backend == NULL) {
        free_stream_buffers(&rspduo);
        return sdrplay_api_HwError;
    }
    openrsp_radio_config config;
    fill_radio_config(&rspduo, &config);
    result = openrsp_daemon_backend_configure(rspduo.backend, &config);
    if (result < 0) {
        openrsp_daemon_backend_close(rspduo.backend);
        rspduo.backend = NULL;
        free_stream_buffers(&rspduo);
        return sdrplay_api_HwError;
    }
    rspduo.callbacks = *callbacks;
    rspduo.callback_context = context;
    if (start_stream_callback_thread(&rspduo) != 0) {
        openrsp_daemon_backend_close(rspduo.backend);
        rspduo.backend = NULL;
        free_stream_buffers(&rspduo);
        memset(&rspduo.callbacks, 0, sizeof(rspduo.callbacks));
        rspduo.callback_context = NULL;
        return sdrplay_api_OutOfMemError;
    }
    atomic_store(&rspduo.sample_number, 0u);
    rspduo.last_iq_sequence = 0u;
    rspduo.iq_sequence_seen = 0;
    atomic_store(&rspduo.stream_state, 0);
    atomic_store(&rspduo.agc_stop, 0);
    atomic_store(&rspduo.agc_peak, 0u);
    atomic_store(&rspduo.agc_mode, rspduo.channel_a.ctrlParams.agc.enable);
    atomic_store(&rspduo.agc_setpoint,
                 rspduo.channel_a.ctrlParams.agc.setPoint_dBfs);
    atomic_store(&rspduo.callback_samples,
                 rspduo.dev_params.fsFreq.fsHz > 9216000.0 ? 2016u : 1344u);
    rspduo.first_callback = 1;
    rspduo.initialized = 1;
    if (openrsp_daemon_backend_start(rspduo.backend, daemon_stream_callback,
                                     daemon_failure_callback, &rspduo) != 0) {
        rspduo.initialized = 0;
        openrsp_daemon_backend_close(rspduo.backend);
        rspduo.backend = NULL;
        stop_stream_callback_thread(&rspduo);
        free_stream_buffers(&rspduo);
        return sdrplay_api_StartPending;
    }
    rspduo.thread_started = 1;
    if (pthread_create(&rspduo.agc_thread, NULL, agc_thread_main, &rspduo) != 0) {
        (void)openrsp_daemon_backend_stop(rspduo.backend);
        openrsp_daemon_backend_close(rspduo.backend);
        rspduo.backend = NULL;
        rspduo.thread_started = 0;
        rspduo.initialized = 0;
        stop_stream_callback_thread(&rspduo);
        free_stream_buffers(&rspduo);
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
        stop_stream_callback_thread(&rspduo);
        free_stream_buffers(&rspduo);
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
        stop_stream_callback_thread(&rspduo);
        free_stream_buffers(&rspduo);
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
    stop_stream_callback_thread(&rspduo);
    free_stream_buffers(&rspduo);
    wait_for_event_idle(&rspduo);
    memset(&rspduo.callbacks, 0, sizeof(rspduo.callbacks));
    rspduo.callback_context = NULL;
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
    uint32_t change_flags = protocol_change_flags(reason);
    int previous_agc_mode = atomic_load(&rspduo.agc_mode);
    int previous_agc_setpoint = atomic_load(&rspduo.agc_setpoint);
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
            atomic_store(&rspduo.agc_mode, mode);
            atomic_store(&rspduo.agc_setpoint,
                         rspduo.channel_a.ctrlParams.agc.setPoint_dBfs);
        }
    }
    if ((reason & sdrplay_api_Update_Ctrl_OverloadMsgAck) != 0u) {
        atomic_store(&rspduo.overload_event_pending, 0);
        int state = atomic_load(&rspduo.overload_state);
        if (state != atomic_load(&rspduo.overload_reported_state))
            emit_overload_event(&rspduo, state);
    }
    /* Install the software-only state before a daemon response can be followed
     * by IQ. The update round trip provides an ordering boundary for callers. */
    if (result >= 0 && (reason & sdrplay_api_Update_Ctrl_Decimation) != 0u) {
        const sdrplay_api_DecimationT *decimation = &rspduo.channel_a.ctrlParams.decimation;
        unsigned int factor = decimation->enable != 0u ? decimation->decimationFactor : 1u;
        if (configure_decimator(&rspduo, factor) != 0) result = -1;
    }
    if (result >= 0 &&
        (reason & (sdrplay_api_Update_Dev_Fs |
                   sdrplay_api_Update_Tuner_IfType)) != 0u) {
        (void)pthread_mutex_lock(&decimation_lock);
        result = openrsp_low_if_configure(
            &rspduo.low_if_dsp, rspduo.dev_params.fsFreq.fsHz,
            (int)rspduo.channel_a.tunerParams.ifType * 1000);
        (void)pthread_mutex_unlock(&decimation_lock);
    }
    if (result >= 0) {
        openrsp_radio_config config;
        fill_radio_config(&rspduo, &config);
        result = openrsp_daemon_backend_update(rspduo.backend, &config,
                                                change_flags);
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
    if (result < 0 && (reason & sdrplay_api_Update_Ctrl_Agc) != 0u) {
        atomic_store(&rspduo.agc_mode, previous_agc_mode);
        atomic_store(&rspduo.agc_setpoint, previous_agc_setpoint);
    }
    pthread_mutex_unlock(&hardware_lock);
    if (result < 0) {
        sdrplay_api_ErrT error = update_failure_code(reason);
        record_last_error("sdrplay_api_Update",
                          "OpenRSP daemon rejected update reasons 0x%08x (backend %d)",
                          reason, result);
        return error;
    }
    return sdrplay_api_Success;
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
