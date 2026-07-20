/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "daemon_backend.h"
#include "openrsp/protocol.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

typedef struct {
    pthread_mutex_t lock;
    unsigned int iq_callbacks;
    unsigned int failures;
    unsigned int removals;
    uint32_t last_sequence;
    ULONGLONG first_iq_at;
    ULONGLONG resumed_iq_at;
} callback_state;

typedef struct {
    SOCKET listener;
    int result;
} mock_server_state;

static int transfer_exact(SOCKET descriptor, void *buffer, size_t bytes, int writing)
{
    unsigned char *cursor = buffer;
    while (bytes != 0u) {
        int count = writing ? send(descriptor, (const char *)cursor, (int)bytes, 0) :
                              recv(descriptor, (char *)cursor, (int)bytes, 0);
        if (count <= 0) return -1;
        cursor += (size_t)count;
        bytes -= (size_t)count;
    }
    return 0;
}

static int send_frame(SOCKET descriptor, uint16_t type, uint32_t sequence,
                      const void *payload, uint32_t payload_bytes)
{
    openrsp_message_header header = {
        .magic = OPENRSP_PROTOCOL_MAGIC,
        .version = OPENRSP_PROTOCOL_VERSION,
        .type = type,
        .sequence = sequence,
        .payload_bytes = payload_bytes
    };
    return transfer_exact(descriptor, &header, sizeof(header), 1) == 0 &&
           (payload_bytes == 0u ||
            transfer_exact(descriptor, (void *)payload, payload_bytes, 1) == 0) ? 0 : -1;
}

static int send_response(SOCKET descriptor, uint32_t sequence)
{
    const openrsp_response response = {
        .status = OPENRSP_STATUS_OK,
        .sequence = sequence
    };
    return send_frame(descriptor, OPENRSP_MSG_RESPONSE, sequence,
                      &response, sizeof(response));
}

static int send_iq(SOCKET descriptor, uint32_t sequence)
{
    const int16_t iq[] = {-4, 4, -3, 3, -2, 2, -1, 1};
    return send_frame(descriptor, OPENRSP_EVENT_IQ, sequence, iq, sizeof(iq));
}

static int send_removed(SOCKET descriptor)
{
    const openrsp_device_status status = {
        .reason = OPENRSP_DEVICE_STATUS_REMOVED,
        .tuner = OPENRSP_TUNER_A
    };
    return send_frame(descriptor, OPENRSP_EVENT_STATUS, 0u,
                      &status, sizeof(status));
}

static void *mock_server_main(void *opaque)
{
    mock_server_state *state = opaque;
    SOCKET client = accept(state->listener, NULL, NULL);
    if (client == INVALID_SOCKET) {
        state->result = 1;
        return NULL;
    }
    for (;;) {
        openrsp_message_header request;
        unsigned char payload[4096];
        if (transfer_exact(client, &request, sizeof(request), 0) != 0) break;
        if (request.magic != OPENRSP_PROTOCOL_MAGIC ||
            request.version != OPENRSP_PROTOCOL_VERSION ||
            request.payload_bytes > sizeof(payload) ||
            (request.payload_bytes != 0u &&
             transfer_exact(client, payload, request.payload_bytes, 0) != 0)) {
            state->result = 2;
            break;
        }
        if (send_response(client, request.sequence) != 0) {
            state->result = 3;
            break;
        }
        if (request.type == OPENRSP_CMD_START) {
            if (send_iq(client, 1u) != 0) {
                state->result = 4;
                break;
            }
            /* The client receives with a 100 ms deadline.  This must cross
             * at least three full deadlines without becoming DeviceFailure. */
            Sleep(350u);
            if (send_iq(client, 2u) != 0 || send_removed(client) != 0) {
                state->result = 5;
                break;
            }
        }
    }
    (void)closesocket(client);
    return NULL;
}

static void iq_callback(const int16_t *iq, size_t samples, uint32_t sequence,
                        uint32_t tuner, void *opaque)
{
    callback_state *state = opaque;
    assert(iq != NULL && samples == 4u && tuner == OPENRSP_TUNER_A);
    (void)pthread_mutex_lock(&state->lock);
    ++state->iq_callbacks;
    state->last_sequence = sequence;
    if (sequence == 1u)
        state->first_iq_at = GetTickCount64();
    else if (sequence == 2u)
        state->resumed_iq_at = GetTickCount64();
    (void)pthread_mutex_unlock(&state->lock);
}

static void failure_callback(void *opaque)
{
    callback_state *state = opaque;
    (void)pthread_mutex_lock(&state->lock);
    ++state->failures;
    (void)pthread_mutex_unlock(&state->lock);
}

static void status_callback(uint32_t reason, uint32_t tuner, void *opaque)
{
    callback_state *state = opaque;
    assert(reason == OPENRSP_DEVICE_STATUS_REMOVED && tuner == OPENRSP_TUNER_A);
    (void)pthread_mutex_lock(&state->lock);
    ++state->removals;
    (void)pthread_mutex_unlock(&state->lock);
}

static int wait_for_recovery(callback_state *state)
{
    const ULONGLONG deadline = GetTickCount64() + 3000u;
    for (;;) {
        (void)pthread_mutex_lock(&state->lock);
        const int complete = state->iq_callbacks == 2u &&
                             state->last_sequence == 2u &&
                             state->removals == 1u && state->failures == 0u;
        const ULONGLONG gap = state->resumed_iq_at - state->first_iq_at;
        (void)pthread_mutex_unlock(&state->lock);
        if (complete) return gap >= 300u ? 0 : -1;
        if (GetTickCount64() >= deadline) return -1;
        Sleep(10u);
    }
}

int main(void)
{
    WSADATA winsock;
    assert(WSAStartup(MAKEWORD(2, 2), &winsock) == 0);
    const char *port_text = getenv("OPENRSPD_PORT");
    assert(port_text != NULL && port_text[0] != '\0');
    const unsigned long port = strtoul(port_text, NULL, 10);
    assert(port > 0ul && port <= 65535ul);

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(listener != INVALID_SOCKET);
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons((u_short)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(listener, (const struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(listener, 1) == 0);
    mock_server_state server = {.listener = listener};
    pthread_t server_thread;
    assert(pthread_create(&server_thread, NULL, mock_server_main, &server) == 0);

    openrsp_acquire_request identity = {
        .device_index = 1u,
        .vendor_id = 0x1df7u,
        .product_id = 0x3020u
    };
    (void)strcpy(identity.serial, "WINDOWS-RECOVERY-TEST");
    openrsp_daemon_backend *backend = NULL;
    assert(openrsp_daemon_backend_open(&backend, &identity) == 0);
    const openrsp_radio_config config = {
        .sample_rate_hz = 2000000u,
        .center_frequency_hz = 101100000u,
        .bandwidth_hz = 1536000u,
        .gain_reduction_db = 50
    };
    assert(openrsp_daemon_backend_configure(backend, &config) == 0);
    callback_state callbacks = {.lock = PTHREAD_MUTEX_INITIALIZER};
    assert(openrsp_daemon_backend_start(backend, iq_callback, status_callback,
                                        failure_callback, &callbacks) == 0);
    assert(wait_for_recovery(&callbacks) == 0);
    assert(openrsp_daemon_backend_stop(backend) == 0);
    openrsp_daemon_backend_close(backend);
    assert(pthread_join(server_thread, NULL) == 0);
    (void)closesocket(listener);
    assert(server.result == 0);
    assert(callbacks.failures == 0u && callbacks.removals == 1u);
    (void)pthread_mutex_destroy(&callbacks.lock);
    WSACleanup();
    puts("WINDOWS_DAEMON_BACKEND_RECOVERY_OK");
    return 0;
}
