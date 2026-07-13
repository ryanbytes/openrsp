/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "openrsp/protocol.h"
#include "sdrplay_api_compat.h"

#include <assert.h>
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

static int send_response(int descriptor, uint32_t sequence)
{
    const openrsp_response response = {.status = OPENRSP_STATUS_OK, .sequence = sequence};
    return send_frame(descriptor, OPENRSP_MSG_RESPONSE, sequence, &response, sizeof(response));
}

static int serve_client(int descriptor)
{
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
                .status = OPENRSP_STATUS_OK, .sequence = request.sequence, .changed_flags = 1u
            };
            const openrsp_device_record device = {
                .device_index = 0u, .vendor_id = 0x1df7u, .product_id = 0x3020u,
                .serial = "MOCK-RSPDUO", .model = "SDRplay RSPduo"
            };
            if (send_frame(descriptor, OPENRSP_MSG_RESPONSE, request.sequence,
                           &response, sizeof(response)) != 0 ||
                send_frame(descriptor, OPENRSP_EVENT_DEVICE, request.sequence,
                           &device, sizeof(device)) != 0)
                return -1;
        } else {
            if (send_response(descriptor, request.sequence) != 0) return -1;
            if (request.type == OPENRSP_CMD_START) {
                int16_t iq[2048];
                for (size_t index = 0u; index < sizeof(iq) / sizeof(iq[0]); ++index)
                    iq[index] = (int16_t)(index - 1024);
                if (send_frame(descriptor, OPENRSP_EVENT_IQ, request.sequence,
                               iq, sizeof(iq)) != 0)
                    return -1;
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
} callback_metrics;

static void stream_callback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params,
                            unsigned int samples, unsigned int reset, void *opaque)
{
    (void)reset;
    callback_metrics *metrics = opaque;
    assert(samples == params->numSamples);
    assert(xi[0] == -1024 && xq[0] == -1023);
    (void)pthread_mutex_lock(&metrics->lock);
    ++metrics->callbacks;
    metrics->samples += samples;
    metrics->rf_changed += params->rfChanged;
    metrics->gain_changed += params->grChanged;
    (void)pthread_cond_signal(&metrics->ready);
    (void)pthread_mutex_unlock(&metrics->lock);
}

static int wait_for_callback(callback_metrics *metrics)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) return -1;
    deadline.tv_sec += 2;
    (void)pthread_mutex_lock(&metrics->lock);
    while (metrics->callbacks == 0u) {
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
    sdrplay_api_DeviceT devices[SDRPLAY_MAX_DEVICES];
    unsigned int count = 0;
    assert(sdrplay_api_GetDevices(devices, &count, SDRPLAY_MAX_DEVICES) == sdrplay_api_Success);
    assert(count == 1u && devices[0].hwVer == 3u && devices[0].valid == 1u);
    assert(sdrplay_api_SelectDevice(&devices[0]) == sdrplay_api_Success);
    sdrplay_api_DeviceParamsT *params = NULL;
    assert(sdrplay_api_GetDeviceParams(devices[0].dev, &params) == sdrplay_api_Success);

    callback_metrics metrics = {.lock = PTHREAD_MUTEX_INITIALIZER, .ready = PTHREAD_COND_INITIALIZER};
    sdrplay_api_CallbackFnsT callbacks = {.StreamACbFn = stream_callback};
    assert(sdrplay_api_Init(devices[0].dev, &callbacks, &metrics) == sdrplay_api_Success);
    assert(wait_for_callback(&metrics) == 0);
    params->rxChannelA->tunerParams.rfFreq.rfHz = 101000000.0;
    params->rxChannelA->tunerParams.gain.gRdB = 42;
    assert(sdrplay_api_Update(devices[0].dev, sdrplay_api_Tuner_A,
                              sdrplay_api_Update_Tuner_Frf | sdrplay_api_Update_Tuner_Gr,
                              0u) == sdrplay_api_Success);
    assert(sdrplay_api_Uninit(devices[0].dev) == sdrplay_api_Success);
    assert(sdrplay_api_ReleaseDevice(&devices[0]) == sdrplay_api_Success);
    assert(sdrplay_api_Close() == sdrplay_api_Success);

    (void)kill(daemon, SIGTERM);
    (void)waitpid(daemon, NULL, 0);
    (void)unlink(socket_path);
    (void)pthread_cond_destroy(&metrics.ready);
    (void)pthread_mutex_destroy(&metrics.lock);
    puts("SDRPLAY_API_COMPAT_MOCK_OK");
    return 0;
}
