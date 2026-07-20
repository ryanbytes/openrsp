/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sdrplay_api_compat.h"
#include "daemon_backend.h"
#include "adsb_filter.h"
#include "debug_log.h"
#include "gain_values.h"
#include "iq_correction.h"
#include "low_if_dsp.h"
#include "sync_update.h"


#include <pthread.h>
#include <errno.h>
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

typedef struct {
    atomic_uint factor;
    unsigned int taps;
    unsigned int position;
    unsigned int phase;
    double coefficients[513];
    int16_t history_i[513];
    int16_t history_q[513];
} compat_decimator;
static pthread_mutex_t device_api_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned int device_api_lock_depth;
static openrsp_daemon_api_lock *daemon_api_lock;

typedef struct {
    openrsp_acquire_request identity;
    unsigned char hw_version;
    uint32_t rspduo_mode_mask;
    uint32_t rspduo_sample_rate_hz;
    int valid;
} compat_discovery_handle;

static compat_discovery_handle discovery_handles[SDRPLAY_MAX_DEVICES];
static unsigned int discovery_handle_count;

#define OPENRSP_EVENT_QUEUE_CAPACITY 32u
#if defined(_WIN32)
/* Do not make a brief JVM-side callback pause look like lost RF.  This queue
 * remains bounded and continues to retain the newest IQ if the consumer is
 * persistently behind; 128 frames gives the 10 MS/s RSPduo about 0.8 s of
 * callback headroom. */
#define OPENRSP_STREAM_QUEUE_CAPACITY 128u
#else
#define OPENRSP_STREAM_QUEUE_CAPACITY 8u
#endif

typedef struct {
    sdrplay_api_EventT event;
    sdrplay_api_TunerSelectT tuner;
    sdrplay_api_EventParamsT params;
} compat_api_event;

typedef struct {
    size_t samples;
    sdrplay_api_TunerSelectT tuner;
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
    sdrplay_api_TunerSelectT tuner;
    sdrplay_api_RspDuoModeT mode;
    uint32_t duo_role;
    int start_pending;
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
    int stream_callback_paused;
    int stream_callback_active;
    unsigned int stream_callback_head;
    unsigned int stream_callback_count;
    unsigned int stream_callback_drop_count;
    unsigned int stream_callback_next_sample;
    unsigned int stream_callback_next_sample_b;
    int stream_callback_seen;
    int stream_callback_seen_b;
    pthread_t agc_thread;
    int thread_started;
    int agc_thread_started;
    sdrplay_api_CallbackFnsT callbacks;
    void *callback_context;
    atomic_uint sample_number;
    atomic_uint sample_number_b;
    uint32_t last_iq_sequence;
    uint32_t last_iq_sequence_b;
    int iq_sequence_seen;
    int iq_sequence_seen_b;
    unsigned int first_callback;
    unsigned int first_callback_b;
    atomic_uint pending_gr_changed;
    atomic_uint pending_rf_changed;
    atomic_uint pending_fs_changed;
    atomic_uint pending_gr_changed_b;
    atomic_uint pending_rf_changed_b;
    atomic_uint pending_fs_changed_b;
    atomic_int stream_state;
    atomic_uint callback_samples;
    atomic_uint agc_peak;
    atomic_uint agc_peak_b;
    atomic_int agc_stop;
    atomic_int agc_mode;
    atomic_int agc_setpoint;
    atomic_int agc_mode_b;
    atomic_int agc_setpoint_b;
    uint64_t agc_last_adjust_ms[2];
    uint64_t agc_decay_since_ms[2];
    atomic_int overload_state;
    atomic_int overload_event_pending;
    atomic_int overload_reported_state;
    atomic_int overload_state_b;
    atomic_int overload_event_pending_b;
    atomic_int overload_reported_state_b;
    compat_decimator decimator[2];
    openrsp_low_if_dsp low_if_dsp;
    openrsp_low_if_dsp low_if_dsp_b;
    openrsp_iq_correction iq_correction[2];
    openrsp_adsb_filter adsb_filter[2];
    openrsp_sync_update sync_update[2];
    atomic_int sync_waiting[2];
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
enum { OPENRSP_LAST_ERROR_CATEGORY_COUNT = 4 };

static sdrplay_api_ErrorInfoT last_errors[OPENRSP_LAST_ERROR_CATEGORY_COUNT];
static unsigned long long last_error_times[OPENRSP_LAST_ERROR_CATEGORY_COUNT];
static unsigned char last_error_present[OPENRSP_LAST_ERROR_CATEGORY_COUNT];
static _Thread_local sdrplay_api_ErrorInfoT last_error_view;
static pthread_mutex_t last_error_lock = PTHREAD_MUTEX_INITIALIZER;

static sdrplay_api_RxChannelParamsT *active_channel(compat_device_context *device)
{
    return device->tuner == sdrplay_api_Tuner_B ? &device->channel_b :
                                                  &device->channel_a;
}

static sdrplay_api_RxChannelParamsT *channel_for_tuner(
    compat_device_context *device, sdrplay_api_TunerSelectT tuner)
{
    return tuner == sdrplay_api_Tuner_B ? &device->channel_b : &device->channel_a;
}

static const sdrplay_api_RxChannelParamsT *channel_for_tuner_const(
    const compat_device_context *device, sdrplay_api_TunerSelectT tuner)
{
    return tuner == sdrplay_api_Tuner_B ? &device->channel_b : &device->channel_a;
}

static int configure_iq_correction(compat_device_context *device,
                                   sdrplay_api_TunerSelectT tuner)
{
    unsigned int slot = tuner == sdrplay_api_Tuner_B ? 1u : 0u;
    const sdrplay_api_RxChannelParamsT *channel =
        channel_for_tuner_const(device, tuner);
    int result = openrsp_iq_correction_configure(
        &device->iq_correction[slot],
        channel->ctrlParams.dcOffset.DCenable != 0u,
        channel->ctrlParams.dcOffset.IQenable != 0u);
    if (result == 0) {
        const sdrplay_api_DcOffsetTunerT *calibration =
            &channel->tunerParams.dcOffsetTuner;
        result = openrsp_iq_correction_configure_calibration(
            &device->iq_correction[slot], device->dev_params.fsFreq.fsHz,
            calibration->dcCal, calibration->speedUp != 0u,
            calibration->trackTime, calibration->refreshRateTime);
    }
    return result;
}

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
    if (device->event_stop) {
        (void)pthread_mutex_unlock(&device->event_lock);
        return -1;
    }
    if (device->event_count == OPENRSP_EVENT_QUEUE_CAPACITY) {
        /* Transport and physical-device failures must not disappear behind a
         * burst of lower-priority gain/overload notifications.  Preserve the
         * bounded queue by replacing its oldest event. */
        if (event != sdrplay_api_DeviceRemoved &&
            event != sdrplay_api_DeviceFailure) {
            (void)pthread_mutex_unlock(&device->event_lock);
            return -1;
        }
        device->event_head =
            (device->event_head + 1u) % OPENRSP_EVENT_QUEUE_CAPACITY;
        --device->event_count;
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

static void record_last_error(int type, const char *function, int line,
                              const char *format, ...)
{
    if (type < 0 || type >= OPENRSP_LAST_ERROR_CATEGORY_COUNT) return;
    (void)pthread_mutex_lock(&last_error_lock);
    sdrplay_api_ErrorInfoT *error = &last_errors[type];
    (void)snprintf(error->file, sizeof(error->file), "%s",
                   "compat/sdrplay_api_compat.c");
    (void)snprintf(error->function, sizeof(error->function), "%s", function);
    error->line = line;
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(error->message, sizeof(error->message), format, arguments);
    va_end(arguments);
    struct timespec recorded_at;
    if (timespec_get(&recorded_at, TIME_UTC) == TIME_UTC) {
        last_error_times[type] =
            (unsigned long long)recorded_at.tv_sec * 1000000u +
            (unsigned long long)recorded_at.tv_nsec / 1000u;
    } else {
        last_error_times[type] = 0u;
    }
    last_error_present[type] = 1u;
    (void)pthread_mutex_unlock(&last_error_lock);
}

#define RECORD_LAST_ERROR(type, function, ...) \
    record_last_error((type), (function), __LINE__, __VA_ARGS__)

static sdrplay_api_ErrT update_failure_code(sdrplay_api_ReasonForUpdateT reason)
{
    if ((reason & (sdrplay_api_Update_Tuner_Gr |
                   sdrplay_api_Update_Tuner_GrLimits)) != 0u)
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

static int wait_for_sync_boundary(compat_device_context *device,
                                  sdrplay_api_TunerSelectT tuner,
                                  uint32_t categories)
{
    unsigned int slot = tuner == sdrplay_api_Tuner_B ? 1u : 0u;
    atomic_uint *sample_number = slot != 0u ? &device->sample_number_b :
                                             &device->sample_number;
    uint32_t current = atomic_load(sample_number);
    uint32_t target = current;
    for (uint32_t bit = OPENRSP_SYNC_GAIN; bit <= OPENRSP_SYNC_FS; bit <<= 1u) {
        if ((categories & bit) == 0u) continue;
        uint32_t candidate = 0u;
        if (openrsp_sync_update_next(&device->sync_update[slot], bit, current,
                                     &candidate) != 0)
            return -1;
        if ((int32_t)(candidate - target) > 0) target = candidate;
    }
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) return -1;
    deadline.tv_sec += 5;
    (void)pthread_mutex_lock(&device->stream_callback_lock);
    while ((int32_t)(atomic_load(sample_number) - target) < 0 &&
           atomic_load(&device->stream_state) >= 0) {
        int wait_result = pthread_cond_timedwait(&device->stream_callback_ready,
                                                 &device->stream_callback_lock,
                                                 &deadline);
        if (wait_result == ETIMEDOUT) {
            (void)pthread_mutex_unlock(&device->stream_callback_lock);
            return -1;
        }
    }
    int result = atomic_load(&device->stream_state) >= 0 ? 0 : -1;
    (void)pthread_mutex_unlock(&device->stream_callback_lock);
    return result;
}

static int clamp_int(int value, int minimum, int maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static int agc_minimum_setpoint(double sample_rate_hz)
{
    if (sample_rate_hz < 8064000.0) return -72;
    if (sample_rate_hz <= 9216000.0) return -60;
    return -48;
}

static int agc_maximum_setpoint(const sdrplay_api_GainT *gain)
{
    return gain->minGr == sdrplay_api_EXTENDED_MIN_GR ? 0 : -20;
}

static int valid_decimation_factor(unsigned int factor)
{
    return factor == 1u || factor == 2u || factor == 4u || factor == 8u ||
           factor == 16u || factor == 32u;
}

static int validate_dsp_configuration(
    const sdrplay_api_RxChannelParamsT *channel, double sample_rate_hz)
{
    if (channel == NULL ||
        channel->tunerParams.dcOffsetTuner.trackTime <= 0 ||
        channel->tunerParams.dcOffsetTuner.refreshRateTime <= 0)
        return -1;
    unsigned int factor = channel->ctrlParams.decimation.enable != 0u ?
                          channel->ctrlParams.decimation.decimationFactor : 1u;
    if (!valid_decimation_factor(factor)) return -1;
    openrsp_iq_correction correction;
    if (openrsp_iq_correction_configure(
            &correction, channel->ctrlParams.dcOffset.DCenable != 0u,
            channel->ctrlParams.dcOffset.IQenable != 0u) != 0 ||
        openrsp_iq_correction_configure_calibration(
            &correction, sample_rate_hz,
            channel->tunerParams.dcOffsetTuner.dcCal,
            channel->tunerParams.dcOffsetTuner.speedUp != 0u,
            channel->tunerParams.dcOffsetTuner.trackTime,
            channel->tunerParams.dcOffsetTuner.refreshRateTime) != 0)
        return -1;
    openrsp_adsb_filter filter;
    if (openrsp_adsb_filter_configure(
            &filter, sample_rate_hz,
            (openrsp_adsb_mode)channel->ctrlParams.adsbMode) != 0)
        return -1;
    openrsp_low_if_dsp low_if;
    return openrsp_low_if_configure(
        &low_if, sample_rate_hz, (int)channel->tunerParams.ifType * 1000);
}

static compat_decimator *decimator_for_tuner(compat_device_context *device,
                                              sdrplay_api_TunerSelectT tuner)
{
    return &device->decimator[tuner == sdrplay_api_Tuner_B ? 1 : 0];
}

static int configure_decimator(compat_device_context *device,
                               sdrplay_api_TunerSelectT tuner,
                               unsigned int factor)
{
    const double pi = 3.14159265358979323846;
    if (!valid_decimation_factor(factor)) return -1;
    (void)pthread_mutex_lock(&decimation_lock);
    compat_decimator *decimator = decimator_for_tuner(device, tuner);
    memset(decimator->history_i, 0, sizeof(decimator->history_i));
    memset(decimator->history_q, 0, sizeof(decimator->history_q));
    decimator->position = 0u;
    decimator->phase = 0u;
    decimator->taps = factor == 1u ? 0u : 16u * factor + 1u;
    if (factor > 1u) {
        const double cutoff = 0.45 / (double)factor;
        const double center = ((double)decimator->taps - 1.0) / 2.0;
        double sum = 0.0;
        for (unsigned int index = 0u; index < decimator->taps; ++index) {
            double offset = (double)index - center;
            double sinc = offset == 0.0 ? 2.0 * cutoff :
                          sin(2.0 * pi * cutoff * offset) / (pi * offset);
            double window = 0.54 - 0.46 * cos(2.0 * pi * (double)index /
                                             (double)(decimator->taps - 1u));
            decimator->coefficients[index] = sinc * window;
            sum += decimator->coefficients[index];
        }
        for (unsigned int index = 0u; index < decimator->taps; ++index)
            decimator->coefficients[index] /= sum;
    }
    atomic_store(&decimator->factor, factor);
    (void)pthread_mutex_unlock(&decimation_lock);
    return 0;
}

static size_t decimate_iq(compat_device_context *device,
                          sdrplay_api_TunerSelectT tuner, short *xi, short *xq,
                          size_t samples)
{
    size_t output = 0u;
    (void)pthread_mutex_lock(&decimation_lock);
    compat_decimator *decimator = decimator_for_tuner(device, tuner);
    unsigned int factor = atomic_load(&decimator->factor);
    if (factor == 1u) {
        (void)pthread_mutex_unlock(&decimation_lock);
        return samples;
    }
    for (size_t input = 0u; input < samples; ++input) {
        decimator->history_i[decimator->position] = xi[input];
        decimator->history_q[decimator->position] = xq[input];
        decimator->position = (decimator->position + 1u) % decimator->taps;
        if (decimator->phase == 0u) {
            double sum_i = 0.0;
            double sum_q = 0.0;
            for (unsigned int tap = 0u; tap < decimator->taps; ++tap) {
                unsigned int history = (decimator->position +
                                        decimator->taps - 1u - tap) %
                                       decimator->taps;
                sum_i += decimator->coefficients[tap] *
                         (double)decimator->history_i[history];
                sum_q += decimator->coefficients[tap] *
                         (double)decimator->history_q[history];
            }
            xi[output] = (short)clamp_int((int)lround(sum_i), -32768, 32767);
            xq[output] = (short)clamp_int((int)lround(sum_q), -32768, 32767);
            ++output;
        }
        decimator->phase = (decimator->phase + 1u) % factor;
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

static int rspduo_am_port_lna_state_valid(
    const sdrplay_api_RxChannelParamsT *channel,
    sdrplay_api_TunerSelectT tuner)
{
    return tuner != sdrplay_api_Tuner_A ||
           channel->rspDuoTunerParams.tuner1AmPortSel !=
               sdrplay_api_RspDuo_AMPORT_1 ||
           channel->tunerParams.rfFreq.rfHz >= 60000000.0 ||
           channel->tunerParams.gain.LNAstate < 5u;
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

static void fill_radio_config_tuner(const compat_device_context *device,
                                    sdrplay_api_TunerSelectT tuner,
                                    openrsp_radio_config *config)
{
    const sdrplay_api_RxChannelParamsT *channel = channel_for_tuner_const(device, tuner);
    double correction = 1.0 + device->dev_params.ppm / 1000000.0;
    memset(config, 0, sizeof(*config));
    config->sample_rate_hz = (uint32_t)device->dev_params.fsFreq.fsHz;
    config->center_frequency_hz = (uint32_t)(channel->tunerParams.rfFreq.rfHz / correction);
    config->bandwidth_hz = (uint32_t)channel->tunerParams.bwType * 1000u;
    config->if_frequency_hz = (int32_t)channel->tunerParams.ifType * 1000;
    /* The public API retains the caller's nominal 6/8 MHz RSPduo bandwidth,
     * while the shared direct-dual hardware path operates each low-IF lane at
     * no more than 1.536 MHz.  Keep every role-specific daemon request in the
     * same normalized form used to start the endpoint; otherwise a later gain
     * update is rejected because it appears to change a shared bandwidth. */
    if (device->duo_role != 0u && config->bandwidth_hz > 1536000u)
        config->bandwidth_hz = 1536000u;
    config->gain_reduction_db = channel->tunerParams.gain.gRdB;
    config->lna_state = channel->tunerParams.gain.LNAstate;
    config->agc_mode = channel->ctrlParams.agc.enable;
    config->agc_setpoint_dbfs = channel->ctrlParams.agc.setPoint_dBfs;
    config->tuner = tuner == sdrplay_api_Tuner_B ? OPENRSP_TUNER_B : OPENRSP_TUNER_A;
    config->bias_tee_enabled = channel->rspDuoTunerParams.biasTEnable != 0u;
    config->rf_notch_enabled = channel->rspDuoTunerParams.rfNotchEnable != 0u;
    config->dab_notch_enabled = channel->rspDuoTunerParams.rfDabNotchEnable != 0u;
    config->external_reference_enabled =
        device->dev_params.rspDuoParams.extRefOutputEn != 0;
    config->am_port_select = channel->rspDuoTunerParams.tuner1AmPortSel;
    config->am_notch_enabled =
        channel->rspDuoTunerParams.tuner1AmNotchEnable != 0u;
}

static void fill_radio_config(const compat_device_context *device,
                              openrsp_radio_config *config)
{
    fill_radio_config_tuner(device, device->tuner, config);
}

static uint32_t enabled_rspduo_control_flags(const openrsp_radio_config *config)
{
    uint32_t flags = 0u;
    if (config->bias_tee_enabled != 0u) flags |= OPENRSP_CHANGE_BIAS_TEE;
    if (config->rf_notch_enabled != 0u) flags |= OPENRSP_CHANGE_RF_NOTCH;
    if (config->dab_notch_enabled != 0u) flags |= OPENRSP_CHANGE_DAB_NOTCH;
    if (config->external_reference_enabled != 0u) flags |= OPENRSP_CHANGE_EXT_REF;
    if (config->am_port_select == sdrplay_api_RspDuo_AMPORT_1)
        flags |= OPENRSP_CHANGE_AM_PORT;
    if (config->am_notch_enabled != 0u) flags |= OPENRSP_CHANGE_AM_NOTCH;
    return flags;
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
    if ((reason & sdrplay_api_Update_Tuner_GrLimits) != 0u)
        flags |= OPENRSP_CHANGE_GAIN;
    if ((reason & sdrplay_api_Update_Ctrl_Agc) != 0u) flags |= OPENRSP_CHANGE_AGC;
    if ((reason & sdrplay_api_Update_RspDuo_BiasTControl) != 0u)
        flags |= OPENRSP_CHANGE_BIAS_TEE;
    if ((reason & sdrplay_api_Update_RspDuo_RfNotchControl) != 0u)
        flags |= OPENRSP_CHANGE_RF_NOTCH;
    if ((reason & sdrplay_api_Update_RspDuo_RfDabNotchControl) != 0u)
        flags |= OPENRSP_CHANGE_DAB_NOTCH;
    if ((reason & sdrplay_api_Update_RspDuo_ExtRefControl) != 0u)
        flags |= OPENRSP_CHANGE_EXT_REF;
    if ((reason & sdrplay_api_Update_RspDuo_AmPortSelect) != 0u)
        flags |= OPENRSP_CHANGE_AM_PORT;
    if ((reason & sdrplay_api_Update_RspDuo_Tuner1AmNotchControl) != 0u)
        flags |= OPENRSP_CHANGE_AM_NOTCH;
    return flags;
}

static sdrplay_api_ErrT validate_update(const compat_device_context *device,
                                        sdrplay_api_TunerSelectT tuner,
                                        sdrplay_api_ReasonForUpdateT reason,
                                        sdrplay_api_ReasonForUpdateExtension1T extension)
{
    const sdrplay_api_RxChannelParamsT *channel = channel_for_tuner_const(device, tuner);
    const uint32_t other_models =
        sdrplay_api_Update_Rsp1a_BiasTControl |
        sdrplay_api_Update_Rsp1a_RfNotchControl |
        sdrplay_api_Update_Rsp1a_RfDabNotchControl |
        sdrplay_api_Update_Rsp2_BiasTControl |
        sdrplay_api_Update_Rsp2_AmPortSelect |
        sdrplay_api_Update_Rsp2_AntennaControl |
        sdrplay_api_Update_Rsp2_RfNotchControl |
        sdrplay_api_Update_Rsp2_ExtRefControl;
    if ((extension & ~0x7fu) != 0u) return sdrplay_api_InvalidParam;
    if ((extension & (sdrplay_api_Update_RspDx_HdrEnable |
                      sdrplay_api_Update_RspDx_BiasTControl |
                      sdrplay_api_Update_RspDx_AntennaControl |
                      sdrplay_api_Update_RspDx_RfNotchControl |
                      sdrplay_api_Update_RspDx_RfDabNotchControl |
                      sdrplay_api_Update_RspDx_HdrBw)) != 0u)
        return sdrplay_api_HwVerError;
    if ((extension & sdrplay_api_Update_RspDuo_ResetSlaveFlags) != 0u) {
        if (device->mode != sdrplay_api_RspDuoMode_Master ||
            device->duo_role != OPENRSP_DUO_ROLE_MASTER)
            return sdrplay_api_InvalidMode;
    }
    if ((reason & other_models) != 0u) return sdrplay_api_HwVerError;
    if ((reason & (sdrplay_api_Update_RspDuo_AmPortSelect |
                   sdrplay_api_Update_RspDuo_Tuner1AmNotchControl)) != 0u &&
        tuner != sdrplay_api_Tuner_A)
        return sdrplay_api_OutOfRange;
    if ((reason & sdrplay_api_Update_RspDuo_BiasTControl) != 0u &&
        tuner != sdrplay_api_Tuner_B)
        return sdrplay_api_InvalidParam;
    if (channel->rspDuoTunerParams.tuner1AmPortSel !=
            sdrplay_api_RspDuo_AMPORT_1 &&
        channel->rspDuoTunerParams.tuner1AmPortSel !=
            sdrplay_api_RspDuo_AMPORT_2)
        return sdrplay_api_InvalidParam;
    if (device->mode == sdrplay_api_RspDuoMode_Dual_Tuner &&
        (reason & (sdrplay_api_Update_Dev_Fs |
                   sdrplay_api_Update_Tuner_BwType |
                   sdrplay_api_Update_Tuner_IfType)) != 0u)
        return sdrplay_api_InvalidMode;
    if ((reason & (sdrplay_api_Update_Master_Spare_1 |
                   sdrplay_api_Update_Master_Spare_2)) != 0u)
        return sdrplay_api_InvalidParam;
    if ((reason & sdrplay_api_Update_Dev_Fs) != 0u &&
        (!isfinite(device->dev_params.fsFreq.fsHz) ||
         device->dev_params.fsFreq.fsHz < 2000000.0 ||
         device->dev_params.fsFreq.fsHz > 10660000.0))
        return sdrplay_api_OutOfRange;
    if ((reason & sdrplay_api_Update_Dev_Fs) != 0u &&
        validate_dsp_configuration(
            channel, device->dev_params.fsFreq.fsHz) != 0)
        return sdrplay_api_OutOfRange;
    if ((reason & sdrplay_api_Update_Dev_Ppm) != 0u &&
        (!isfinite(device->dev_params.ppm) || device->dev_params.ppm < -300.0 ||
         device->dev_params.ppm > 300.0))
        return sdrplay_api_OutOfRange;
    if ((reason & sdrplay_api_Update_Tuner_Frf) != 0u &&
        (!isfinite(channel->tunerParams.rfFreq.rfHz) ||
         channel->tunerParams.rfFreq.rfHz < 0.0 ||
         channel->tunerParams.rfFreq.rfHz > 2000000000.0))
        return sdrplay_api_OutOfRange;
    if ((reason & sdrplay_api_Update_Tuner_BwType) != 0u &&
        !valid_bandwidth(channel->tunerParams.bwType))
        return sdrplay_api_OutOfRange;
    if ((reason & sdrplay_api_Update_Tuner_IfType) != 0u &&
        !valid_if(channel->tunerParams.ifType))
        return sdrplay_api_OutOfRange;
    if ((reason & (sdrplay_api_Update_Tuner_BwType |
                   sdrplay_api_Update_Tuner_IfType)) != 0u &&
        channel->tunerParams.ifType != sdrplay_api_IF_Zero &&
        channel->tunerParams.bwType > sdrplay_api_BW_1_536)
        return sdrplay_api_InvalidMode;
    if ((reason & sdrplay_api_Update_Tuner_GrLimits) != 0u &&
        channel->tunerParams.gain.minGr != sdrplay_api_EXTENDED_MIN_GR &&
        channel->tunerParams.gain.minGr != sdrplay_api_NORMAL_MIN_GR)
        return sdrplay_api_InvalidParam;
    if ((reason & (sdrplay_api_Update_Tuner_Gr |
                   sdrplay_api_Update_Tuner_GrLimits |
                   sdrplay_api_Update_RspDuo_AmPortSelect)) != 0u) {
        const sdrplay_api_GainT *gain = &channel->tunerParams.gain;
        int lna_reduction = 0;
        if (gain->gRdB < (int)gain->minGr || gain->gRdB > 59 ||
            rspduo_lna_gain_reduction(channel->tunerParams.rfFreq.rfHz,
                                      gain->LNAstate, &lna_reduction) < 0 ||
            !rspduo_am_port_lna_state_valid(channel, tuner))
            return sdrplay_api_OutOfRange;
    }
    /* API 3.15 section 3.17 intentionally maps the DcOffset reason to
     * tunerParams.loMode and the LoMode reason to dcOffsetTuner.*. */
    if ((reason & sdrplay_api_Update_Tuner_DcOffset) != 0u &&
        channel->tunerParams.loMode != sdrplay_api_LO_Auto)
        return sdrplay_api_InvalidMode;
    if ((reason & sdrplay_api_Update_Ctrl_Decimation) != 0u) {
        const sdrplay_api_DecimationT *decimation = &channel->ctrlParams.decimation;
        unsigned int factor = decimation->enable != 0u ? decimation->decimationFactor : 1u;
        if (!valid_decimation_factor(factor)) return sdrplay_api_OutOfRange;
    }
    if ((reason & sdrplay_api_Update_Tuner_LoMode) != 0u &&
        (channel->tunerParams.dcOffsetTuner.trackTime <= 0 ||
         channel->tunerParams.dcOffsetTuner.refreshRateTime <= 0))
        return sdrplay_api_OutOfRange;
    if ((reason & sdrplay_api_Update_Ctrl_Agc) != 0u &&
        (channel->ctrlParams.agc.enable < sdrplay_api_AGC_DISABLE ||
         channel->ctrlParams.agc.enable > sdrplay_api_AGC_CTRL_EN ||
         channel->ctrlParams.agc.setPoint_dBfs <
             agc_minimum_setpoint(device->dev_params.fsFreq.fsHz) ||
         channel->ctrlParams.agc.setPoint_dBfs >
             agc_maximum_setpoint(&channel->tunerParams.gain)))
        return sdrplay_api_OutOfRange;
    if ((reason & sdrplay_api_Update_Ctrl_AdsbMode) != 0u) {
        openrsp_adsb_filter filter;
        if (channel->ctrlParams.adsbMode < sdrplay_api_ADSB_DECIMATION ||
            channel->ctrlParams.adsbMode >
                sdrplay_api_ADSB_NO_DECIMATION_BANDPASS_3MHZ ||
            openrsp_adsb_filter_configure(
                &filter, device->dev_params.fsFreq.fsHz,
                (openrsp_adsb_mode)channel->ctrlParams.adsbMode) != 0)
            return sdrplay_api_OutOfRange;
    }
    return sdrplay_api_Success;
}

static int apply_rspduo_gain_locked(compat_device_context *device,
                                    sdrplay_api_TunerSelectT tuner)
{
    sdrplay_api_RxChannelParamsT *channel = channel_for_tuner(device, tuner);
    sdrplay_api_GainT *gain = &channel->tunerParams.gain;
    int lna_reduction;
    if (gain->gRdB < (int)gain->minGr || gain->gRdB > 59 ||
        rspduo_lna_gain_reduction(channel->tunerParams.rfFreq.rfHz,
                                  gain->LNAstate, &lna_reduction) < 0 ||
        !rspduo_am_port_lna_state_valid(channel, tuner)) return -1;

    openrsp_radio_config config;
    fill_radio_config_tuner(device, tuner, &config);
    int result = device->duo_role != 0u ?
        openrsp_daemon_backend_update_duo(device->backend, &config,
                                          OPENRSP_CHANGE_GAIN) :
        openrsp_daemon_backend_update(device->backend, &config,
                                      OPENRSP_CHANGE_GAIN);
    if (result >= 0) {
        result = openrsp_rspduo_gain_values(
            channel->tunerParams.rfFreq.rfHz, gain->LNAstate, gain->gRdB,
            (int)gain->minGr,
            tuner == sdrplay_api_Tuner_A &&
                channel->rspDuoTunerParams.tuner1AmPortSel ==
                    sdrplay_api_RspDuo_AMPORT_1,
            &gain->gainVals.curr, &gain->gainVals.max, &gain->gainVals.min);
    }
    return result;
}

static unsigned int agc_period_ms(sdrplay_api_AgcControlT mode)
{
    if (mode == sdrplay_api_AGC_100HZ) return 10u;
    if (mode == sdrplay_api_AGC_50HZ || mode == sdrplay_api_AGC_CTRL_EN) return 20u;
    return 200u;
}

static uint64_t monotonic_milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0u;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static unsigned int agc_ctrl_interval_ms(unsigned short configured_ms)
{
    unsigned int interval = (unsigned int)configured_ms / 5u;
    return interval < 50u ? 50u : interval;
}

static void restore_agc_peak(atomic_uint *peak_state, unsigned int peak)
{
    unsigned int current = atomic_load(peak_state);
    while (peak > current &&
           !atomic_compare_exchange_weak(peak_state, &current, peak)) {
    }
}

static void *agc_thread_main(void *opaque)
{
    compat_device_context *device = opaque;
    while (!atomic_load(&device->agc_stop)) {
        const struct timespec delay = {
            .tv_sec = 0,
            .tv_nsec = 10000000L
        };
        nanosleep(&delay, NULL);
        unsigned int tuner_count = device->mode == sdrplay_api_RspDuoMode_Dual_Tuner ? 2u : 1u;
        for (unsigned int slot = 0u; slot < tuner_count; ++slot) {
            sdrplay_api_TunerSelectT tuner = tuner_count == 1u ? device->tuner :
                (slot == 0u ? sdrplay_api_Tuner_A : sdrplay_api_Tuner_B);
            atomic_int *mode_state = tuner == sdrplay_api_Tuner_B ?
                &device->agc_mode_b : &device->agc_mode;
            atomic_int *setpoint_state = tuner == sdrplay_api_Tuner_B ?
                &device->agc_setpoint_b : &device->agc_setpoint;
            atomic_uint *peak_state = tuner == sdrplay_api_Tuner_B ?
                &device->agc_peak_b : &device->agc_peak;
            sdrplay_api_AgcControlT mode =
                (sdrplay_api_AgcControlT)atomic_load(mode_state);
            unsigned int peak = atomic_exchange(peak_state, 0u);
            if (mode == sdrplay_api_AGC_DISABLE || peak == 0u || !device->initialized)
                continue;
            double level_dbfs = 20.0 * log10((double)peak / 32767.0);
            pthread_mutex_lock(&hardware_lock);
            sdrplay_api_RxChannelParamsT *channel = channel_for_tuner(device, tuner);
            int target = clamp_int(atomic_load(setpoint_state),
                                   agc_minimum_setpoint(device->dev_params.fsFreq.fsHz),
                                   agc_maximum_setpoint(&channel->tunerParams.gain));
            double error = level_dbfs - (double)target;
            unsigned int tuner_slot = tuner == sdrplay_api_Tuner_B ? 1u : 0u;
            uint64_t now_ms = monotonic_milliseconds();
            int step = 0;
            int deferred = 0;
            if (mode == sdrplay_api_AGC_CTRL_EN) {
                const sdrplay_api_AgcT *agc = &channel->ctrlParams.agc;
                if (error > 1.5) {
                    device->agc_decay_since_ms[tuner_slot] = 0u;
                    unsigned int interval = agc_ctrl_interval_ms(agc->attack_ms);
                    if (now_ms - device->agc_last_adjust_ms[tuner_slot] >= interval)
                        step = clamp_int((int)ceil(error * 0.4), 1, 24);
                    else
                        deferred = 1;
                } else {
                    double threshold = agc->decay_threshold_dB > 0u ?
                        (double)agc->decay_threshold_dB : 1.5;
                    if (error < -threshold) {
                        unsigned int interval = agc_ctrl_interval_ms(agc->decay_ms);
                        if (device->agc_decay_since_ms[tuner_slot] == 0u)
                            device->agc_decay_since_ms[tuner_slot] = now_ms;
                        uint64_t decay_ready =
                            device->agc_decay_since_ms[tuner_slot] +
                            (uint64_t)agc->decay_delay_ms + interval;
                        if (now_ms >= decay_ready &&
                            now_ms - device->agc_last_adjust_ms[tuner_slot] >= interval)
                            step = -clamp_int((int)ceil(-error * 0.8), 1, 24);
                        else
                            deferred = 1;
                    } else {
                        device->agc_decay_since_ms[tuner_slot] = 0u;
                    }
                }
            } else if (now_ms - device->agc_last_adjust_ms[tuner_slot] >=
                       agc_period_ms(mode)) {
                step = error > 6.0 ? 3 : error > 1.5 ? 1 :
                       error < -6.0 ? -2 : error < -1.5 ? -1 : 0;
            } else {
                deferred = 1;
            }
            if (step == 0) {
                if (deferred) restore_agc_peak(peak_state, peak);
                pthread_mutex_unlock(&hardware_lock);
                continue;
            }
            if (atomic_load(&device->sync_waiting[tuner == sdrplay_api_Tuner_B ?
                                                   1u : 0u]) != 0) {
                pthread_mutex_unlock(&hardware_lock);
                continue;
            }
            sdrplay_api_EventParamsT event_params;
            int notify_gain = 0;
            int old_reduction = channel->tunerParams.gain.gRdB;
            int new_reduction = clamp_int(old_reduction + step,
                                          (int)channel->tunerParams.gain.minGr, 59);
            if (new_reduction != old_reduction) {
                channel->tunerParams.gain.gRdB = new_reduction;
                if (apply_rspduo_gain_locked(device, tuner) >= 0) {
                    device->agc_last_adjust_ms[tuner_slot] =
                        monotonic_milliseconds();
                    atomic_fetch_add(tuner == sdrplay_api_Tuner_B ?
                                     &device->pending_gr_changed_b :
                                     &device->pending_gr_changed, 1u);
                    int lna_reduction = 0;
                    (void)rspduo_lna_gain_reduction(
                        channel->tunerParams.rfFreq.rfHz,
                        channel->tunerParams.gain.LNAstate, &lna_reduction);
                    memset(&event_params, 0, sizeof(event_params));
                    event_params.gainParams.gRdB = (unsigned int)new_reduction;
                    event_params.gainParams.lnaGRdB = (unsigned int)lna_reduction;
                    event_params.gainParams.currGain = channel->tunerParams.gain.gainVals.curr;
                    notify_gain = 1;
                } else {
                    channel->tunerParams.gain.gRdB = old_reduction;
                }
            }
            pthread_mutex_unlock(&hardware_lock);
            if (notify_gain)
                (void)queue_api_event(device, sdrplay_api_GainChange,
                                      tuner, &event_params);
        }
    }
    return NULL;
}

static void emit_overload_event(compat_device_context *device,
                                sdrplay_api_TunerSelectT tuner, int overloaded)
{
    sdrplay_api_EventParamsT params;
    memset(&params, 0, sizeof(params));
    params.powerOverloadParams.powerOverloadChangeType =
        overloaded ? sdrplay_api_Overload_Detected : sdrplay_api_Overload_Corrected;
    atomic_int *reported = tuner == sdrplay_api_Tuner_B ?
        &device->overload_reported_state_b : &device->overload_reported_state;
    atomic_int *pending = tuner == sdrplay_api_Tuner_B ?
        &device->overload_event_pending_b : &device->overload_event_pending;
    if (queue_api_event(device, sdrplay_api_PowerOverloadChange,
                        tuner, &params) == 0) {
        atomic_store(reported, overloaded);
        atomic_store(pending, 1);
    } else {
        atomic_store(pending, 0);
    }
}

static void *stream_callback_thread_main(void *opaque)
{
    compat_device_context *device = opaque;
    for (;;) {
        (void)pthread_mutex_lock(&device->stream_callback_lock);
        while ((device->stream_callback_count == 0u ||
                device->stream_callback_paused) && !device->stream_callback_stop)
            (void)pthread_cond_wait(&device->stream_callback_ready,
                                    &device->stream_callback_lock);
        if (device->stream_callback_stop) {
            (void)pthread_mutex_unlock(&device->stream_callback_lock);
            break;
        }
        compat_stream_frame *frame =
            &device->stream_queue[device->stream_callback_head];
        size_t samples = frame->samples;
        sdrplay_api_TunerSelectT tuner = frame->tuner;
        unsigned int first_sample = frame->first_sample;
        int *seen = tuner == sdrplay_api_Tuner_B ? &device->stream_callback_seen_b :
                                                   &device->stream_callback_seen;
        unsigned int *next = tuner == sdrplay_api_Tuner_B ?
            &device->stream_callback_next_sample_b : &device->stream_callback_next_sample;
        unsigned int reset = frame->reset || !*seen || first_sample != *next;
        int gr_changed = frame->gr_changed;
        int rf_changed = frame->rf_changed;
        int fs_changed = frame->fs_changed;
        memcpy(device->callback_i, frame->xi, samples * sizeof(*device->callback_i));
        memcpy(device->callback_q, frame->xq, samples * sizeof(*device->callback_q));
        device->stream_callback_head =
            (device->stream_callback_head + 1u) % OPENRSP_STREAM_QUEUE_CAPACITY;
        --device->stream_callback_count;
        device->stream_callback_active = 1;
        *seen = 1;
        *next = first_sample + (unsigned int)samples;
        (void)pthread_mutex_unlock(&device->stream_callback_lock);

        if (samples == 0u) {
            sdrplay_api_StreamCbParamsT params = {
                .firstSampleNum = first_sample,
                .grChanged = gr_changed,
                .rfChanged = rf_changed,
                .fsChanged = fs_changed,
                .numSamples = 0u
            };
            sdrplay_api_StreamCallback_t callback =
                tuner == sdrplay_api_Tuner_B &&
                device->mode == sdrplay_api_RspDuoMode_Dual_Tuner ?
                device->callbacks.StreamBCbFn : device->callbacks.StreamACbFn;
            callback(device->callback_i, device->callback_q, &params, 0u, reset,
                     device->callback_context);
            (void)pthread_mutex_lock(&device->stream_callback_lock);
            device->stream_callback_active = 0;
            (void)pthread_cond_broadcast(&device->stream_callback_ready);
            (void)pthread_mutex_unlock(&device->stream_callback_lock);
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
            sdrplay_api_StreamCallback_t callback =
                tuner == sdrplay_api_Tuner_B &&
                device->mode == sdrplay_api_RspDuoMode_Dual_Tuner ?
                device->callbacks.StreamBCbFn : device->callbacks.StreamACbFn;
            callback(device->callback_i + offset, device->callback_q + offset,
                     &params, chunk, offset == 0u ? reset : 0u,
                     device->callback_context);
            offset += chunk;
        }
        (void)pthread_mutex_lock(&device->stream_callback_lock);
        device->stream_callback_active = 0;
        (void)pthread_cond_broadcast(&device->stream_callback_ready);
        (void)pthread_mutex_unlock(&device->stream_callback_lock);
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
    device->stream_callback_paused = 0;
    device->stream_callback_active = 0;
    device->stream_callback_head = 0u;
    device->stream_callback_count = 0u;
    device->stream_callback_drop_count = 0u;
    device->stream_callback_seen = 0;
    device->stream_callback_seen_b = 0;
    if (pthread_create(&device->stream_callback_thread, NULL,
                       stream_callback_thread_main, device) != 0) {
        (void)pthread_cond_destroy(&device->stream_callback_ready);
        (void)pthread_mutex_destroy(&device->stream_callback_lock);
        return -1;
    }
    device->stream_callback_thread_started = 1;
    return 0;
}

static void pause_stream_callbacks(compat_device_context *device)
{
    (void)pthread_mutex_lock(&device->stream_callback_lock);
    device->stream_callback_paused = 1;
    while (device->stream_callback_active)
        (void)pthread_cond_wait(&device->stream_callback_ready,
                                &device->stream_callback_lock);
    device->stream_callback_head = 0u;
    device->stream_callback_count = 0u;
    (void)pthread_mutex_unlock(&device->stream_callback_lock);
}

static void resume_stream_callbacks(compat_device_context *device)
{
    (void)pthread_mutex_lock(&device->stream_callback_lock);
    device->stream_callback_paused = 0;
    (void)pthread_cond_signal(&device->stream_callback_ready);
    (void)pthread_mutex_unlock(&device->stream_callback_lock);
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

static void queue_stream_callback(compat_device_context *device,
                                  sdrplay_api_TunerSelectT tuner, size_t samples,
                                  unsigned int first_sample, unsigned int reset,
                                  int gr_changed, int rf_changed, int fs_changed)
{
    (void)pthread_mutex_lock(&device->stream_callback_lock);
    /* A mode/tuner transition clears the old callback epoch.  Frames that
     * race with the daemon transition belong to that old epoch and must not
     * be interpreted using the new callback/tuner mapping. */
    if (device->stream_callback_paused || device->stream_callback_stop) {
        (void)pthread_mutex_unlock(&device->stream_callback_lock);
        return;
    }
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
            openrsp_debug_log(sdrplay_api_DbgLvl_Warning,
                              "OPENRSP_API_CALLBACK_DROP count=%u\n", drops);
    }
    unsigned int tail = (device->stream_callback_head +
                         device->stream_callback_count) % OPENRSP_STREAM_QUEUE_CAPACITY;
    compat_stream_frame *frame = &device->stream_queue[tail];
    frame->samples = samples;
    frame->tuner = tuner;
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
                                   uint32_t sequence, uint32_t protocol_tuner,
                                   void *opaque)
{
    compat_device_context *device = opaque;
    sdrplay_api_TunerSelectT tuner =
        device->mode == sdrplay_api_RspDuoMode_Dual_Tuner ?
        (protocol_tuner == OPENRSP_TUNER_B ? sdrplay_api_Tuner_B :
                                             sdrplay_api_Tuner_A) :
        device->tuner;
    uint32_t *last_sequence = tuner == sdrplay_api_Tuner_B ?
        &device->last_iq_sequence_b : &device->last_iq_sequence;
    int *sequence_seen = tuner == sdrplay_api_Tuner_B ?
        &device->iq_sequence_seen_b : &device->iq_sequence_seen;
    atomic_store(&device->stream_state, 1);
    int discontinuity = 0;
    uint32_t missing_frames = 0u;
    if (*sequence_seen) {
        uint32_t expected = *last_sequence + 1u;
        if (sequence != expected) {
            uint32_t distance = sequence - expected;
            discontinuity = 1;
            if (distance < 0x80000000u) missing_frames = distance;
            openrsp_debug_log(
                sdrplay_api_DbgLvl_Warning,
                "OPENRSP_API_IQ_GAP expected=%u received=%u missing_frames=%u\n",
                expected, sequence, missing_frames);
        }
    }
    *last_sequence = sequence;
    *sequence_seen = 1;
    if (samples > device->scratch_capacity || device->scratch_i == NULL ||
        device->scratch_q == NULL) {
        atomic_store(&device->stream_state, -1);
        return;
    }
    short *xi = device->scratch_i;
    short *xq = device->scratch_q;
    unsigned int peak = 0u;
    uint64_t power_sum = 0u;
    for (size_t index = 0; index < samples; ++index) {
        xi[index] = interleaved[index * 2u];
        xq[index] = interleaved[index * 2u + 1u];
        unsigned int abs_i = (unsigned int)abs(xi[index]);
        unsigned int abs_q = (unsigned int)abs(xq[index]);
        unsigned int sample_peak = abs_i > abs_q ? abs_i : abs_q;
        if (sample_peak > peak) peak = sample_peak;
        power_sum += (uint64_t)((int64_t)xi[index] * xi[index]) +
                     (uint64_t)((int64_t)xq[index] * xq[index]);
    }
    atomic_int *overload_state = tuner == sdrplay_api_Tuner_B ?
        &device->overload_state_b : &device->overload_state;
    atomic_int *overload_pending = tuner == sdrplay_api_Tuner_B ?
        &device->overload_event_pending_b : &device->overload_event_pending;
    int overloaded = atomic_load(overload_state);
    int next_overload = overloaded ? peak > 30000u : peak >= 32700u;
    if (next_overload != overloaded) {
        atomic_store(overload_state, next_overload);
        int expected_pending = 0;
        if (atomic_compare_exchange_strong(overload_pending, &expected_pending, 1))
            emit_overload_event(device, tuner, next_overload);
    }
    (void)pthread_mutex_lock(&decimation_lock);
    openrsp_low_if_dsp *low_if = tuner == sdrplay_api_Tuner_B ?
        &device->low_if_dsp_b : &device->low_if_dsp;
    samples = openrsp_iq_correction_process(
        &device->iq_correction[tuner == sdrplay_api_Tuner_B ? 1u : 0u],
        xi, xq, samples);
    samples = openrsp_adsb_filter_process(
        &device->adsb_filter[tuner == sdrplay_api_Tuner_B ? 1u : 0u],
        xi, xq, samples);
    samples = openrsp_low_if_process(low_if, xi, xq, samples);
    int adsb_decimation =
        device->adsb_filter[tuner == sdrplay_api_Tuner_B ? 1u : 0u].mode ==
        OPENRSP_ADSB_DECIMATION;
    (void)pthread_mutex_unlock(&decimation_lock);
    if (adsb_decimation)
        samples = decimate_iq(device, tuner, xi, xq, samples);
    if (missing_frames != 0u)
        (void)atomic_fetch_add(tuner == sdrplay_api_Tuner_B ?
                               &device->sample_number_b : &device->sample_number,
                               missing_frames * (unsigned int)samples);
    unsigned int rms = samples > 0u ?
        (unsigned int)sqrt((double)power_sum / (2.0 * (double)samples)) : 0u;
    atomic_uint *peak_state = tuner == sdrplay_api_Tuner_B ?
        &device->agc_peak_b : &device->agc_peak;
    unsigned int observed = atomic_load(peak_state);
    while (rms > observed &&
           !atomic_compare_exchange_weak(peak_state, &observed, rms)) {}
    atomic_uint *pending_gr = tuner == sdrplay_api_Tuner_B ?
        &device->pending_gr_changed_b : &device->pending_gr_changed;
    atomic_uint *pending_rf = tuner == sdrplay_api_Tuner_B ?
        &device->pending_rf_changed_b : &device->pending_rf_changed;
    atomic_uint *pending_fs = tuner == sdrplay_api_Tuner_B ?
        &device->pending_fs_changed_b : &device->pending_fs_changed;
    atomic_uint *sample_number = tuner == sdrplay_api_Tuner_B ?
        &device->sample_number_b : &device->sample_number;
    unsigned int *first_callback = tuner == sdrplay_api_Tuner_B ?
        &device->first_callback_b : &device->first_callback;
    int gr_changed = consume_one(pending_gr);
    int rf_changed = consume_one(pending_rf);
    int fs_changed = consume_one(pending_fs);
    unsigned int first_sample = atomic_fetch_add(sample_number,
                                                  (unsigned int)samples);
    unsigned int reset = *first_callback || discontinuity;
    queue_stream_callback(device, tuner, samples, first_sample, reset,
                          gr_changed, rf_changed, fs_changed);
    *first_callback = 0;
}

static void daemon_failure_callback(void *opaque)
{
    compat_device_context *device = opaque;
    atomic_store(&device->stream_state, -1);
    openrsp_debug_log(sdrplay_api_DbgLvl_Error,
                      "OPENRSP_API_DEVICE_FAILURE transport_lost=1\n");
    sdrplay_api_EventParamsT params;
    memset(&params, 0, sizeof(params));
    (void)queue_api_event(device, sdrplay_api_DeviceFailure,
                          device->tuner, &params);
}

static void daemon_status_callback(uint32_t reason, uint32_t protocol_tuner,
                                   void *opaque)
{
    compat_device_context *device = opaque;
    if (reason != OPENRSP_DEVICE_STATUS_REMOVED) return;
    if (protocol_tuner != OPENRSP_TUNER_A &&
        protocol_tuner != OPENRSP_TUNER_B &&
        protocol_tuner != OPENRSP_TUNER_BOTH) {
        openrsp_debug_log(sdrplay_api_DbgLvl_Error,
                          "OPENRSP_API_DEVICE_STATUS_INVALID_TUNER tuner=%u\n",
                          protocol_tuner);
        return;
    }
    sdrplay_api_TunerSelectT tuner = protocol_tuner == OPENRSP_TUNER_B ?
        sdrplay_api_Tuner_B : protocol_tuner == OPENRSP_TUNER_BOTH ?
        sdrplay_api_Tuner_Both : sdrplay_api_Tuner_A;
    sdrplay_api_EventParamsT params;
    memset(&params, 0, sizeof(params));
    openrsp_debug_log(sdrplay_api_DbgLvl_Warning,
                      "OPENRSP_API_DEVICE_REMOVED tuner=%u\n",
                      protocol_tuner);
    atomic_store(&device->stream_state, -1);
    if (queue_api_event(device, sdrplay_api_DeviceRemoved, tuner, &params) != 0)
        openrsp_debug_log(sdrplay_api_DbgLvl_Error,
                          "OPENRSP_API_DEVICE_REMOVED_QUEUE_FAILED\n");
}

static void daemon_duo_callback(uint32_t kind, uint32_t protocol_tuner,
                                void *opaque)
{
    compat_device_context *device = opaque;
    if (kind < 1u || kind > 7u ||
        (protocol_tuner != OPENRSP_TUNER_A &&
         protocol_tuner != OPENRSP_TUNER_B))
        return;
    sdrplay_api_EventParamsT params;
    memset(&params, 0, sizeof(params));
    params.rspDuoModeParams.modeChangeType =
        (sdrplay_api_RspDuoModeCbEventIdT)(kind - 1u);
    (void)queue_api_event(
        device, sdrplay_api_RspDuoModeChange,
        protocol_tuner == OPENRSP_TUNER_B ? sdrplay_api_Tuner_B :
                                             sdrplay_api_Tuner_A,
        &params);
}

static void emit_update_ack(compat_device_context *device, int fs_changed,
                            int rf_changed, int gr_changed,
                            sdrplay_api_TunerSelectT tuner)
{
    atomic_uint *sample_number = tuner == sdrplay_api_Tuner_B ?
        &device->sample_number_b : &device->sample_number;
    queue_stream_callback(device, tuner, 0u, atomic_load(sample_number), 0u,
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
    openrsp_debug_reset();
    memset(last_errors, 0, sizeof(last_errors));
    memset(last_error_times, 0, sizeof(last_error_times));
    memset(last_error_present, 0, sizeof(last_error_present));
    atomic_init(&rspduo.pending_gr_changed, 0u);
    atomic_init(&rspduo.sample_number, 0u);
    atomic_init(&rspduo.sample_number_b, 0u);
    atomic_init(&rspduo.pending_rf_changed, 0u);
    atomic_init(&rspduo.pending_fs_changed, 0u);
    atomic_init(&rspduo.pending_gr_changed_b, 0u);
    atomic_init(&rspduo.pending_rf_changed_b, 0u);
    atomic_init(&rspduo.pending_fs_changed_b, 0u);
    atomic_init(&rspduo.stream_state, 0);
    atomic_init(&rspduo.agc_peak, 0u);
    atomic_init(&rspduo.agc_peak_b, 0u);
    atomic_init(&rspduo.agc_stop, 0);
    atomic_init(&rspduo.agc_mode, sdrplay_api_AGC_50HZ);
    atomic_init(&rspduo.agc_setpoint, -60);
    atomic_init(&rspduo.agc_mode_b, sdrplay_api_AGC_50HZ);
    atomic_init(&rspduo.agc_setpoint_b, -60);
    atomic_init(&rspduo.overload_state, 0);
    atomic_init(&rspduo.overload_event_pending, 0);
    atomic_init(&rspduo.overload_reported_state, 0);
    atomic_init(&rspduo.overload_state_b, 0);
    atomic_init(&rspduo.overload_event_pending_b, 0);
    atomic_init(&rspduo.overload_reported_state_b, 0);
    atomic_init(&rspduo.decimator[0].factor, 1u);
    atomic_init(&rspduo.decimator[1].factor, 1u);
    (void)openrsp_low_if_configure(&rspduo.low_if_dsp, 2000000.0, 0);
    (void)openrsp_low_if_configure(&rspduo.low_if_dsp_b, 2000000.0, 0);
    rspduo.params.devParams = &rspduo.dev_params;
    rspduo.params.rxChannelA = &rspduo.channel_a;
    rspduo.params.rxChannelB = &rspduo.channel_b;
    rspduo.tuner = sdrplay_api_Tuner_A;
    rspduo.mode = sdrplay_api_RspDuoMode_Single_Tuner;
    rspduo.dev_params.fsFreq.fsHz = 2000000.0;
    rspduo.dev_params.mode = sdrplay_api_BULK;
    rspduo.channel_a.tunerParams.bwType = sdrplay_api_BW_0_200;
    rspduo.channel_a.tunerParams.ifType = sdrplay_api_IF_Zero;
    rspduo.channel_a.tunerParams.loMode = sdrplay_api_LO_Auto;
    rspduo.channel_a.tunerParams.gain.gRdB = 50;
    rspduo.channel_a.tunerParams.gain.LNAstate = 0;
    rspduo.channel_a.tunerParams.gain.minGr = sdrplay_api_NORMAL_MIN_GR;
    rspduo.channel_a.tunerParams.rfFreq.rfHz = 200000000.0;
    rspduo.channel_a.tunerParams.dcOffsetTuner.dcCal = 3u;
    rspduo.channel_a.tunerParams.dcOffsetTuner.speedUp = 0u;
    rspduo.channel_a.tunerParams.dcOffsetTuner.trackTime = 1;
    rspduo.channel_a.tunerParams.dcOffsetTuner.refreshRateTime = 2048;
    rspduo.channel_a.ctrlParams.dcOffset.DCenable = 1;
    rspduo.channel_a.ctrlParams.dcOffset.IQenable = 1;
    rspduo.channel_a.ctrlParams.decimation.decimationFactor = 1;
    rspduo.channel_a.ctrlParams.agc.enable = sdrplay_api_AGC_50HZ;
    rspduo.channel_a.ctrlParams.agc.setPoint_dBfs = -60;
    rspduo.channel_a.rspDuoTunerParams.tuner1AmPortSel =
        sdrplay_api_RspDuo_AMPORT_2;
    rspduo.channel_b = rspduo.channel_a;
    (void)configure_iq_correction(&rspduo, sdrplay_api_Tuner_A);
    (void)configure_iq_correction(&rspduo, sdrplay_api_Tuner_B);
    (void)openrsp_adsb_filter_configure(&rspduo.adsb_filter[0], 2000000.0,
                                        OPENRSP_ADSB_DECIMATION);
    (void)openrsp_adsb_filter_configure(&rspduo.adsb_filter[1], 2000000.0,
                                        OPENRSP_ADSB_DECIMATION);
    openrsp_sync_update_init(&rspduo.sync_update[0]);
    openrsp_sync_update_init(&rspduo.sync_update[1]);
    atomic_init(&rspduo.sync_waiting[0], 0);
    atomic_init(&rspduo.sync_waiting[1], 0);
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
    if (openrsp_daemon_backend_list(NULL, 0u) < 0) {
        RECORD_LAST_ERROR(0, "sdrplay_api_Open",
                          "OpenRSP daemon is not available");
        (void)pthread_mutex_unlock(&device_api_lock);
        return sdrplay_api_Fail;
    }
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
        return atomic_load(&api_open) ? sdrplay_api_Success : sdrplay_api_NotInitialised;
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
        handle->rspduo_mode_mask = found[index].rspduo_mode_mask;
        handle->rspduo_sample_rate_hz = found[index].rspduo_sample_rate_hz;
        handle->valid = 1;
        sdrplay_api_DeviceT *device = &devices[written++];
        memset(device, 0, sizeof(*device));
        const char *override = getenv("OPENRSP_SERIAL");
        const char *identity = discovered == 1 && override != NULL && override[0] != '\0' ?
                               override :
                               (found[index].serial[0] == '\0' ? "OPENRSP0" : found[index].serial);
        (void)snprintf(device->SerNo, sizeof(device->SerNo), "%s", identity);
        device->hwVer = hw_version;
        device->tuner = found[index].product_id == 0x3020u ?
            (found[index].rspduo_mode_mask == OPENRSP_MODE_CAP_SLAVE ?
             sdrplay_api_Tuner_B : sdrplay_api_Tuner_Both) :
            sdrplay_api_Tuner_A;
        /* GetDevices reports available RSPduo modes as a bit mask.  Once a
         * master owns tuner A, the same physical receiver is advertised to a
         * second process as a tuner-B slave at the master's shared rate. */
        device->rspDuoMode = found[index].product_id == 0x3020u ?
            (sdrplay_api_RspDuoModeT)(found[index].rspduo_mode_mask != 0u ?
                                      found[index].rspduo_mode_mask :
                                      OPENRSP_MODE_CAP_SINGLE |
                                      OPENRSP_MODE_CAP_DUAL) :
            sdrplay_api_RspDuoMode_Unknown;
        device->rspDuoSampleFreq = found[index].rspduo_sample_rate_hz;
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
        "sdrplay_api_Success", "sdrplay_api_Fail", "sdrplay_api_InvalidParam",
        "sdrplay_api_OutOfRange", "sdrplay_api_GainUpdateError",
        "sdrplay_api_RfUpdateError", "sdrplay_api_FsUpdateError",
        "sdrplay_api_HwError", "sdrplay_api_AliasingError",
        "sdrplay_api_AlreadyInitialised", "sdrplay_api_NotInitialised",
        "sdrplay_api_NotEnabled", "sdrplay_api_HwVerError",
        "sdrplay_api_OutOfMemError", "sdrplay_api_ServiceNotResponding",
        "sdrplay_api_StartPending", "sdrplay_api_StopPending",
        "sdrplay_api_InvalidMode", "sdrplay_api_FailedVerification1",
        "sdrplay_api_FailedVerification2", "sdrplay_api_FailedVerification3",
        "sdrplay_api_FailedVerification4", "sdrplay_api_FailedVerification5",
        "sdrplay_api_FailedVerification6", "sdrplay_api_InvalidServiceVersion"
    };
    return (unsigned int)err < sizeof(names) / sizeof(names[0]) ? names[err] : "Unknown Error";
}

sdrplay_api_ErrorInfoT *sdrplay_api_GetLastError(sdrplay_api_DeviceT *device)
{
    (void)device;
    (void)pthread_mutex_lock(&last_error_lock);
    int latest_type = -1;
    unsigned long long latest_time = 0u;
    for (int type = 0; type < OPENRSP_LAST_ERROR_CATEGORY_COUNT; ++type) {
        if (last_error_present[type] != 0u &&
            (latest_type < 0 || last_error_times[type] >= latest_time)) {
            latest_type = type;
            latest_time = last_error_times[type];
        }
    }
    if (latest_type >= 0) last_error_view = last_errors[latest_type];
    (void)pthread_mutex_unlock(&last_error_lock);
    return latest_type >= 0 ? &last_error_view : NULL;
}

sdrplay_api_ErrorInfoT *sdrplay_api_GetLastErrorByType(sdrplay_api_DeviceT *device,
                                                        int type, unsigned long long *time)
{
    (void)device;
    /* API 3.15 defines 0 as a DLL error and 1--3 as DLL-device,
     * service, and service-device errors. Valid empty categories zero the
     * timestamp; unsupported types preserve it, matching the vendor ABI. */
    if (type < 0 || type >= OPENRSP_LAST_ERROR_CATEGORY_COUNT) return NULL;
    (void)pthread_mutex_lock(&last_error_lock);
    int present = last_error_present[type] != 0u;
    if (present) {
        last_error_view = last_errors[type];
        if (time != NULL) *time = last_error_times[type];
    } else if (time != NULL) {
        *time = 0u;
    }
    (void)pthread_mutex_unlock(&last_error_lock);
    return present ? &last_error_view : NULL;
}

sdrplay_api_ErrT sdrplay_api_DisableHeartbeat(void)
{
    if (!atomic_load(&api_open)) return sdrplay_api_NotInitialised;
    if (rspduo.selected) return sdrplay_api_Fail;
    return sdrplay_api_Success;
}

sdrplay_api_ErrT sdrplay_api_DebugEnable(HANDLE dev, sdrplay_api_DbgLvl_t level)
{
    if (!api_open) return sdrplay_api_NotInitialised;
    if (level < sdrplay_api_DbgLvl_Disable || level > sdrplay_api_DbgLvl_Message)
        return sdrplay_api_OutOfRange;
    if (dev != NULL && (dev != &rspduo || !rspduo.selected))
        return sdrplay_api_InvalidParam;
    openrsp_debug_set_level(level);
    openrsp_debug_log(sdrplay_api_DbgLvl_Message,
                      "OPENRSP_API_DEBUG level=%d\n", (int)level);
    return sdrplay_api_Success;
}

sdrplay_api_ErrT sdrplay_api_SelectDevice(sdrplay_api_DeviceT *device)
{
    if (!api_open) return sdrplay_api_NotInitialised;
    if (device == NULL ||
        (device->tuner != sdrplay_api_Tuner_A && device->tuner != sdrplay_api_Tuner_B &&
         device->tuner != sdrplay_api_Tuner_Both) ||
        !device->valid)
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
    if (handle->identity.product_id == 0x3020u &&
        device->rspDuoMode != sdrplay_api_RspDuoMode_Single_Tuner &&
        device->rspDuoMode != sdrplay_api_RspDuoMode_Dual_Tuner &&
        device->rspDuoMode != sdrplay_api_RspDuoMode_Master &&
        device->rspDuoMode != sdrplay_api_RspDuoMode_Slave)
        return sdrplay_api_InvalidMode;
    if (device->rspDuoMode == sdrplay_api_RspDuoMode_Dual_Tuner &&
        device->tuner != sdrplay_api_Tuner_Both) return sdrplay_api_InvalidMode;
    if (device->rspDuoMode == sdrplay_api_RspDuoMode_Dual_Tuner &&
        device->rspDuoSampleFreq != 6000000.0 &&
        device->rspDuoSampleFreq != 8000000.0)
        return sdrplay_api_InvalidMode;
    if (device->rspDuoMode == sdrplay_api_RspDuoMode_Single_Tuner &&
        device->tuner == sdrplay_api_Tuner_Both) return sdrplay_api_InvalidMode;
    if (device->rspDuoMode == sdrplay_api_RspDuoMode_Master &&
        (device->tuner != sdrplay_api_Tuner_A ||
         (device->rspDuoSampleFreq != 6000000.0 &&
          device->rspDuoSampleFreq != 8000000.0)))
        return sdrplay_api_InvalidMode;
    if (device->rspDuoMode == sdrplay_api_RspDuoMode_Slave &&
        (device->tuner != sdrplay_api_Tuner_B ||
         (handle->rspduo_mode_mask & OPENRSP_MODE_CAP_SLAVE) == 0u ||
         (handle->rspduo_sample_rate_hz != 6000000u &&
          handle->rspduo_sample_rate_hz != 8000000u)))
        return sdrplay_api_InvalidMode;
    if (device->tuner == sdrplay_api_Tuner_B &&
        handle->identity.product_id != 0x3020u) return sdrplay_api_InvalidParam;
    rspduo.selected = 1;
    rspduo.identity = handle->identity;
    rspduo.product_id = handle->identity.product_id;
    rspduo.hw_version = handle->hw_version;
    rspduo.tuner = device->tuner;
    rspduo.mode = device->rspDuoMode;
    rspduo.duo_role = device->rspDuoMode == sdrplay_api_RspDuoMode_Master ?
        OPENRSP_DUO_ROLE_MASTER :
        device->rspDuoMode == sdrplay_api_RspDuoMode_Slave ?
        OPENRSP_DUO_ROLE_SLAVE : 0u;
    if (rspduo.duo_role != 0u) {
        uint32_t sample_rate_hz = rspduo.duo_role == OPENRSP_DUO_ROLE_MASTER ?
            (uint32_t)device->rspDuoSampleFreq : handle->rspduo_sample_rate_hz;
        rspduo.dev_params.fsFreq.fsHz = sample_rate_hz;
        device->rspDuoSampleFreq = sample_rate_hz;
        sdrplay_api_RxChannelParamsT *role_channel =
            device->tuner == sdrplay_api_Tuner_B ? &rspduo.channel_b :
                                                   &rspduo.channel_a;
        role_channel->tunerParams.ifType = sample_rate_hz == 6000000u ?
            sdrplay_api_IF_1_620 : sdrplay_api_IF_2_048;
        if (role_channel->tunerParams.bwType > sdrplay_api_BW_1_536)
            role_channel->tunerParams.bwType = sdrplay_api_BW_1_536;
        if (openrsp_daemon_backend_open_duo(
                &rspduo.backend, &rspduo.identity, rspduo.duo_role,
                device->tuner == sdrplay_api_Tuner_B ? OPENRSP_TUNER_B :
                                                       OPENRSP_TUNER_A,
                sample_rate_hz) != 0) {
            rspduo.selected = 0;
            rspduo.duo_role = 0u;
            return sdrplay_api_HwError;
        }
    }
    if (rspduo.mode == sdrplay_api_RspDuoMode_Dual_Tuner) {
        sdrplay_api_If_kHzT dual_if = device->rspDuoSampleFreq == 6000000.0 ?
            sdrplay_api_IF_1_620 : sdrplay_api_IF_2_048;
        rspduo.dev_params.fsFreq.fsHz = device->rspDuoSampleFreq;
        rspduo.channel_a.tunerParams.ifType = dual_if;
        rspduo.channel_b.tunerParams.ifType = dual_if;
    }
    rspduo.params.rxChannelA = device->tuner == sdrplay_api_Tuner_B ? NULL :
                                                                  &rspduo.channel_a;
    rspduo.params.rxChannelB = device->tuner == sdrplay_api_Tuner_A ? NULL :
                                                                  &rspduo.channel_b;
    if (device->tuner == sdrplay_api_Tuner_Both) {
        rspduo.params.rxChannelA = &rspduo.channel_a;
        rspduo.params.rxChannelB = &rspduo.channel_b;
    }
    if (rspduo.duo_role == OPENRSP_DUO_ROLE_MASTER) {
        rspduo.params.devParams = &rspduo.dev_params;
        rspduo.params.rxChannelA = &rspduo.channel_a;
        rspduo.params.rxChannelB = NULL;
    } else if (rspduo.duo_role == OPENRSP_DUO_ROLE_SLAVE) {
        rspduo.params.devParams = NULL;
        rspduo.params.rxChannelA = NULL;
        rspduo.params.rxChannelB = &rspduo.channel_b;
    }
    device->dev = &rspduo;
    return sdrplay_api_Success;
}

sdrplay_api_ErrT sdrplay_api_ReleaseDevice(sdrplay_api_DeviceT *device)
{
    if (!api_open) return sdrplay_api_NotInitialised;
    if (device == NULL || device->dev != &rspduo) return sdrplay_api_InvalidParam;
    if (rspduo.initialized) return sdrplay_api_AlreadyInitialised;
    if (rspduo.backend != NULL) {
        openrsp_daemon_backend_close(rspduo.backend);
        rspduo.backend = NULL;
    }
    device->dev = NULL;
    rspduo.selected = 0;
    memset(&rspduo.identity, 0, sizeof(rspduo.identity));
    rspduo.product_id = 0u;
    rspduo.hw_version = 0u;
    rspduo.duo_role = 0u;
    rspduo.start_pending = 0;
    rspduo.params.devParams = &rspduo.dev_params;
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
    if (callbacks == NULL || callbacks->StreamACbFn == NULL ||
        (rspduo.mode == sdrplay_api_RspDuoMode_Dual_Tuner &&
         callbacks->StreamBCbFn == NULL)) return sdrplay_api_InvalidParam;
    if (rspduo.initialized) return sdrplay_api_AlreadyInitialised;
    sdrplay_api_RxChannelParamsT *channel = active_channel(&rspduo);
    if (validate_dsp_configuration(channel, rspduo.dev_params.fsFreq.fsHz) != 0 ||
        (rspduo.mode == sdrplay_api_RspDuoMode_Dual_Tuner &&
         validate_dsp_configuration(
             &rspduo.channel_b, rspduo.dev_params.fsFreq.fsHz) != 0))
        return sdrplay_api_OutOfRange;
    sdrplay_api_GainT *gain_a = &rspduo.channel_a.tunerParams.gain;
    sdrplay_api_GainT *gain_b = &rspduo.channel_b.tunerParams.gain;
    if ((rspduo.tuner != sdrplay_api_Tuner_B &&
         openrsp_rspduo_gain_values(
             rspduo.channel_a.tunerParams.rfFreq.rfHz, gain_a->LNAstate,
             gain_a->gRdB, (int)gain_a->minGr,
             rspduo.channel_a.rspDuoTunerParams.tuner1AmPortSel ==
                 sdrplay_api_RspDuo_AMPORT_1,
             &gain_a->gainVals.curr, &gain_a->gainVals.max,
             &gain_a->gainVals.min) != 0) ||
        (rspduo.tuner != sdrplay_api_Tuner_A &&
         openrsp_rspduo_gain_values(
             rspduo.channel_b.tunerParams.rfFreq.rfHz, gain_b->LNAstate,
             gain_b->gRdB, (int)gain_b->minGr, 0, &gain_b->gainVals.curr,
             &gain_b->gainVals.max,
             &gain_b->gainVals.min) != 0))
        return sdrplay_api_OutOfRange;
    if (allocate_stream_buffers(&rspduo) != 0) return sdrplay_api_HwError;
    const sdrplay_api_DecimationT *decimation = &channel->ctrlParams.decimation;
    unsigned int factor = decimation->enable != 0u ? decimation->decimationFactor : 1u;
    (void)pthread_mutex_lock(&decimation_lock);
    int dsp_result = configure_iq_correction(&rspduo, rspduo.tuner);
    if (dsp_result == 0)
        dsp_result = openrsp_adsb_filter_configure(
            &rspduo.adsb_filter[rspduo.tuner == sdrplay_api_Tuner_B ? 1u : 0u],
            rspduo.dev_params.fsFreq.fsHz,
            (openrsp_adsb_mode)channel->ctrlParams.adsbMode);
    if (dsp_result == 0)
        dsp_result = openrsp_low_if_configure(
        rspduo.tuner == sdrplay_api_Tuner_B ?
            &rspduo.low_if_dsp_b : &rspduo.low_if_dsp,
        rspduo.dev_params.fsFreq.fsHz,
        (int)channel->tunerParams.ifType * 1000);
    (void)pthread_mutex_unlock(&decimation_lock);
    if (dsp_result != 0 ||
        configure_decimator(&rspduo, rspduo.tuner, factor) != 0) {
        free_stream_buffers(&rspduo);
        return sdrplay_api_OutOfRange;
    }
    if (rspduo.mode == sdrplay_api_RspDuoMode_Dual_Tuner) {
        if ((rspduo.dev_params.fsFreq.fsHz != 6000000.0 &&
             rspduo.dev_params.fsFreq.fsHz != 8000000.0) ||
            rspduo.channel_a.tunerParams.ifType !=
                (rspduo.dev_params.fsFreq.fsHz == 6000000.0 ?
                 sdrplay_api_IF_1_620 : sdrplay_api_IF_2_048) ||
            rspduo.channel_b.tunerParams.ifType != rspduo.channel_a.tunerParams.ifType ||
            rspduo.channel_a.tunerParams.bwType > sdrplay_api_BW_1_536 ||
            rspduo.channel_b.tunerParams.bwType > sdrplay_api_BW_1_536) {
            free_stream_buffers(&rspduo);
            return sdrplay_api_InvalidMode;
        }
        (void)pthread_mutex_lock(&decimation_lock);
        dsp_result = configure_iq_correction(&rspduo, sdrplay_api_Tuner_B);
        if (dsp_result == 0)
            dsp_result = openrsp_adsb_filter_configure(
                &rspduo.adsb_filter[1], rspduo.dev_params.fsFreq.fsHz,
                (openrsp_adsb_mode)rspduo.channel_b.ctrlParams.adsbMode);
        if (dsp_result == 0)
            dsp_result = openrsp_low_if_configure(
                &rspduo.low_if_dsp_b, rspduo.dev_params.fsFreq.fsHz,
                (int)rspduo.channel_b.tunerParams.ifType * 1000);
        (void)pthread_mutex_unlock(&decimation_lock);
        if (dsp_result != 0) {
            free_stream_buffers(&rspduo);
            return sdrplay_api_OutOfRange;
        }
        const sdrplay_api_DecimationT *decimation_b =
            &rspduo.channel_b.ctrlParams.decimation;
        unsigned int factor_b = decimation_b->enable != 0u ?
            decimation_b->decimationFactor : 1u;
        if (configure_decimator(&rspduo, sdrplay_api_Tuner_B, factor_b) != 0) {
            free_stream_buffers(&rspduo);
            return sdrplay_api_OutOfRange;
        }
    }
    int result = 0;
    if (rspduo.backend == NULL) {
        result = openrsp_daemon_backend_open(&rspduo.backend, &rspduo.identity);
        if (result < 0 || rspduo.backend == NULL) {
            fprintf(stderr,
                    "OPENRSP_API_INIT_FAILURE stage=open result=%d role=%u "
                    "mode=%d tuner=%d rate=%.0f\n",
                    result, rspduo.duo_role, rspduo.mode, rspduo.tuner,
                    rspduo.dev_params.fsFreq.fsHz);
            free_stream_buffers(&rspduo);
            return sdrplay_api_HwError;
        }
    }
    openrsp_radio_config config;
    fill_radio_config(&rspduo, &config);
    if (rspduo.duo_role != 0u) {
        result = openrsp_daemon_backend_configure_duo(rspduo.backend, &config);
    } else if (rspduo.mode == sdrplay_api_RspDuoMode_Dual_Tuner) {
        openrsp_dual_config dual = {
            .sample_rate_hz = (uint32_t)rspduo.dev_params.fsFreq.fsHz
        };
        fill_radio_config_tuner(&rspduo, sdrplay_api_Tuner_A, &dual.channel_a);
        fill_radio_config_tuner(&rspduo, sdrplay_api_Tuner_B, &dual.channel_b);
        result = openrsp_daemon_backend_configure_dual(rspduo.backend, &dual);
    } else {
        result = openrsp_daemon_backend_configure(rspduo.backend, &config);
    }
    if (result < 0) {
        fprintf(stderr,
                "OPENRSP_API_INIT_FAILURE stage=configure result=%d role=%u "
                "mode=%d tuner=%d rate=%.0f if=%d bw=%d\n",
                result, rspduo.duo_role, rspduo.mode, rspduo.tuner,
                rspduo.dev_params.fsFreq.fsHz, channel->tunerParams.ifType,
                channel->tunerParams.bwType);
        if (rspduo.duo_role == 0u) {
            openrsp_daemon_backend_close(rspduo.backend);
            rspduo.backend = NULL;
        }
        free_stream_buffers(&rspduo);
        return sdrplay_api_HwError;
    }
    rspduo.callbacks = *callbacks;
    rspduo.callback_context = context;
    if (start_stream_callback_thread(&rspduo) != 0) {
        if (rspduo.duo_role == 0u) {
            openrsp_daemon_backend_close(rspduo.backend);
            rspduo.backend = NULL;
        }
        free_stream_buffers(&rspduo);
        memset(&rspduo.callbacks, 0, sizeof(rspduo.callbacks));
        rspduo.callback_context = NULL;
        return sdrplay_api_OutOfMemError;
    }
    atomic_store(&rspduo.sample_number, 0u);
    atomic_store(&rspduo.sample_number_b, 0u);
    rspduo.last_iq_sequence = 0u;
    rspduo.iq_sequence_seen = 0;
    rspduo.iq_sequence_seen_b = 0;
    atomic_store(&rspduo.stream_state, 0);
    atomic_store(&rspduo.agc_stop, 0);
    atomic_store(&rspduo.agc_peak, 0u);
    atomic_store(&rspduo.agc_peak_b, 0u);
    atomic_store(&rspduo.agc_mode, channel->ctrlParams.agc.enable);
    atomic_store(&rspduo.agc_setpoint,
                 channel->ctrlParams.agc.setPoint_dBfs);
    atomic_store(&rspduo.agc_mode_b, rspduo.channel_b.ctrlParams.agc.enable);
    atomic_store(&rspduo.agc_setpoint_b,
                 rspduo.channel_b.ctrlParams.agc.setPoint_dBfs);
    atomic_store(&rspduo.callback_samples,
                 rspduo.dev_params.fsFreq.fsHz > 9216000.0 ? 2016u : 1344u);
    rspduo.first_callback = 1;
    rspduo.first_callback_b = 1;
    rspduo.initialized = 1;
    int start_result = rspduo.duo_role != 0u ?
        openrsp_daemon_backend_start_duo(
            rspduo.backend, daemon_stream_callback, daemon_status_callback,
            daemon_duo_callback, daemon_failure_callback, &rspduo) :
        openrsp_daemon_backend_start(
            rspduo.backend, daemon_stream_callback, daemon_status_callback,
            daemon_failure_callback, &rspduo);
    if (start_result != 0) {
        fprintf(stderr,
                "OPENRSP_API_INIT_FAILURE stage=start result=%d role=%u "
                "mode=%d tuner=%d rate=%.0f if=%d bw=%d\n",
                start_result, rspduo.duo_role, rspduo.mode, rspduo.tuner,
                rspduo.dev_params.fsFreq.fsHz, channel->tunerParams.ifType,
                channel->tunerParams.bwType);
        rspduo.initialized = 0;
        stop_stream_callback_thread(&rspduo);
        free_stream_buffers(&rspduo);
        if (start_result == -(int)OPENRSP_STATUS_START_PENDING) {
            rspduo.start_pending = 1;
            return sdrplay_api_StartPending;
        }
        if (rspduo.duo_role == 0u) {
            openrsp_daemon_backend_close(rspduo.backend);
            rspduo.backend = NULL;
        }
        return sdrplay_api_HwError;
    }
    rspduo.start_pending = 0;
    rspduo.thread_started = 1;
    if (pthread_create(&rspduo.agc_thread, NULL, agc_thread_main, &rspduo) != 0) {
        if (rspduo.duo_role != 0u)
            (void)openrsp_daemon_backend_stop_duo(rspduo.backend);
        else {
            (void)openrsp_daemon_backend_stop(rspduo.backend);
            openrsp_daemon_backend_close(rspduo.backend);
            rspduo.backend = NULL;
        }
        rspduo.thread_started = 0;
        rspduo.initialized = 0;
        stop_stream_callback_thread(&rspduo);
        free_stream_buffers(&rspduo);
        return sdrplay_api_StartPending;
    }
    rspduo.agc_thread_started = 1;
    const struct timespec startup_poll = {.tv_sec = 0, .tv_nsec = 10000000L};
    /* Normal startup reaches first IQ almost immediately. After the daemon is
     * killed during an active RSPduo bulk stream, however, the replacement
     * daemon may need repeated endpoint-stop/clear/reopen attempts while the
     * device FIFO quiesces. Retain ownership for a bounded 30 seconds instead
     * of aborting recovery after the former two-second window. */
    for (int attempt = 0; attempt < 3000 && atomic_load(&rspduo.stream_state) == 0;
         ++attempt) {
        nanosleep(&startup_poll, NULL);
    }
    if (atomic_load(&rspduo.stream_state) != 1) {
        fprintf(stderr,
                "OPENRSP_API_INIT_FAILURE stage=first-iq state=%d role=%u "
                "mode=%d tuner=%d rate=%.0f if=%d bw=%d\n",
                atomic_load(&rspduo.stream_state), rspduo.duo_role,
                rspduo.mode, rspduo.tuner, rspduo.dev_params.fsFreq.fsHz,
                channel->tunerParams.ifType, channel->tunerParams.bwType);
        if (rspduo.duo_role != 0u)
            (void)openrsp_daemon_backend_stop_duo(rspduo.backend);
        else
            (void)openrsp_daemon_backend_stop(rspduo.backend);
        atomic_store(&rspduo.agc_stop, 1);
        pthread_join(rspduo.agc_thread, NULL);
        if (rspduo.duo_role == 0u) {
            openrsp_daemon_backend_close(rspduo.backend);
            rspduo.backend = NULL;
        }
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
    int gain_result = 0;
    if (rspduo.mode == sdrplay_api_RspDuoMode_Dual_Tuner) {
        fill_radio_config_tuner(&rspduo, sdrplay_api_Tuner_A, &config);
        gain_result = openrsp_daemon_backend_update(
            rspduo.backend, &config,
            OPENRSP_CHANGE_GAIN | enabled_rspduo_control_flags(&config));
        if (gain_result >= 0) {
            fill_radio_config_tuner(&rspduo, sdrplay_api_Tuner_B, &config);
            gain_result = openrsp_daemon_backend_update(
                rspduo.backend, &config,
                OPENRSP_CHANGE_GAIN | enabled_rspduo_control_flags(&config));
        }
    } else if (rspduo.duo_role != 0u) {
        gain_result = openrsp_daemon_backend_update_duo(
            rspduo.backend, &config,
            OPENRSP_CHANGE_GAIN | enabled_rspduo_control_flags(&config));
    } else {
        gain_result = openrsp_daemon_backend_update(
            rspduo.backend, &config,
            OPENRSP_CHANGE_GAIN | enabled_rspduo_control_flags(&config));
    }
    if (gain_result < 0) {
        fprintf(stderr,
                "OPENRSP_API_INIT_FAILURE stage=gain result=%d role=%u "
                "mode=%d tuner=%d rate=%.0f if=%d bw=%d rf=%.0f gr=%d "
                "min_gr=%u lna=%u agc=%d setpoint=%d flags=%u\n",
                gain_result, rspduo.duo_role, rspduo.mode, rspduo.tuner,
                rspduo.dev_params.fsFreq.fsHz, channel->tunerParams.ifType,
                channel->tunerParams.bwType, channel->tunerParams.rfFreq.rfHz,
                channel->tunerParams.gain.gRdB,
                channel->tunerParams.gain.minGr,
                channel->tunerParams.gain.LNAstate,
                channel->ctrlParams.agc.enable,
                channel->ctrlParams.agc.setPoint_dBfs,
                OPENRSP_CHANGE_GAIN | enabled_rspduo_control_flags(&config));
        if (rspduo.duo_role != 0u)
            (void)openrsp_daemon_backend_stop_duo(rspduo.backend);
        else
            (void)openrsp_daemon_backend_stop(rspduo.backend);
        atomic_store(&rspduo.agc_stop, 1);
        pthread_join(rspduo.agc_thread, NULL);
        if (rspduo.duo_role == 0u) {
            openrsp_daemon_backend_close(rspduo.backend);
            rspduo.backend = NULL;
        }
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
    if (rspduo.duo_role != 0u) {
        int stop_result = openrsp_daemon_backend_stop_duo(rspduo.backend);
        if (stop_result == -(int)OPENRSP_STATUS_STOP_PENDING)
            return sdrplay_api_StopPending;
        if (stop_result != 0) return sdrplay_api_HwError;
    }
    atomic_store(&rspduo.agc_stop, 1);
    if (rspduo.agc_thread_started) pthread_join(rspduo.agc_thread, NULL);
    if (rspduo.duo_role == 0u) {
        if (rspduo.thread_started) (void)openrsp_daemon_backend_stop(rspduo.backend);
        openrsp_daemon_backend_close(rspduo.backend);
        rspduo.backend = NULL;
    }
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
    if (dev != &rspduo ||
        (rspduo.mode == sdrplay_api_RspDuoMode_Dual_Tuner ?
         (tuner != sdrplay_api_Tuner_A && tuner != sdrplay_api_Tuner_B &&
          tuner != sdrplay_api_Tuner_Both) :
         tuner != rspduo.tuner)) return sdrplay_api_InvalidParam;
    if (!rspduo.initialized || rspduo.backend == NULL) return sdrplay_api_NotInitialised;
    if (tuner == sdrplay_api_Tuner_Both) {
        /* API 3.15.1 accepts Both in direct-dual mode and applies the current
         * parameter block for each tuner.  Validate both sides before sending
         * either update so a bad B parameter cannot leave only A changed. */
        sdrplay_api_ErrT validation = validate_update(
            &rspduo, sdrplay_api_Tuner_A, reason, extension);
        if (validation == sdrplay_api_Success)
            validation = validate_update(
                &rspduo, sdrplay_api_Tuner_B, reason, extension);
        if (validation != sdrplay_api_Success) return validation;
        if ((reason & sdrplay_api_Update_Dev_SyncUpdate) != 0u) {
            for (unsigned int slot = 0u; slot < 2u; ++slot)
                openrsp_sync_update_schedule(
                    &rspduo.sync_update[slot],
                    OPENRSP_SYNC_GAIN | OPENRSP_SYNC_RF | OPENRSP_SYNC_FS,
                    rspduo.dev_params.syncUpdate.sampleNum,
                    rspduo.dev_params.syncUpdate.period);
            reason &= ~sdrplay_api_Update_Dev_SyncUpdate;
        }
        if ((reason & sdrplay_api_Update_Dev_ResetFlags) != 0u) {
            uint32_t reset = 0u;
            if (rspduo.dev_params.resetFlags.resetGainUpdate != 0u)
                reset |= OPENRSP_SYNC_GAIN;
            if (rspduo.dev_params.resetFlags.resetRfUpdate != 0u)
                reset |= OPENRSP_SYNC_RF;
            if (rspduo.dev_params.resetFlags.resetFsUpdate != 0u)
                reset |= OPENRSP_SYNC_FS;
            for (unsigned int slot = 0u; slot < 2u; ++slot)
                openrsp_sync_update_reset(&rspduo.sync_update[slot], reset);
            if ((reset & OPENRSP_SYNC_GAIN) != 0u) {
                rspduo.channel_a.tunerParams.gain.syncUpdate = 0u;
                rspduo.channel_b.tunerParams.gain.syncUpdate = 0u;
                rspduo.channel_a.ctrlParams.agc.syncUpdate = 0;
                rspduo.channel_b.ctrlParams.agc.syncUpdate = 0;
            }
            if ((reset & OPENRSP_SYNC_RF) != 0u) {
                rspduo.channel_a.tunerParams.rfFreq.syncUpdate = 0u;
                rspduo.channel_b.tunerParams.rfFreq.syncUpdate = 0u;
            }
            if ((reset & OPENRSP_SYNC_FS) != 0u)
                rspduo.dev_params.fsFreq.syncUpdate = 0u;
            rspduo.dev_params.resetFlags.resetGainUpdate = 0u;
            rspduo.dev_params.resetFlags.resetRfUpdate = 0u;
            rspduo.dev_params.resetFlags.resetFsUpdate = 0u;
            reason &= ~sdrplay_api_Update_Dev_ResetFlags;
        }
        if (reason == sdrplay_api_Update_None) return sdrplay_api_Success;

        /* A recursive A-then-B wait lets A update while B is still short of
         * its requested boundary.  When either side requests synchronized
         * gain/RF state, hold both AGC loops, wait for both timelines first,
         * then perform the existing validated per-tuner updates without a
         * second wait. */
        {
            sdrplay_api_RxChannelParamsT *channel_a = &rspduo.channel_a;
            sdrplay_api_RxChannelParamsT *channel_b = &rspduo.channel_b;
            uint32_t categories_a = 0u;
            uint32_t categories_b = 0u;
            if ((reason & sdrplay_api_Update_Tuner_Gr) != 0u) {
                if (channel_a->tunerParams.gain.syncUpdate != 0u)
                    categories_a |= OPENRSP_SYNC_GAIN;
                if (channel_b->tunerParams.gain.syncUpdate != 0u)
                    categories_b |= OPENRSP_SYNC_GAIN;
            }
            if ((reason & sdrplay_api_Update_Ctrl_Agc) != 0u) {
                if (channel_a->ctrlParams.agc.syncUpdate != 0)
                    categories_a |= OPENRSP_SYNC_GAIN;
                if (channel_b->ctrlParams.agc.syncUpdate != 0)
                    categories_b |= OPENRSP_SYNC_GAIN;
            }
            if ((reason & sdrplay_api_Update_Tuner_Frf) != 0u) {
                if (channel_a->tunerParams.rfFreq.syncUpdate != 0u)
                    categories_a |= OPENRSP_SYNC_RF;
                if (channel_b->tunerParams.rfFreq.syncUpdate != 0u)
                    categories_b |= OPENRSP_SYNC_RF;
            }
            if (categories_a != 0u || categories_b != 0u) {
                atomic_store(&rspduo.sync_waiting[0], 1);
                atomic_store(&rspduo.sync_waiting[1], 1);
                int wait_a = categories_a == 0u ? 0 :
                    wait_for_sync_boundary(
                        &rspduo, sdrplay_api_Tuner_A, categories_a);
                int wait_b = wait_a != 0 || categories_b == 0u ? wait_a :
                    wait_for_sync_boundary(
                        &rspduo, sdrplay_api_Tuner_B, categories_b);
                if (wait_a != 0 || wait_b != 0) {
                    atomic_store(&rspduo.sync_waiting[0], 0);
                    atomic_store(&rspduo.sync_waiting[1], 0);
                    RECORD_LAST_ERROR(
                        3,
                        "sdrplay_api_Update",
                        "dual synchronized update boundary was not reached");
                    return sdrplay_api_ServiceNotResponding;
                }
                unsigned char gain_sync_a = channel_a->tunerParams.gain.syncUpdate;
                unsigned char gain_sync_b = channel_b->tunerParams.gain.syncUpdate;
                int agc_sync_a = channel_a->ctrlParams.agc.syncUpdate;
                int agc_sync_b = channel_b->ctrlParams.agc.syncUpdate;
                unsigned char rf_sync_a = channel_a->tunerParams.rfFreq.syncUpdate;
                unsigned char rf_sync_b = channel_b->tunerParams.rfFreq.syncUpdate;
                channel_a->tunerParams.gain.syncUpdate = 0u;
                channel_b->tunerParams.gain.syncUpdate = 0u;
                channel_a->ctrlParams.agc.syncUpdate = 0;
                channel_b->ctrlParams.agc.syncUpdate = 0;
                channel_a->tunerParams.rfFreq.syncUpdate = 0u;
                channel_b->tunerParams.rfFreq.syncUpdate = 0u;
                sdrplay_api_ErrT result = sdrplay_api_Update(
                    dev, sdrplay_api_Tuner_A, reason, extension);
                if (result == sdrplay_api_Success)
                    result = sdrplay_api_Update(
                        dev, sdrplay_api_Tuner_B, reason, extension);
                channel_a->tunerParams.gain.syncUpdate = gain_sync_a;
                channel_b->tunerParams.gain.syncUpdate = gain_sync_b;
                channel_a->ctrlParams.agc.syncUpdate = agc_sync_a;
                channel_b->ctrlParams.agc.syncUpdate = agc_sync_b;
                channel_a->tunerParams.rfFreq.syncUpdate = rf_sync_a;
                channel_b->tunerParams.rfFreq.syncUpdate = rf_sync_b;
                atomic_store(&rspduo.sync_waiting[0], 0);
                atomic_store(&rspduo.sync_waiting[1], 0);
                if (result == sdrplay_api_Success) {
                    if (categories_a != 0u)
                        openrsp_sync_update_consume(
                            &rspduo.sync_update[0], categories_a,
                            atomic_load(&rspduo.sample_number));
                    if (categories_b != 0u)
                        openrsp_sync_update_consume(
                            &rspduo.sync_update[1], categories_b,
                            atomic_load(&rspduo.sample_number_b));
                }
                return result;
            }
        }
        sdrplay_api_ErrT result = sdrplay_api_Update(
            dev, sdrplay_api_Tuner_A, reason, extension);
        if (result != sdrplay_api_Success) return result;
        return sdrplay_api_Update(
            dev, sdrplay_api_Tuner_B, reason, extension);
    }
    sdrplay_api_ErrT validation = validate_update(&rspduo, tuner, reason, extension);
    if (validation != sdrplay_api_Success) {
        RECORD_LAST_ERROR(
            3, "sdrplay_api_Update",
            "device update validation failed for tuner %d, reasons 0x%08x "
            "and extension 0x%08x (%s)",
            tuner, reason, extension, sdrplay_api_GetErrorString(validation));
        return validation;
    }
    unsigned int sync_slot = tuner == sdrplay_api_Tuner_B ? 1u : 0u;
    if ((reason & sdrplay_api_Update_Dev_SyncUpdate) != 0u) {
        openrsp_sync_update_schedule(
            &rspduo.sync_update[sync_slot],
            OPENRSP_SYNC_GAIN | OPENRSP_SYNC_RF | OPENRSP_SYNC_FS,
            rspduo.dev_params.syncUpdate.sampleNum,
            rspduo.dev_params.syncUpdate.period);
        reason &= ~sdrplay_api_Update_Dev_SyncUpdate;
    }
    if ((reason & sdrplay_api_Update_Dev_ResetFlags) != 0u) {
        uint32_t reset = 0u;
        if (rspduo.dev_params.resetFlags.resetGainUpdate != 0u)
            reset |= OPENRSP_SYNC_GAIN;
        if (rspduo.dev_params.resetFlags.resetRfUpdate != 0u)
            reset |= OPENRSP_SYNC_RF;
        if (rspduo.dev_params.resetFlags.resetFsUpdate != 0u)
            reset |= OPENRSP_SYNC_FS;
        openrsp_sync_update_reset(&rspduo.sync_update[sync_slot], reset);
        sdrplay_api_RxChannelParamsT *reset_channel =
            channel_for_tuner(&rspduo, tuner);
        if ((reset & OPENRSP_SYNC_GAIN) != 0u)
            reset_channel->tunerParams.gain.syncUpdate = 0u;
        if ((reset & OPENRSP_SYNC_GAIN) != 0u)
            reset_channel->ctrlParams.agc.syncUpdate = 0;
        if ((reset & OPENRSP_SYNC_RF) != 0u)
            reset_channel->tunerParams.rfFreq.syncUpdate = 0u;
        if ((reset & OPENRSP_SYNC_FS) != 0u)
            rspduo.dev_params.fsFreq.syncUpdate = 0u;
        rspduo.dev_params.resetFlags.resetGainUpdate = 0u;
        rspduo.dev_params.resetFlags.resetRfUpdate = 0u;
        rspduo.dev_params.resetFlags.resetFsUpdate = 0u;
        reason &= ~sdrplay_api_Update_Dev_ResetFlags;
    }
    if ((extension & sdrplay_api_Update_RspDuo_ResetSlaveFlags) != 0u) {
        /* The vendor service uses these flags to clear a slave-DLL update
         * latch after an application timeout. OpenRSP does not retain such a
         * latch: failed/timed-out updates are immediately retryable. Consume
         * the one-shot API parameters after the validated master-only call. */
        sdrplay_api_RxChannelParamsT *reset_channel =
            channel_for_tuner(&rspduo, tuner);
        reset_channel->rspDuoTunerParams.resetSlaveFlags.resetGainUpdate = 0u;
        reset_channel->rspDuoTunerParams.resetSlaveFlags.resetRfUpdate = 0u;
        extension &= ~sdrplay_api_Update_RspDuo_ResetSlaveFlags;
    }
    uint32_t synchronous_categories = 0u;
    const sdrplay_api_RxChannelParamsT *sync_channel =
        channel_for_tuner_const(&rspduo, tuner);
    if ((reason & sdrplay_api_Update_Tuner_Gr) != 0u &&
        sync_channel->tunerParams.gain.syncUpdate != 0u)
        synchronous_categories |= OPENRSP_SYNC_GAIN;
    if ((reason & sdrplay_api_Update_Ctrl_Agc) != 0u &&
        sync_channel->ctrlParams.agc.syncUpdate != 0)
        synchronous_categories |= OPENRSP_SYNC_GAIN;
    if ((reason & sdrplay_api_Update_Tuner_Frf) != 0u &&
        sync_channel->tunerParams.rfFreq.syncUpdate != 0u)
        synchronous_categories |= OPENRSP_SYNC_RF;
    if ((reason & sdrplay_api_Update_Dev_Fs) != 0u &&
        rspduo.dev_params.fsFreq.syncUpdate != 0u)
        synchronous_categories |= OPENRSP_SYNC_FS;
    if (synchronous_categories != 0u) {
        atomic_store(&rspduo.sync_waiting[sync_slot], 1);
        int wait_result = wait_for_sync_boundary(&rspduo, tuner,
                                                 synchronous_categories);
        atomic_store(&rspduo.sync_waiting[sync_slot], 0);
        if (wait_result != 0) {
            RECORD_LAST_ERROR(3, "sdrplay_api_Update",
                              "synchronous update boundary was not reached");
            return sdrplay_api_ServiceNotResponding;
        }
    }
    if (reason == sdrplay_api_Update_None) return sdrplay_api_Success;
    int result = 0;
    /* The vendor API reports a PPM update through fsChanged, even though the
     * correction also requires retuning the synthesizer in our backend. */
    int fs_changed = (reason & (sdrplay_api_Update_Dev_Fs |
                                sdrplay_api_Update_Dev_Ppm)) != 0u;
    int rf_changed = (reason & sdrplay_api_Update_Tuner_Frf) != 0u;
    int gr_changed = (reason & (sdrplay_api_Update_Tuner_Gr |
                                sdrplay_api_Update_Tuner_GrLimits)) != 0u || rf_changed;
    uint32_t change_flags = protocol_change_flags(reason);
    atomic_int *agc_mode_state = tuner == sdrplay_api_Tuner_B ?
        &rspduo.agc_mode_b : &rspduo.agc_mode;
    atomic_int *agc_setpoint_state = tuner == sdrplay_api_Tuner_B ?
        &rspduo.agc_setpoint_b : &rspduo.agc_setpoint;
    atomic_uint *agc_peak_state = tuner == sdrplay_api_Tuner_B ?
        &rspduo.agc_peak_b : &rspduo.agc_peak;
    int previous_agc_mode = atomic_load(agc_mode_state);
    int previous_agc_setpoint = atomic_load(agc_setpoint_state);
    const uint32_t dsp_reasons =
        sdrplay_api_Update_Ctrl_DCoffsetIQimbalance |
        sdrplay_api_Update_Tuner_LoMode |
        sdrplay_api_Update_Ctrl_AdsbMode |
        sdrplay_api_Update_Ctrl_Decimation |
        sdrplay_api_Update_Dev_Fs |
        sdrplay_api_Update_Tuner_IfType;
    const int dsp_changes = (reason & dsp_reasons) != 0u;
    unsigned int dsp_slot = tuner == sdrplay_api_Tuner_B ? 1u : 0u;
    openrsp_iq_correction saved_iq;
    openrsp_adsb_filter saved_adsb;
    openrsp_low_if_dsp saved_low_if;
    compat_decimator saved_decimator;
    unsigned int saved_callback_samples = atomic_load(&rspduo.callback_samples);
    if (dsp_changes) {
        (void)pthread_mutex_lock(&decimation_lock);
        saved_iq = rspduo.iq_correction[dsp_slot];
        saved_adsb = rspduo.adsb_filter[dsp_slot];
        saved_low_if = tuner == sdrplay_api_Tuner_B ?
            rspduo.low_if_dsp_b : rspduo.low_if_dsp;
        memcpy(&saved_decimator, &rspduo.decimator[dsp_slot],
               sizeof(saved_decimator));
        (void)pthread_mutex_unlock(&decimation_lock);
    }
    pthread_mutex_lock(&hardware_lock);
    sdrplay_api_RxChannelParamsT *channel = channel_for_tuner(&rspduo, tuner);
    if (fs_changed) {
        atomic_store(&rspduo.callback_samples,
                     rspduo.dev_params.fsFreq.fsHz > 9216000.0 ? 2016u : 1344u);
    }
    if ((reason & sdrplay_api_Update_Tuner_BwType) != 0u) {
    }
    if ((reason & sdrplay_api_Update_Tuner_IfType) != 0u) {
    }
    if ((reason & sdrplay_api_Update_Ctrl_Agc) != 0u) {
        sdrplay_api_AgcControlT mode = channel->ctrlParams.agc.enable;
        if (mode < sdrplay_api_AGC_DISABLE || mode > sdrplay_api_AGC_CTRL_EN ||
            channel->ctrlParams.agc.setPoint_dBfs <
                agc_minimum_setpoint(rspduo.dev_params.fsFreq.fsHz) ||
            channel->ctrlParams.agc.setPoint_dBfs >
                agc_maximum_setpoint(&channel->tunerParams.gain)) {
            result = -1;
        } else {
            atomic_store(agc_peak_state, 0u);
            atomic_store(agc_mode_state, mode);
            atomic_store(agc_setpoint_state,
                         channel->ctrlParams.agc.setPoint_dBfs);
            rspduo.agc_last_adjust_ms[dsp_slot] = monotonic_milliseconds();
            rspduo.agc_decay_since_ms[dsp_slot] = 0u;
        }
    }
    if (result >= 0 &&
        (reason & (sdrplay_api_Update_Ctrl_DCoffsetIQimbalance |
                   sdrplay_api_Update_Tuner_LoMode |
                   sdrplay_api_Update_Dev_Fs)) != 0u) {
        (void)pthread_mutex_lock(&decimation_lock);
        result = configure_iq_correction(&rspduo, tuner);
        (void)pthread_mutex_unlock(&decimation_lock);
    }
    if (result >= 0 &&
        (reason & (sdrplay_api_Update_Ctrl_AdsbMode |
                   sdrplay_api_Update_Dev_Fs)) != 0u) {
        (void)pthread_mutex_lock(&decimation_lock);
        result = openrsp_adsb_filter_configure(
            &rspduo.adsb_filter[tuner == sdrplay_api_Tuner_B ? 1u : 0u],
            rspduo.dev_params.fsFreq.fsHz,
            (openrsp_adsb_mode)channel->ctrlParams.adsbMode);
        (void)pthread_mutex_unlock(&decimation_lock);
    }
    if ((reason & sdrplay_api_Update_Ctrl_OverloadMsgAck) != 0u) {
        atomic_int *pending = tuner == sdrplay_api_Tuner_B ?
            &rspduo.overload_event_pending_b : &rspduo.overload_event_pending;
        atomic_int *state_value = tuner == sdrplay_api_Tuner_B ?
            &rspduo.overload_state_b : &rspduo.overload_state;
        atomic_int *reported = tuner == sdrplay_api_Tuner_B ?
            &rspduo.overload_reported_state_b : &rspduo.overload_reported_state;
        atomic_store(pending, 0);
        int state = atomic_load(state_value);
        if (state != atomic_load(reported))
            emit_overload_event(&rspduo, tuner, state);
    }
    /* Install the software-only state before a daemon response can be followed
     * by IQ. The update round trip provides an ordering boundary for callers. */
    if (result >= 0 && (reason & sdrplay_api_Update_Ctrl_Decimation) != 0u) {
        const sdrplay_api_DecimationT *decimation = &channel->ctrlParams.decimation;
        unsigned int factor = decimation->enable != 0u ? decimation->decimationFactor : 1u;
        if (configure_decimator(&rspduo, tuner, factor) != 0) result = -1;
    }
    if (result >= 0 &&
        (reason & (sdrplay_api_Update_Dev_Fs |
                   sdrplay_api_Update_Tuner_IfType)) != 0u) {
        (void)pthread_mutex_lock(&decimation_lock);
        result = openrsp_low_if_configure(
            tuner == sdrplay_api_Tuner_B ? &rspduo.low_if_dsp_b : &rspduo.low_if_dsp,
            rspduo.dev_params.fsFreq.fsHz,
            (int)channel->tunerParams.ifType * 1000);
        (void)pthread_mutex_unlock(&decimation_lock);
    }
    if (result >= 0) {
        openrsp_radio_config config;
        fill_radio_config_tuner(&rspduo, tuner, &config);
        result = rspduo.duo_role != 0u ?
            openrsp_daemon_backend_update_duo(
                rspduo.backend, &config, change_flags) :
            openrsp_daemon_backend_update(
                rspduo.backend, &config, change_flags);
    }
    if (result >= 0) {
        if (gr_changed || rf_changed ||
            (reason & sdrplay_api_Update_RspDuo_AmPortSelect) != 0u) {
            sdrplay_api_GainT *gain = &channel->tunerParams.gain;
            if (openrsp_rspduo_gain_values(
                    channel->tunerParams.rfFreq.rfHz, gain->LNAstate,
                    gain->gRdB, (int)gain->minGr,
                    tuner == sdrplay_api_Tuner_A &&
                        channel->rspDuoTunerParams.tuner1AmPortSel ==
                            sdrplay_api_RspDuo_AMPORT_1,
                    &gain->gainVals.curr, &gain->gainVals.max,
                    &gain->gainVals.min) != 0)
                result = -1;
        }
        if (result > 0) {
            emit_update_ack(&rspduo, fs_changed, rf_changed, gr_changed, tuner);
        } else {
            atomic_uint *pending_fs = tuner == sdrplay_api_Tuner_B ?
                &rspduo.pending_fs_changed_b : &rspduo.pending_fs_changed;
            atomic_uint *pending_rf = tuner == sdrplay_api_Tuner_B ?
                &rspduo.pending_rf_changed_b : &rspduo.pending_rf_changed;
            atomic_uint *pending_gr = tuner == sdrplay_api_Tuner_B ?
                &rspduo.pending_gr_changed_b : &rspduo.pending_gr_changed;
            if (fs_changed) atomic_fetch_add(pending_fs, 1u);
            if (rf_changed) atomic_fetch_add(pending_rf, 1u);
            if (gr_changed) atomic_fetch_add(pending_gr, 1u);
        }
    }
    if (result < 0 && (reason & sdrplay_api_Update_Ctrl_Agc) != 0u) {
        atomic_store(agc_mode_state, previous_agc_mode);
        atomic_store(agc_setpoint_state, previous_agc_setpoint);
        rspduo.agc_last_adjust_ms[dsp_slot] = 0u;
        rspduo.agc_decay_since_ms[dsp_slot] = 0u;
    }
    if (result < 0 && dsp_changes) {
        (void)pthread_mutex_lock(&decimation_lock);
        rspduo.iq_correction[dsp_slot] = saved_iq;
        rspduo.adsb_filter[dsp_slot] = saved_adsb;
        if (tuner == sdrplay_api_Tuner_B)
            rspduo.low_if_dsp_b = saved_low_if;
        else
            rspduo.low_if_dsp = saved_low_if;
        memcpy(&rspduo.decimator[dsp_slot], &saved_decimator,
               sizeof(saved_decimator));
        atomic_store(&rspduo.callback_samples, saved_callback_samples);
        (void)pthread_mutex_unlock(&decimation_lock);
    }
    pthread_mutex_unlock(&hardware_lock);
    if (result < 0) {
        sdrplay_api_ErrT error = update_failure_code(reason);
        RECORD_LAST_ERROR(
            3, "sdrplay_api_Update",
            "OpenRSP daemon rejected update reasons 0x%08x (backend %d)",
            reason, result);
        return error;
    }
    if (synchronous_categories != 0u)
        openrsp_sync_update_consume(&rspduo.sync_update[sync_slot],
                                    synchronous_categories,
                                    atomic_load(sync_slot != 0u ?
                                                &rspduo.sample_number_b :
                                                &rspduo.sample_number));
    return sdrplay_api_Success;
}

sdrplay_api_ErrT sdrplay_api_SwapRspDuoActiveTuner(
    HANDLE dev, sdrplay_api_TunerSelectT *current_tuner,
    sdrplay_api_RspDuo_AmPortSelectT tuner1_am_port)
{
    if (dev != &rspduo || current_tuner == NULL) return sdrplay_api_InvalidParam;
    if (!rspduo.initialized || rspduo.mode != sdrplay_api_RspDuoMode_Single_Tuner ||
        (*current_tuner != sdrplay_api_Tuner_A &&
         *current_tuner != sdrplay_api_Tuner_B) || *current_tuner != rspduo.tuner)
        return sdrplay_api_InvalidMode;
    if (tuner1_am_port != sdrplay_api_RspDuo_AMPORT_1 &&
        tuner1_am_port != sdrplay_api_RspDuo_AMPORT_2)
        return sdrplay_api_InvalidParam;
    sdrplay_api_TunerSelectT next = rspduo.tuner == sdrplay_api_Tuner_A ?
        sdrplay_api_Tuner_B : sdrplay_api_Tuner_A;
    sdrplay_api_RxChannelParamsT saved = *channel_for_tuner(&rspduo, next);
    *channel_for_tuner(&rspduo, next) = *channel_for_tuner(&rspduo, rspduo.tuner);
    /* Tuner A has no bias-tee control; API 3.15.1 returns InvalidParam for
     * that update, so never carry tuner B's enabled bit onto tuner A. */
    if (next == sdrplay_api_Tuner_A)
        channel_for_tuner(&rspduo, next)->rspDuoTunerParams.biasTEnable = 0u;
    if (validate_dsp_configuration(
            channel_for_tuner(&rspduo, next),
            rspduo.dev_params.fsFreq.fsHz) != 0) {
        *channel_for_tuner(&rspduo, next) = saved;
        return sdrplay_api_OutOfRange;
    }
    openrsp_swap_request swap = {
        .tuner = next == sdrplay_api_Tuner_B ? OPENRSP_TUNER_B : OPENRSP_TUNER_A
    };
    fill_radio_config_tuner(&rspduo, next, &swap.config);
    pause_stream_callbacks(&rspduo);
    pthread_mutex_lock(&hardware_lock);
    int result = openrsp_daemon_backend_swap(rspduo.backend, &swap);
    if (result >= 0) {
        pause_stream_callbacks(&rspduo);
        sdrplay_api_RxChannelParamsT *next_channel =
            channel_for_tuner(&rspduo, next);
        unsigned int factor = next_channel->ctrlParams.decimation.enable != 0u ?
            next_channel->ctrlParams.decimation.decimationFactor : 1u;
        (void)pthread_mutex_lock(&decimation_lock);
        int dsp_result = configure_iq_correction(&rspduo, next);
        if (dsp_result == 0)
            dsp_result = openrsp_adsb_filter_configure(
                &rspduo.adsb_filter[next == sdrplay_api_Tuner_B ? 1u : 0u],
                rspduo.dev_params.fsFreq.fsHz,
                (openrsp_adsb_mode)next_channel->ctrlParams.adsbMode);
        if (dsp_result == 0)
            dsp_result = openrsp_low_if_configure(
                next == sdrplay_api_Tuner_B ? &rspduo.low_if_dsp_b :
                                              &rspduo.low_if_dsp,
                rspduo.dev_params.fsFreq.fsHz,
                (int)next_channel->tunerParams.ifType * 1000);
        (void)pthread_mutex_unlock(&decimation_lock);
        if (dsp_result != 0 || configure_decimator(&rspduo, next, factor) != 0) {
            atomic_store(&rspduo.stream_state, -1);
            pthread_mutex_unlock(&hardware_lock);
            resume_stream_callbacks(&rspduo);
            return sdrplay_api_HwError;
        }
        rspduo.tuner = next;
        *current_tuner = next;
        rspduo.params.rxChannelA = next == sdrplay_api_Tuner_A ? &rspduo.channel_a : NULL;
        rspduo.params.rxChannelB = next == sdrplay_api_Tuner_B ? &rspduo.channel_b : NULL;
        (void)pthread_mutex_lock(&rspduo.stream_callback_lock);
        rspduo.stream_callback_head = 0u;
        rspduo.stream_callback_count = 0u;
        if (next == sdrplay_api_Tuner_B)
            rspduo.stream_callback_seen_b = 0;
        else
            rspduo.stream_callback_seen = 0;
        (void)pthread_mutex_unlock(&rspduo.stream_callback_lock);
        if (next == sdrplay_api_Tuner_B) {
            rspduo.first_callback_b = 1u;
            rspduo.iq_sequence_seen_b = 0;
            atomic_store(&rspduo.sample_number_b, 0u);
        } else {
            rspduo.first_callback = 1u;
            rspduo.iq_sequence_seen = 0;
            atomic_store(&rspduo.sample_number, 0u);
        }
        /* Arm the local callback consumer before asking the daemon to resume.
         * The daemon may send the first IQ frame immediately after its
         * response; resuming locally afterward races that frame and can drop
         * the only reset-marked callback at the new tuner epoch. */
        resume_stream_callbacks(&rspduo);
        if (openrsp_daemon_backend_resume_mode(rspduo.backend) < 0) {
            atomic_store(&rspduo.stream_state, -1);
            pthread_mutex_unlock(&hardware_lock);
            return sdrplay_api_HwError;
        }
    } else {
        *channel_for_tuner(&rspduo, next) = saved;
    }
    pthread_mutex_unlock(&hardware_lock);
    if (result < 0) resume_stream_callbacks(&rspduo);
    return result >= 0 ? sdrplay_api_Success : sdrplay_api_HwError;
}

sdrplay_api_ErrT sdrplay_api_SwapRspDuoDualTunerModeSampleRate(
    HANDLE dev, double *current_sample_rate, double new_sample_rate)
{
    if (dev != &rspduo || current_sample_rate == NULL) return sdrplay_api_InvalidParam;
    if (!rspduo.initialized) return sdrplay_api_InvalidMode;
    if (rspduo.duo_role != OPENRSP_DUO_ROLE_MASTER ||
        rspduo.mode != sdrplay_api_RspDuoMode_Master)
        return sdrplay_api_InvalidParam;
    if (!isfinite(new_sample_rate) ||
        (new_sample_rate != 6000000.0 && new_sample_rate != 8000000.0))
        return sdrplay_api_OutOfRange;

    double old_sample_rate = rspduo.dev_params.fsFreq.fsHz;
    if (new_sample_rate == old_sample_rate) {
        *current_sample_rate = old_sample_rate;
        return sdrplay_api_Success;
    }
    sdrplay_api_If_kHzT old_if = rspduo.channel_a.tunerParams.ifType;
    sdrplay_api_If_kHzT new_if = new_sample_rate == 6000000.0 ?
        sdrplay_api_IF_1_620 : sdrplay_api_IF_2_048;
    rspduo.dev_params.fsFreq.fsHz = new_sample_rate;
    rspduo.channel_a.tunerParams.ifType = new_if;
    if (validate_dsp_configuration(&rspduo.channel_a, new_sample_rate) != 0) {
        rspduo.dev_params.fsFreq.fsHz = old_sample_rate;
        rspduo.channel_a.tunerParams.ifType = old_if;
        return sdrplay_api_OutOfRange;
    }

    pause_stream_callbacks(&rspduo);
    (void)pthread_mutex_lock(&hardware_lock);
    openrsp_iq_correction saved_iq;
    openrsp_adsb_filter saved_adsb;
    openrsp_low_if_dsp saved_low_if;
    compat_decimator saved_decimator;
    (void)pthread_mutex_lock(&decimation_lock);
    saved_iq = rspduo.iq_correction[0];
    saved_adsb = rspduo.adsb_filter[0];
    saved_low_if = rspduo.low_if_dsp;
    memcpy(&saved_decimator, &rspduo.decimator[0], sizeof(saved_decimator));
    int dsp_result = configure_iq_correction(&rspduo, sdrplay_api_Tuner_A);
    if (dsp_result == 0)
        dsp_result = openrsp_adsb_filter_configure(
            &rspduo.adsb_filter[0], new_sample_rate,
            (openrsp_adsb_mode)rspduo.channel_a.ctrlParams.adsbMode);
    if (dsp_result == 0)
        dsp_result = openrsp_low_if_configure(
            &rspduo.low_if_dsp, new_sample_rate, (int)new_if * 1000);
    (void)pthread_mutex_unlock(&decimation_lock);
    unsigned int factor = rspduo.channel_a.ctrlParams.decimation.enable != 0u ?
        rspduo.channel_a.ctrlParams.decimation.decimationFactor : 1u;
    if (dsp_result == 0)
        dsp_result = configure_decimator(&rspduo, sdrplay_api_Tuner_A, factor);
    int result = dsp_result == 0 ? openrsp_daemon_backend_swap_duo_rate(
        rspduo.backend, (uint32_t)new_sample_rate) : -1;
    if (result != 0) {
        rspduo.dev_params.fsFreq.fsHz = old_sample_rate;
        rspduo.channel_a.tunerParams.ifType = old_if;
        (void)pthread_mutex_lock(&decimation_lock);
        rspduo.iq_correction[0] = saved_iq;
        rspduo.adsb_filter[0] = saved_adsb;
        rspduo.low_if_dsp = saved_low_if;
        memcpy(&rspduo.decimator[0], &saved_decimator,
               sizeof(saved_decimator));
        (void)pthread_mutex_unlock(&decimation_lock);
    } else {
        atomic_store(&rspduo.callback_samples, 1344u);
        rspduo.first_callback = 1u;
        rspduo.iq_sequence_seen = 0;
        atomic_store(&rspduo.sample_number, 0u);
        *current_sample_rate = new_sample_rate;
    }
    (void)pthread_mutex_unlock(&hardware_lock);
    resume_stream_callbacks(&rspduo);
    if (result == -(int)OPENRSP_STATUS_STOP_PENDING)
        return sdrplay_api_StopPending;
    return result == 0 ? sdrplay_api_Success : sdrplay_api_HwError;
}

sdrplay_api_ErrT sdrplay_api_SwapRspDuoMode(
    sdrplay_api_DeviceT *current_device, sdrplay_api_DeviceParamsT **device_params,
    sdrplay_api_RspDuoModeT mode, double sample_rate, sdrplay_api_TunerSelectT tuner,
    sdrplay_api_Bw_MHzT bandwidth, sdrplay_api_If_kHzT if_type,
    sdrplay_api_RspDuo_AmPortSelectT tuner1_am_port)
{
    if (current_device == NULL || device_params == NULL) return sdrplay_api_InvalidParam;
    if (current_device->dev != &rspduo || !rspduo.initialized)
        return sdrplay_api_InvalidMode;
    if (mode != sdrplay_api_RspDuoMode_Single_Tuner &&
        mode != sdrplay_api_RspDuoMode_Dual_Tuner)
        return sdrplay_api_InvalidParam;
    if (mode == rspduo.mode) return sdrplay_api_InvalidMode;
    if (!isfinite(sample_rate) || !valid_bandwidth(bandwidth) ||
        !valid_if(if_type) ||
        (tuner1_am_port != sdrplay_api_RspDuo_AMPORT_1 &&
         tuner1_am_port != sdrplay_api_RspDuo_AMPORT_2))
        return sdrplay_api_OutOfRange;
    if (mode == sdrplay_api_RspDuoMode_Dual_Tuner) {
        sdrplay_api_If_kHzT required_if = sample_rate == 6000000.0 ?
            sdrplay_api_IF_1_620 : sample_rate == 8000000.0 ?
            sdrplay_api_IF_2_048 : sdrplay_api_IF_Undefined;
        if (tuner != sdrplay_api_Tuner_Both || required_if == sdrplay_api_IF_Undefined ||
            if_type != required_if || bandwidth > sdrplay_api_BW_1_536 ||
            rspduo.callbacks.StreamBCbFn == NULL)
            return sdrplay_api_OutOfRange;
    } else if ((tuner != sdrplay_api_Tuner_A && tuner != sdrplay_api_Tuner_B) ||
               sample_rate < 2000000.0 || sample_rate > 10660000.0 ||
               (if_type != sdrplay_api_IF_Zero && bandwidth > sdrplay_api_BW_1_536)) {
        return sdrplay_api_OutOfRange;
    }

    sdrplay_api_DevParamsT saved_dev = rspduo.dev_params;
    sdrplay_api_RxChannelParamsT saved_a = rspduo.channel_a;
    sdrplay_api_RxChannelParamsT saved_b = rspduo.channel_b;
    rspduo.dev_params.fsFreq.fsHz = sample_rate;
    sdrplay_api_RxChannelParamsT *target =
        tuner == sdrplay_api_Tuner_B ? &rspduo.channel_b : &rspduo.channel_a;
    if (mode == sdrplay_api_RspDuoMode_Dual_Tuner) {
        rspduo.channel_a.tunerParams.bwType = bandwidth;
        rspduo.channel_b.tunerParams.bwType = bandwidth;
        rspduo.channel_a.tunerParams.ifType = if_type;
        rspduo.channel_b.tunerParams.ifType = if_type;
        rspduo.channel_a.rspDuoTunerParams.tuner1AmPortSel = tuner1_am_port;
    } else {
        target->tunerParams.bwType = bandwidth;
        target->tunerParams.ifType = if_type;
        if (tuner == sdrplay_api_Tuner_A)
            target->rspDuoTunerParams.tuner1AmPortSel = tuner1_am_port;
    }

    /* Reject DSP combinations before changing hardware.  In particular, an
     * ADS-B filter that was valid at a higher sample rate may not fit below
     * the new Nyquist frequency after a mode transition. */
    sdrplay_api_RxChannelParamsT *stream_a_validation =
        mode == sdrplay_api_RspDuoMode_Dual_Tuner ? &rspduo.channel_a : target;
    if (validate_dsp_configuration(stream_a_validation, sample_rate) != 0 ||
        (mode == sdrplay_api_RspDuoMode_Dual_Tuner &&
         validate_dsp_configuration(&rspduo.channel_b, sample_rate) != 0)) {
        rspduo.dev_params = saved_dev;
        rspduo.channel_a = saved_a;
        rspduo.channel_b = saved_b;
        return sdrplay_api_OutOfRange;
    }

    openrsp_mode_swap_request swap = {
        .mode = mode == sdrplay_api_RspDuoMode_Dual_Tuner ?
                OPENRSP_MODE_DUAL : OPENRSP_MODE_SINGLE
    };
    if (swap.mode == OPENRSP_MODE_DUAL) {
        swap.dual.sample_rate_hz = (uint32_t)sample_rate;
        fill_radio_config_tuner(&rspduo, sdrplay_api_Tuner_A,
                                &swap.dual.channel_a);
        fill_radio_config_tuner(&rspduo, sdrplay_api_Tuner_B,
                                &swap.dual.channel_b);
    } else {
        fill_radio_config_tuner(&rspduo, tuner, &swap.single);
    }

    pause_stream_callbacks(&rspduo);
    (void)pthread_mutex_lock(&hardware_lock);
    int result = openrsp_daemon_backend_swap_mode(rspduo.backend, &swap);
    if (result < 0) {
        rspduo.dev_params = saved_dev;
        rspduo.channel_a = saved_a;
        rspduo.channel_b = saved_b;
        (void)pthread_mutex_unlock(&hardware_lock);
        resume_stream_callbacks(&rspduo);
        return sdrplay_api_HwError;
    }
    pause_stream_callbacks(&rspduo);

    rspduo.mode = mode;
    rspduo.tuner = tuner;
    rspduo.params.rxChannelA = tuner == sdrplay_api_Tuner_B ? NULL :
                                                               &rspduo.channel_a;
    rspduo.params.rxChannelB = tuner == sdrplay_api_Tuner_A ? NULL :
                                                               &rspduo.channel_b;
    if (tuner == sdrplay_api_Tuner_Both) {
        rspduo.params.rxChannelA = &rspduo.channel_a;
        rspduo.params.rxChannelB = &rspduo.channel_b;
    }
    current_device->rspDuoMode = mode;
    current_device->tuner = tuner;
    current_device->rspDuoSampleFreq = sample_rate;
    *device_params = &rspduo.params;

    sdrplay_api_RxChannelParamsT *stream_a_channel =
        mode == sdrplay_api_RspDuoMode_Dual_Tuner ? &rspduo.channel_a : target;
    openrsp_low_if_dsp *stream_a_dsp =
        mode == sdrplay_api_RspDuoMode_Single_Tuner &&
        tuner == sdrplay_api_Tuner_B ? &rspduo.low_if_dsp_b : &rspduo.low_if_dsp;
    sdrplay_api_TunerSelectT stream_a_tuner =
        mode == sdrplay_api_RspDuoMode_Single_Tuner ? tuner : sdrplay_api_Tuner_A;
    (void)pthread_mutex_lock(&decimation_lock);
    int dsp_result = configure_iq_correction(&rspduo, stream_a_tuner);
    if (dsp_result == 0)
        dsp_result = openrsp_adsb_filter_configure(
            &rspduo.adsb_filter[stream_a_tuner == sdrplay_api_Tuner_B ? 1u : 0u],
            sample_rate,
            (openrsp_adsb_mode)stream_a_channel->ctrlParams.adsbMode);
    if (dsp_result == 0)
        dsp_result = openrsp_low_if_configure(
            stream_a_dsp, sample_rate,
            (int)stream_a_channel->tunerParams.ifType * 1000);
    if (dsp_result == 0 && mode == sdrplay_api_RspDuoMode_Dual_Tuner)
        dsp_result = configure_iq_correction(&rspduo, sdrplay_api_Tuner_B);
    if (dsp_result == 0 && mode == sdrplay_api_RspDuoMode_Dual_Tuner)
        dsp_result = openrsp_adsb_filter_configure(
            &rspduo.adsb_filter[1], sample_rate,
            (openrsp_adsb_mode)rspduo.channel_b.ctrlParams.adsbMode);
    if (dsp_result == 0 && mode == sdrplay_api_RspDuoMode_Dual_Tuner)
        dsp_result = openrsp_low_if_configure(
            &rspduo.low_if_dsp_b, sample_rate,
            (int)rspduo.channel_b.tunerParams.ifType * 1000);
    (void)pthread_mutex_unlock(&decimation_lock);
    unsigned int factor_a = stream_a_channel->ctrlParams.decimation.enable != 0u ?
        stream_a_channel->ctrlParams.decimation.decimationFactor : 1u;
    unsigned int factor_b = rspduo.channel_b.ctrlParams.decimation.enable != 0u ?
        rspduo.channel_b.ctrlParams.decimation.decimationFactor : 1u;
    if (dsp_result != 0 || configure_decimator(&rspduo,
                                                mode == sdrplay_api_RspDuoMode_Single_Tuner ?
                                                tuner : sdrplay_api_Tuner_A,
                                                factor_a) != 0 ||
        (mode == sdrplay_api_RspDuoMode_Dual_Tuner &&
         configure_decimator(&rspduo, sdrplay_api_Tuner_B, factor_b) != 0)) {
        /* All values were validated before the hardware request, so reaching
         * this path indicates an internal state error rather than a caller
         * parameter problem. */
        atomic_store(&rspduo.stream_state, -1);
        (void)pthread_mutex_unlock(&hardware_lock);
        resume_stream_callbacks(&rspduo);
        return sdrplay_api_HwError;
    }
    (void)pthread_mutex_lock(&rspduo.stream_callback_lock);
    rspduo.stream_callback_head = 0u;
    rspduo.stream_callback_count = 0u;
    rspduo.stream_callback_seen = 0;
    rspduo.stream_callback_seen_b = 0;
    (void)pthread_mutex_unlock(&rspduo.stream_callback_lock);
    atomic_store(&rspduo.sample_number, 0u);
    atomic_store(&rspduo.sample_number_b, 0u);
    rspduo.last_iq_sequence = 0u;
    rspduo.last_iq_sequence_b = 0u;
    rspduo.iq_sequence_seen = 0;
    rspduo.iq_sequence_seen_b = 0;
    rspduo.first_callback = 1u;
    rspduo.first_callback_b = 1u;
    atomic_store(&rspduo.callback_samples,
                 sample_rate > 9216000.0 ? 2016u : 1344u);
    atomic_store(tuner == sdrplay_api_Tuner_B &&
                 mode == sdrplay_api_RspDuoMode_Single_Tuner ?
                 &rspduo.agc_mode_b : &rspduo.agc_mode,
                 stream_a_channel->ctrlParams.agc.enable);
    atomic_store(tuner == sdrplay_api_Tuner_B &&
                 mode == sdrplay_api_RspDuoMode_Single_Tuner ?
                 &rspduo.agc_setpoint_b : &rspduo.agc_setpoint,
                 stream_a_channel->ctrlParams.agc.setPoint_dBfs);
    atomic_store(&rspduo.agc_mode_b, rspduo.channel_b.ctrlParams.agc.enable);
    atomic_store(&rspduo.agc_setpoint_b,
                 rspduo.channel_b.ctrlParams.agc.setPoint_dBfs);
    /* See SwapRspDuoActiveTuner: the first frame of the new mode may follow
     * the resume response without any scheduling gap. */
    resume_stream_callbacks(&rspduo);
    if (openrsp_daemon_backend_resume_mode(rspduo.backend) < 0) {
        atomic_store(&rspduo.stream_state, -1);
        (void)pthread_mutex_unlock(&hardware_lock);
        return sdrplay_api_HwError;
    }
    (void)pthread_mutex_unlock(&hardware_lock);
    return sdrplay_api_Success;
}
