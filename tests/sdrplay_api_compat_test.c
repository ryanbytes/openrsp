/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "openrsp/protocol.h"
#include "sdrplay_api_compat.h"

#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int transfer_exact(int descriptor, void *buffer, size_t bytes, int writing)
{
    unsigned char *cursor = buffer;
    while (bytes != 0u) {
        ssize_t count = writing ? write(descriptor, cursor, bytes) : read(descriptor, cursor, bytes);
        if (count <= 0) return -1;
        cursor += (size_t)count;
        bytes -= (size_t)count;
    }
    return 0;
}

static int send_frame(int descriptor, uint16_t type, uint32_t sequence,
                      const void *payload, uint32_t bytes)
{
    openrsp_message_header header = {
        .magic = OPENRSP_PROTOCOL_MAGIC, .version = OPENRSP_PROTOCOL_VERSION,
        .type = type, .sequence = sequence, .payload_bytes = bytes
    };
    return transfer_exact(descriptor, &header, sizeof(header), 1) == 0 &&
           (bytes == 0u || transfer_exact(descriptor, (void *)payload, bytes, 1) == 0) ? 0 : -1;
}

static int send_response(int descriptor, uint32_t sequence, uint32_t status)
{
    const openrsp_response response = {.status = status, .sequence = sequence};
    return send_frame(descriptor, OPENRSP_MSG_RESPONSE, sequence, &response, sizeof(response));
}

static int send_mock_iq(int descriptor, uint32_t sequence)
{
    int16_t iq[2048];
    for (size_t index = 0u; index < sizeof(iq) / sizeof(iq[0]); ++index)
        iq[index] = (int16_t)(index - 1024);
    return send_frame(descriptor, OPENRSP_EVENT_IQ, sequence, iq, sizeof(iq));
}

static int send_overload_iq(int descriptor, uint32_t sequence)
{
    int16_t iq[2048];
    for (size_t sample = 0u; sample < 1024u; ++sample) {
        iq[sample * 2u] = sample % 2u == 0u ? 32767 : -32767;
        iq[sample * 2u + 1u] = 0;
    }
    return send_frame(descriptor, OPENRSP_EVENT_IQ, sequence, iq, sizeof(iq));
}

static int send_stopband_iq(int descriptor, uint32_t sequence)
{
    const double pi = 3.14159265358979323846;
    int16_t iq[2048];
    for (size_t sample = 0u; sample < 1024u; ++sample) {
        double phase = 2.0 * pi * 0.375 * (double)sample;
        iq[sample * 2u] = (int16_t)lround(12000.0 * sin(phase));
        iq[sample * 2u + 1u] = (int16_t)lround(12000.0 * cos(phase));
    }
    return send_frame(descriptor, OPENRSP_EVENT_IQ, sequence, iq, sizeof(iq));
}

static int send_passband_iq(int descriptor, uint32_t sequence)
{
    const double pi = 3.14159265358979323846;
    int16_t iq[2048];
    for (size_t sample = 0u; sample < 1024u; ++sample) {
        double phase = 2.0 * pi * 0.0625 * (double)sample;
        iq[sample * 2u] = (int16_t)lround(12000.0 * sin(phase));
        iq[sample * 2u + 1u] = (int16_t)lround(12000.0 * cos(phase));
    }
    return send_frame(descriptor, OPENRSP_EVENT_IQ, sequence, iq, sizeof(iq));
}

static int serve_client(int descriptor)
{
    int streaming = 0;
    unsigned int streaming_updates = 0u;
    uint32_t iq_sequence = 0u;
    for (;;) {
        openrsp_message_header request;
        unsigned char payload[4096];
        if (transfer_exact(descriptor, &request, sizeof(request), 0) != 0 ||
            request.magic != OPENRSP_PROTOCOL_MAGIC ||
            request.version != OPENRSP_PROTOCOL_VERSION || request.payload_bytes > sizeof(payload) ||
            (request.payload_bytes != 0u &&
             transfer_exact(descriptor, payload, request.payload_bytes, 0) != 0))
            return 0;
        if (request.type == OPENRSP_CMD_LIST) {
            const openrsp_response response = {
                .status = OPENRSP_STATUS_OK, .sequence = request.sequence, .changed_flags = 4u
            };
            const openrsp_device_record devices[] = {
                {.device_index = 13u, .vendor_id = 0x1df7u, .product_id = 0x3020u,
                 .serial = "MOCK-RSPDUO", .model = "SDRplay RSPduo"},
                {.device_index = 7u, .vendor_id = 0x1df7u, .product_id = 0x2500u,
                 .serial = "MOCK-RSP1", .model = "SDRplay RSP1"},
                {.device_index = 9u, .vendor_id = 0x1df7u, .product_id = 0x3000u,
                 .serial = "MOCK-RSP1A", .model = "SDRplay RSP1A"},
                {.device_index = 11u, .vendor_id = 0x1df7u, .product_id = 0x3010u,
                 .serial = "MOCK-RSP2", .model = "SDRplay RSP2"}
            };
            if (send_frame(descriptor, OPENRSP_MSG_RESPONSE, request.sequence,
                           &response, sizeof(response)) != 0) return -1;
            for (size_t index = 0u;
                 index < sizeof(devices) / sizeof(devices[0]); ++index) {
                if (send_frame(descriptor, OPENRSP_EVENT_DEVICE, request.sequence,
                               &devices[index], sizeof(devices[index])) != 0)
                    return -1;
            }
        } else if (request.type == OPENRSP_CMD_ACQUIRE) {
            const openrsp_acquire_request *acquire =
                (const openrsp_acquire_request *)payload;
            uint32_t status = request.payload_bytes == sizeof(*acquire) &&
                              acquire->device_index == 13u &&
                              acquire->vendor_id == 0x1df7u &&
                              acquire->product_id == 0x3020u &&
                              strcmp(acquire->serial, "MOCK-RSPDUO") == 0 ?
                              OPENRSP_STATUS_OK : OPENRSP_STATUS_BAD_REQUEST;
            if (send_response(descriptor, request.sequence, status) != 0) return -1;
        } else if (request.type == OPENRSP_CMD_UPDATE && streaming) {
            const openrsp_update_request *update = (const openrsp_update_request *)payload;
            ++streaming_updates;
            const int reject_update =
                update->config.center_frequency_hz == 123456789u;
            const openrsp_response response = {
                .status = reject_update ? OPENRSP_STATUS_IO_ERROR : OPENRSP_STATUS_OK,
                .sequence = request.sequence,
                .changed_flags = OPENRSP_RESPONSE_RECOVERY_QUEUED
            };
            if (reject_update) {
                if (send_frame(descriptor, OPENRSP_MSG_RESPONSE, request.sequence,
                               &response, sizeof(response)) != 0) return -1;
                continue;
            }
            /* Put IQ before the response so each update has a deterministic
             * fixture boundary while also exercising response waiting with
             * an interleaved stream frame. */
            if (update->changed_flags == OPENRSP_CHANGE_RF &&
                update->config.center_frequency_hz >= 100999000u &&
                update->config.center_frequency_hz < 101000000u)
                iq_sequence += 2u;
            int iq_result = 0;
            if ((update->changed_flags & OPENRSP_CHANGE_RF) != 0u &&
                update->config.center_frequency_hz == 102000000u) {
                iq_result = send_overload_iq(descriptor, ++iq_sequence);
            } else if (streaming_updates == 4u) {
                for (unsigned int frame = 0u; frame < 8u && iq_result == 0; ++frame)
                    iq_result = send_passband_iq(descriptor, ++iq_sequence);
                for (unsigned int frame = 0u; frame < 8u && iq_result == 0; ++frame)
                    iq_result = send_stopband_iq(descriptor, ++iq_sequence);
            } else {
                iq_result = send_mock_iq(descriptor, ++iq_sequence);
            }
            if (iq_result != 0 ||
                send_frame(descriptor, OPENRSP_MSG_RESPONSE, request.sequence,
                           &response, sizeof(response)) != 0) return -1;
        } else {
            uint32_t status = request.type == OPENRSP_CMD_UPDATE && !streaming ?
                              OPENRSP_STATUS_BAD_REQUEST : OPENRSP_STATUS_OK;
            if (send_response(descriptor, request.sequence, status) != 0) return -1;
            if (request.type == OPENRSP_CMD_START) {
                streaming = 1;
                if (send_mock_iq(descriptor, ++iq_sequence) != 0)
                    return -1;
            } else if (request.type == OPENRSP_CMD_STOP ||
                       request.type == OPENRSP_CMD_RELEASE) {
                streaming = 0;
            }
        }
    }
}

static int mock_daemon(const char *socket_path)
{
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0) return 1;
    struct sockaddr_un address = {0};
    address.sun_family = AF_UNIX;
    (void)strcpy(address.sun_path, socket_path);
    (void)unlink(socket_path);
    if (bind(server, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        chmod(socket_path, 0600) != 0 || listen(server, 4) != 0) return 2;
    for (;;) {
        int client = accept(server, NULL, NULL);
        if (client < 0) break;
        (void)serve_client(client);
        (void)close(client);
    }
    (void)close(server);
    (void)unlink(socket_path);
    return 0;
}

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    unsigned int callbacks;
    unsigned int samples;
    int rf_changed;
    int gain_changed;
    int validate_samples;
    unsigned int device_failures;
    unsigned int reset_callbacks;
    unsigned int last_reset_first_sample;
    unsigned int last_peak;
    int measure_filter;
    unsigned int filter_frames;
    unsigned int passband_peak;
    unsigned int stopband_peak;
    unsigned int gain_events;
    unsigned int event_gr_db;
    double event_current_gain;
    unsigned int overload_events;
    sdrplay_api_PowerOverloadCbEventIdT last_overload_event;
    HANDLE device_handle;
    int auto_ack_overload;
    sdrplay_api_ErrT overload_ack_result;
    int validate_pointer_reuse;
    short *stream_i_pointer;
    short *stream_q_pointer;
} callback_metrics;

typedef struct {
    atomic_int acquired;
    sdrplay_api_ErrT lock_result;
    sdrplay_api_ErrT unlock_result;
} api_lock_metrics;

static void *api_lock_thread(void *opaque)
{
    api_lock_metrics *metrics = opaque;
    metrics->lock_result = sdrplay_api_LockDeviceApi();
    if (metrics->lock_result == sdrplay_api_Success) {
        atomic_store(&metrics->acquired, 1);
        metrics->unlock_result = sdrplay_api_UnlockDeviceApi();
    }
    return NULL;
}

static void stream_callback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params,
                            unsigned int samples, unsigned int reset, void *opaque)
{
    callback_metrics *metrics = opaque;
    assert(samples == params->numSamples);
    (void)pthread_mutex_lock(&metrics->lock);
    if (samples != 0u && metrics->validate_pointer_reuse) {
        if (metrics->stream_i_pointer == NULL) {
            metrics->stream_i_pointer = xi;
            metrics->stream_q_pointer = xq;
        } else {
            assert(metrics->stream_i_pointer == xi);
            assert(metrics->stream_q_pointer == xq);
        }
    }
    if (samples != 0u && metrics->validate_samples)
        assert(xi[0] == -1024 && xq[0] == -1023);
    ++metrics->callbacks;
    metrics->samples += samples;
    metrics->rf_changed += params->rfChanged;
    metrics->gain_changed += params->grChanged;
    if (reset != 0u) {
        ++metrics->reset_callbacks;
        metrics->last_reset_first_sample = params->firstSampleNum;
    }
    unsigned int peak = 0u;
    for (unsigned int index = 0u; index < samples; ++index) {
        unsigned int abs_i = (unsigned int)(xi[index] < 0 ? -(int)xi[index] : xi[index]);
        unsigned int abs_q = (unsigned int)(xq[index] < 0 ? -(int)xq[index] : xq[index]);
        if (abs_i > peak) peak = abs_i;
        if (abs_q > peak) peak = abs_q;
    }
    metrics->last_peak = peak;
    if (metrics->measure_filter && samples != 0u) {
        ++metrics->filter_frames;
        if (metrics->filter_frames <= 8u)
            metrics->passband_peak = peak;
        else
            metrics->stopband_peak = peak;
    }
    (void)pthread_cond_signal(&metrics->ready);
    (void)pthread_mutex_unlock(&metrics->lock);
}

static void event_callback(sdrplay_api_EventT event, sdrplay_api_TunerSelectT tuner,
                           sdrplay_api_EventParamsT *params, void *opaque)
{
    callback_metrics *metrics = opaque;
    assert(tuner == sdrplay_api_Tuner_A);
    assert(params != NULL);
    int acknowledge_overload = 0;
    HANDLE device_handle = NULL;
    (void)pthread_mutex_lock(&metrics->lock);
    if (event == sdrplay_api_DeviceFailure) {
        ++metrics->device_failures;
    } else if (event == sdrplay_api_GainChange) {
        ++metrics->gain_events;
        metrics->event_gr_db = params->gainParams.gRdB;
        metrics->event_current_gain = params->gainParams.currGain;
    } else if (event == sdrplay_api_PowerOverloadChange) {
        ++metrics->overload_events;
        metrics->last_overload_event =
            params->powerOverloadParams.powerOverloadChangeType;
        if (metrics->auto_ack_overload &&
            metrics->last_overload_event == sdrplay_api_Overload_Detected) {
            acknowledge_overload = 1;
            device_handle = metrics->device_handle;
        }
    }
    (void)pthread_cond_signal(&metrics->ready);
    (void)pthread_mutex_unlock(&metrics->lock);
    if (acknowledge_overload) {
        sdrplay_api_ErrT result = sdrplay_api_Update(
            device_handle, sdrplay_api_Tuner_A,
            sdrplay_api_Update_Ctrl_OverloadMsgAck, 0u);
        (void)pthread_mutex_lock(&metrics->lock);
        metrics->overload_ack_result = result;
        (void)pthread_cond_signal(&metrics->ready);
        (void)pthread_mutex_unlock(&metrics->lock);
    }
}

static int wait_for_callbacks_above(callback_metrics *metrics, unsigned int baseline)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) return -1;
    deadline.tv_sec += 2;
    (void)pthread_mutex_lock(&metrics->lock);
    while (metrics->callbacks <= baseline) {
        if (pthread_cond_timedwait(&metrics->ready, &metrics->lock, &deadline) != 0) {
            (void)pthread_mutex_unlock(&metrics->lock);
            return -1;
        }
    }
    (void)pthread_mutex_unlock(&metrics->lock);
    return 0;
}

static int wait_for_samples_above(callback_metrics *metrics, unsigned int baseline)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) return -1;
    deadline.tv_sec += 2;
    (void)pthread_mutex_lock(&metrics->lock);
    while (metrics->samples <= baseline) {
        if (pthread_cond_timedwait(&metrics->ready, &metrics->lock, &deadline) != 0) {
            (void)pthread_mutex_unlock(&metrics->lock);
            return -1;
        }
    }
    (void)pthread_mutex_unlock(&metrics->lock);
    return 0;
}

static int wait_for_device_failure(callback_metrics *metrics)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) return -1;
    deadline.tv_sec += 2;
    (void)pthread_mutex_lock(&metrics->lock);
    while (metrics->device_failures == 0u) {
        if (pthread_cond_timedwait(&metrics->ready, &metrics->lock, &deadline) != 0) {
            (void)pthread_mutex_unlock(&metrics->lock);
            return -1;
        }
    }
    (void)pthread_mutex_unlock(&metrics->lock);
    return 0;
}

static int wait_for_gain_event(callback_metrics *metrics)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) return -1;
    deadline.tv_sec += 2;
    (void)pthread_mutex_lock(&metrics->lock);
    while (metrics->gain_events == 0u) {
        if (pthread_cond_timedwait(&metrics->ready, &metrics->lock, &deadline) != 0) {
            (void)pthread_mutex_unlock(&metrics->lock);
            return -1;
        }
    }
    (void)pthread_mutex_unlock(&metrics->lock);
    return 0;
}

static int wait_for_overload_events(callback_metrics *metrics, unsigned int count)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) return -1;
    deadline.tv_sec += 2;
    (void)pthread_mutex_lock(&metrics->lock);
    while (metrics->overload_events < count) {
        if (pthread_cond_timedwait(&metrics->ready, &metrics->lock, &deadline) != 0) {
            (void)pthread_mutex_unlock(&metrics->lock);
            return -1;
        }
    }
    (void)pthread_mutex_unlock(&metrics->lock);
    return 0;
}

int main(void)
{
    char socket_path[104];
    (void)snprintf(socket_path, sizeof(socket_path), "/tmp/openrsp-compat-mock-%ld.sock",
                   (long)getpid());
    pid_t daemon = fork();
    if (daemon < 0) return 1;
    if (daemon == 0) _exit(mock_daemon(socket_path));
    for (int attempt = 0; attempt < 100 && access(socket_path, F_OK) != 0; ++attempt) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 10000000L};
        (void)nanosleep(&delay, NULL);
    }
    assert(access(socket_path, F_OK) == 0);
    (void)setenv("OPENRSPD_SOCKET", socket_path, 1);

    float version = 0.0f;
    assert(sdrplay_api_ApiVersion(&version) == sdrplay_api_Success && version == 3.15f);
    assert(sdrplay_api_Open() == sdrplay_api_Success);
    assert(sdrplay_api_DebugEnable(NULL, sdrplay_api_DbgLvl_Error) ==
           sdrplay_api_Success);
    assert(sdrplay_api_DebugEnable(NULL, (sdrplay_api_DbgLvl_t)5) ==
           sdrplay_api_OutOfRange);
    assert(sdrplay_api_DisableHeartbeat() == sdrplay_api_Fail);
    assert(sdrplay_api_LockDeviceApi() == sdrplay_api_Success);
    assert(sdrplay_api_LockDeviceApi() == sdrplay_api_Success);
    assert(sdrplay_api_DisableHeartbeat() == sdrplay_api_Success);
    sdrplay_api_DeviceT devices[SDRPLAY_MAX_DEVICES];
    unsigned int count = 0;
    assert(sdrplay_api_GetDevices(devices, &count, SDRPLAY_MAX_DEVICES) == sdrplay_api_Success);
    assert(count == 4u && devices[0].hwVer == SDRPLAY_RSPduo_ID &&
           devices[1].hwVer == SDRPLAY_RSP1_ID &&
           devices[2].hwVer == SDRPLAY_RSP1A_ID &&
           devices[3].hwVer == SDRPLAY_RSP2_ID);
    assert(devices[0].valid == 1u && devices[1].valid == 1u &&
           devices[2].valid == 1u && devices[3].valid == 1u);
    assert(devices[0].dev != NULL && devices[1].dev != NULL &&
           devices[2].dev != NULL && devices[3].dev != NULL);
    assert(devices[0].rspDuoMode == sdrplay_api_RspDuoMode_Single_Tuner);
    assert(devices[1].rspDuoMode == sdrplay_api_RspDuoMode_Unknown);
    assert(sdrplay_api_SelectDevice(&devices[0]) == sdrplay_api_Success);
    assert(sdrplay_api_DisableHeartbeat() == sdrplay_api_Fail);
    assert(sdrplay_api_Close() == sdrplay_api_Fail);
    api_lock_metrics lock_metrics;
    memset(&lock_metrics, 0, sizeof(lock_metrics));
    atomic_init(&lock_metrics.acquired, 0);
    pthread_t lock_thread;
    assert(pthread_create(&lock_thread, NULL, api_lock_thread, &lock_metrics) == 0);
    const struct timespec lock_probe = {.tv_sec = 0, .tv_nsec = 50000000L};
    (void)nanosleep(&lock_probe, NULL);
    assert(atomic_load(&lock_metrics.acquired) == 0);
    assert(sdrplay_api_UnlockDeviceApi() == sdrplay_api_Success);
    assert(atomic_load(&lock_metrics.acquired) == 0);
    assert(sdrplay_api_UnlockDeviceApi() == sdrplay_api_Success);
    assert(pthread_join(lock_thread, NULL) == 0);
    assert(lock_metrics.lock_result == sdrplay_api_Success);
    assert(lock_metrics.unlock_result == sdrplay_api_Success);
    assert(atomic_load(&lock_metrics.acquired) == 1);
    assert(sdrplay_api_DebugEnable(devices[0].dev, sdrplay_api_DbgLvl_Error) ==
           sdrplay_api_Success);
    assert(sdrplay_api_GetLastError(&devices[0]) != NULL);
    unsigned long long error_time = 1u;
    assert(sdrplay_api_GetLastErrorByType(&devices[0], 0, &error_time) != NULL);
    assert(error_time == 0u);
    for (int unsupported_type = 1; unsupported_type <= 4; ++unsupported_type) {
        error_time = 0x1122334455667788ULL;
        assert(sdrplay_api_GetLastErrorByType(&devices[0], unsupported_type,
                                               &error_time) == NULL);
        assert(error_time == 0x1122334455667788ULL);
    }
    error_time = 0x8877665544332211ULL;
    assert(sdrplay_api_GetLastErrorByType(&devices[0], -1, &error_time) == NULL);
    assert(error_time == 0x8877665544332211ULL);
    sdrplay_api_DeviceParamsT *params = NULL;
    assert(sdrplay_api_GetDeviceParams(devices[0].dev, &params) == sdrplay_api_Success);
    sdrplay_api_TunerSelectT active_tuner = sdrplay_api_Tuner_A;
    assert(sdrplay_api_SwapRspDuoActiveTuner(devices[0].dev, &active_tuner,
                                              sdrplay_api_RspDuo_AMPORT_2) ==
           sdrplay_api_InvalidMode);
    double dual_rate = 2048000.0;
    assert(sdrplay_api_SwapRspDuoDualTunerModeSampleRate(devices[0].dev, &dual_rate,
                                                         6000000.0) ==
           sdrplay_api_InvalidMode);
    assert(sdrplay_api_SwapRspDuoMode(&devices[0], &params,
                                      sdrplay_api_RspDuoMode_Dual_Tuner,
                                      6000000.0, sdrplay_api_Tuner_Both,
                                      sdrplay_api_BW_1_536, sdrplay_api_IF_Zero,
                                      sdrplay_api_RspDuo_AMPORT_2) ==
           sdrplay_api_InvalidMode);

    params->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE;
    callback_metrics metrics = {.lock = PTHREAD_MUTEX_INITIALIZER,
                                .ready = PTHREAD_COND_INITIALIZER,
                                .validate_samples = 1};
    sdrplay_api_CallbackFnsT callbacks = {
        .StreamACbFn = stream_callback, .EventCbFn = event_callback
    };
    for (unsigned int cycle = 0u; cycle < 10u; ++cycle) {
        (void)pthread_mutex_lock(&metrics.lock);
        unsigned int callback_baseline = metrics.callbacks;
        (void)pthread_mutex_unlock(&metrics.lock);
        assert(sdrplay_api_Init(devices[0].dev, &callbacks, &metrics) ==
               sdrplay_api_Success);
        assert(wait_for_callbacks_above(&metrics, callback_baseline) == 0);
        assert(sdrplay_api_Uninit(devices[0].dev) == sdrplay_api_Success);
    }
    (void)pthread_mutex_lock(&metrics.lock);
    unsigned int callback_baseline = metrics.callbacks;
    unsigned int session_sample_baseline = metrics.samples;
    metrics.validate_pointer_reuse = 1;
    metrics.stream_i_pointer = NULL;
    metrics.stream_q_pointer = NULL;
    (void)pthread_mutex_unlock(&metrics.lock);
    assert(sdrplay_api_Init(devices[0].dev, &callbacks, &metrics) == sdrplay_api_Success);
    assert(wait_for_callbacks_above(&metrics, callback_baseline) == 0);
    (void)pthread_mutex_lock(&metrics.lock);
    metrics.device_handle = devices[0].dev;
    (void)pthread_mutex_unlock(&metrics.lock);
    params->rxChannelA->tunerParams.rfFreq.rfHz = 101000000.0;
    params->rxChannelA->tunerParams.gain.gRdB = 42;
    assert(sdrplay_api_Update(devices[0].dev, sdrplay_api_Tuner_A,
                              sdrplay_api_Update_Tuner_Frf | sdrplay_api_Update_Tuner_Gr,
                              0u) == sdrplay_api_Success);
    (void)pthread_mutex_lock(&metrics.lock);
    assert(metrics.rf_changed == 1);
    assert(metrics.gain_changed == 1);
    (void)pthread_mutex_unlock(&metrics.lock);

    params->devParams->fsFreq.fsHz = 1000000.0;
    assert(sdrplay_api_Update(devices[0].dev, sdrplay_api_Tuner_A,
                              sdrplay_api_Update_Dev_Fs, 0u) == sdrplay_api_OutOfRange);
    params->devParams->fsFreq.fsHz = 2000000.0;
    params->rxChannelA->tunerParams.rfFreq.rfHz = 500.0;
    assert(sdrplay_api_Update(devices[0].dev, sdrplay_api_Tuner_A,
                              sdrplay_api_Update_Tuner_Frf, 0u) == sdrplay_api_OutOfRange);
    params->rxChannelA->tunerParams.rfFreq.rfHz = 101000000.0;
    params->rxChannelA->tunerParams.bwType = sdrplay_api_BW_Undefined;
    assert(sdrplay_api_Update(devices[0].dev, sdrplay_api_Tuner_A,
                              sdrplay_api_Update_Tuner_BwType, 0u) ==
           sdrplay_api_OutOfRange);
    params->rxChannelA->tunerParams.bwType = sdrplay_api_BW_0_200;
    params->rxChannelA->tunerParams.ifType = sdrplay_api_IF_Undefined;
    assert(sdrplay_api_Update(devices[0].dev, sdrplay_api_Tuner_A,
                              sdrplay_api_Update_Tuner_IfType, 0u) ==
           sdrplay_api_OutOfRange);
    params->rxChannelA->tunerParams.ifType = sdrplay_api_IF_Zero;
    params->rxChannelA->tunerParams.gain.gRdB = 60;
    assert(sdrplay_api_Update(devices[0].dev, sdrplay_api_Tuner_A,
                              sdrplay_api_Update_Tuner_Gr, 0u) == sdrplay_api_OutOfRange);
    params->rxChannelA->tunerParams.gain.gRdB = 42;
    params->rxChannelA->tunerParams.gain.LNAstate = 10u;
    assert(sdrplay_api_Update(devices[0].dev, sdrplay_api_Tuner_A,
                              sdrplay_api_Update_Tuner_Gr, 0u) == sdrplay_api_OutOfRange);
    params->rxChannelA->tunerParams.gain.LNAstate = 0u;
    params->rxChannelA->ctrlParams.decimation.enable = 2u;
    assert(sdrplay_api_Update(devices[0].dev, sdrplay_api_Tuner_A,
                              sdrplay_api_Update_Ctrl_Decimation, 0u) ==
           sdrplay_api_InvalidParam);
    params->rxChannelA->ctrlParams.decimation.enable = 0u;
    params->rxChannelA->ctrlParams.dcOffset.DCenable = 2u;
    assert(sdrplay_api_Update(devices[0].dev, sdrplay_api_Tuner_A,
                              sdrplay_api_Update_Ctrl_DCoffsetIQimbalance, 0u) ==
           sdrplay_api_InvalidParam);
    params->rxChannelA->ctrlParams.dcOffset.DCenable = 1u;
    params->rxChannelA->ctrlParams.agc.setPoint_dBfs = -61;
    assert(sdrplay_api_Update(devices[0].dev, sdrplay_api_Tuner_A,
                              sdrplay_api_Update_Ctrl_Agc, 0u) ==
           sdrplay_api_OutOfRange);
    params->rxChannelA->ctrlParams.agc.setPoint_dBfs = -60;

    /* Required application setup calls are accepted when their values are supported. */
    params->rxChannelA->tunerParams.loMode = sdrplay_api_LO_Auto;
    params->rxChannelA->ctrlParams.decimation.enable = 0u;
    params->rxChannelA->ctrlParams.decimation.decimationFactor = 1u;
    assert(sdrplay_api_Update(devices[0].dev, sdrplay_api_Tuner_A,
                              sdrplay_api_Update_Tuner_LoMode |
                              sdrplay_api_Update_Ctrl_Decimation |
                              sdrplay_api_Update_Ctrl_DCoffsetIQimbalance |
                              sdrplay_api_Update_Dev_ResetFlags |
                              sdrplay_api_Update_Ctrl_OverloadMsgAck,
                              sdrplay_api_Update_Ext1_None) == sdrplay_api_Success);
    /* The API decimator supports every documented power-of-two factor. */
    (void)pthread_mutex_lock(&metrics.lock);
    unsigned int undecimated_samples = metrics.samples;
    metrics.validate_samples = 0;
    (void)pthread_mutex_unlock(&metrics.lock);
    const unsigned char factors[] = {2u, 4u, 8u, 16u, 32u};
    params->rxChannelA->ctrlParams.decimation.enable = 1u;
    for (size_t index = 0u; index < sizeof(factors) / sizeof(factors[0]); ++index) {
        if (factors[index] == 2u) {
            (void)pthread_mutex_lock(&metrics.lock);
            metrics.measure_filter = 1;
            metrics.filter_frames = 0u;
            (void)pthread_mutex_unlock(&metrics.lock);
        }
        params->rxChannelA->ctrlParams.decimation.decimationFactor = factors[index];
        assert(sdrplay_api_Update(devices[0].dev, sdrplay_api_Tuner_A,
                                  sdrplay_api_Update_Ctrl_Decimation, 0u) ==
               sdrplay_api_Success);
        assert(wait_for_samples_above(&metrics, undecimated_samples) == 0);
        (void)pthread_mutex_lock(&metrics.lock);
        unsigned int produced = metrics.samples - undecimated_samples;
        unsigned int frames = factors[index] == 2u ? 16u : 1u;
        unsigned int expected = frames * 1024u / factors[index];
        if (produced != expected) {
            fprintf(stderr, "decimation x%u produced %u samples, expected %u\n",
                    factors[index], produced, expected);
            abort();
        }
        if (factors[index] == 2u) {
            assert(metrics.filter_frames == 16u);
            assert(metrics.passband_peak > 10000u);
            assert(metrics.stopband_peak < 100u);
            metrics.measure_filter = 0;
        }
        undecimated_samples = metrics.samples;
        (void)pthread_mutex_unlock(&metrics.lock);
    }
    params->rxChannelA->ctrlParams.decimation.decimationFactor = 3u;
    assert(sdrplay_api_Update(devices[0].dev, sdrplay_api_Tuner_A,
                              sdrplay_api_Update_Ctrl_Decimation, 0u) ==
           sdrplay_api_OutOfRange);
    params->rxChannelA->ctrlParams.decimation.enable = 0u;
    params->rxChannelA->ctrlParams.decimation.decimationFactor = 1u;
    assert(sdrplay_api_Update(devices[0].dev, sdrplay_api_Tuner_A,
                              sdrplay_api_Update_Rsp1a_BiasTControl, 0u) ==
           sdrplay_api_HwVerError);
    assert(sdrplay_api_Update(devices[0].dev, sdrplay_api_Tuner_A,
                              sdrplay_api_Update_RspDuo_BiasTControl, 0u) ==
           sdrplay_api_InvalidMode);
    assert(sdrplay_api_Update(devices[0].dev, sdrplay_api_Tuner_A, 0u,
                              0x80u) == sdrplay_api_InvalidParam);

    (void)pthread_mutex_lock(&metrics.lock);
    metrics.auto_ack_overload = 1;
    (void)pthread_mutex_unlock(&metrics.lock);
    params->rxChannelA->tunerParams.rfFreq.rfHz = 102000000.0;
    assert(sdrplay_api_Update(devices[0].dev, sdrplay_api_Tuner_A,
                              sdrplay_api_Update_Tuner_Frf, 0u) ==
           sdrplay_api_Success);
    if (wait_for_overload_events(&metrics, 2u) != 0) {
        (void)pthread_mutex_lock(&metrics.lock);
        fprintf(stderr, "overload timeout events=%u last=%d ack=%d\n",
                metrics.overload_events, (int)metrics.last_overload_event,
                (int)metrics.overload_ack_result);
        (void)pthread_mutex_unlock(&metrics.lock);
        abort();
    }
    (void)pthread_mutex_lock(&metrics.lock);
    assert(metrics.overload_ack_result == sdrplay_api_Success);
    assert(metrics.last_overload_event == sdrplay_api_Overload_Corrected);
    metrics.auto_ack_overload = 0;
    (void)pthread_mutex_unlock(&metrics.lock);
    params->rxChannelA->tunerParams.rfFreq.rfHz = 101000000.0;
    assert(sdrplay_api_Update(devices[0].dev, sdrplay_api_Tuner_A,
                              sdrplay_api_Update_Tuner_Frf, 0u) ==
           sdrplay_api_Success);

    params->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_CTRL_EN;
    params->rxChannelA->ctrlParams.agc.setPoint_dBfs = -60;
    assert(sdrplay_api_Update(devices[0].dev, sdrplay_api_Tuner_A,
                              sdrplay_api_Update_Ctrl_Agc, 0u) ==
           sdrplay_api_Success);
    assert(wait_for_gain_event(&metrics) == 0);
    (void)pthread_mutex_lock(&metrics.lock);
    assert(metrics.event_gr_db > 42u);
    assert(metrics.event_current_gain < 63.0);
    (void)pthread_mutex_unlock(&metrics.lock);

    params->rxChannelA->tunerParams.rfFreq.rfHz = 123456789.0;
    assert(sdrplay_api_Update(devices[0].dev, sdrplay_api_Tuner_A,
                              sdrplay_api_Update_Tuner_Frf, 0u) ==
           sdrplay_api_RfUpdateError);
    sdrplay_api_ErrorInfoT *update_error = sdrplay_api_GetLastError(&devices[0]);
    assert(update_error != NULL);
    assert(strstr(update_error->function, "sdrplay_api_Update") != NULL);
    assert(strstr(update_error->message, "daemon rejected update") != NULL);
    error_time = 0u;
    sdrplay_api_ErrorInfoT *typed_update_error =
        sdrplay_api_GetLastErrorByType(&devices[0], 0, &error_time);
    assert(typed_update_error != NULL);
    assert(error_time != 0u);
    assert(strstr(typed_update_error->function, "sdrplay_api_Update") != NULL);
    assert(strstr(typed_update_error->message, "daemon rejected update") != NULL);
    params->rxChannelA->tunerParams.rfFreq.rfHz = 101000000.0;
    assert(sdrplay_api_Update(devices[0].dev, sdrplay_api_Tuner_A,
                              sdrplay_api_Update_Tuner_Frf, 0u) ==
           sdrplay_api_Success);

    params->devParams->ppm = 2.5;
    (void)pthread_mutex_lock(&metrics.lock);
    unsigned int samples_before_gap = metrics.samples - session_sample_baseline;
    unsigned int resets_before_gap = metrics.reset_callbacks;
    (void)pthread_mutex_unlock(&metrics.lock);
    assert(sdrplay_api_Update(devices[0].dev, sdrplay_api_Tuner_A,
                              sdrplay_api_Update_Dev_Ppm, 0u) ==
           sdrplay_api_Success);
    (void)pthread_mutex_lock(&metrics.lock);
    assert(metrics.reset_callbacks == resets_before_gap + 1u);
    assert(metrics.last_reset_first_sample == samples_before_gap + 64u);
    (void)pthread_mutex_unlock(&metrics.lock);
    params->devParams->ppm = 301.0;
    assert(sdrplay_api_Update(devices[0].dev, sdrplay_api_Tuner_A,
                              sdrplay_api_Update_Dev_Ppm, 0u) ==
           sdrplay_api_OutOfRange);
    (void)kill(daemon, SIGTERM);
    (void)waitpid(daemon, NULL, 0);
    daemon = -1;
    assert(wait_for_device_failure(&metrics) == 0);
    assert(sdrplay_api_Uninit(devices[0].dev) == sdrplay_api_Success);
    assert(sdrplay_api_ReleaseDevice(&devices[0]) == sdrplay_api_Success);
    assert(sdrplay_api_Close() == sdrplay_api_Success);

    if (daemon > 0) {
        (void)kill(daemon, SIGTERM);
        (void)waitpid(daemon, NULL, 0);
    }
    (void)unlink(socket_path);
    (void)pthread_cond_destroy(&metrics.ready);
    (void)pthread_mutex_destroy(&metrics.lock);
    puts("SDRPLAY_API_COMPAT_MOCK_OK");
    return 0;
}
