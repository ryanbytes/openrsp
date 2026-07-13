/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "daemon_backend.h"
#include "openrsp/protocol.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    unsigned int iq_callbacks;
    unsigned int failures;
    uint32_t last_sequence;
} callback_state;

static int transfer_exact(int descriptor, void *buffer, size_t bytes, int writing)
{
    unsigned char *cursor = buffer;
    while (bytes != 0u) {
        ssize_t count = writing ? write(descriptor, cursor, bytes) :
                                  read(descriptor, cursor, bytes);
        if (count <= 0) return -1;
        cursor += (size_t)count;
        bytes -= (size_t)count;
    }
    return 0;
}

static int send_frame(int descriptor, uint16_t type, uint32_t sequence,
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

static int send_response(int descriptor, uint32_t sequence)
{
    openrsp_response response = {
        .status = OPENRSP_STATUS_OK,
        .sequence = sequence
    };
    return send_frame(descriptor, OPENRSP_MSG_RESPONSE, sequence,
                      &response, sizeof(response));
}

static int send_iq(int descriptor, uint32_t sequence)
{
    const int16_t iq[] = {-4, 4, -3, 3, -2, 2, -1, 1};
    return send_frame(descriptor, OPENRSP_EVENT_IQ, sequence, iq, sizeof(iq));
}

static int run_server(const char *socket_path)
{
    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0) return 1;
    struct sockaddr_un address = {0};
    address.sun_family = AF_UNIX;
    (void)strcpy(address.sun_path, socket_path);
    (void)unlink(socket_path);
    if (bind(server, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        chmod(socket_path, 0600) != 0 || listen(server, 1) != 0)
        return 2;
    int client = accept(server, NULL, NULL);
    if (client < 0) return 3;

    for (;;) {
        openrsp_message_header request;
        unsigned char payload[4096];
        if (transfer_exact(client, &request, sizeof(request), 0) != 0) break;
        if (request.magic != OPENRSP_PROTOCOL_MAGIC ||
            request.version != OPENRSP_PROTOCOL_VERSION ||
            request.payload_bytes > sizeof(payload))
            return 4;
        if (request.payload_bytes != 0u &&
            transfer_exact(client, payload, request.payload_bytes, 0) != 0)
            return 5;
        if (send_response(client, request.sequence) != 0) return 6;
        if (request.type == OPENRSP_CMD_START) {
            if (send_iq(client, 1u) != 0) return 7;
            /* The test client has a 100 ms receive deadline.  This deliberate
             * recovery gap crosses three complete deadlines before IQ resumes. */
            const struct timespec recovery_gap = {.tv_sec = 0, .tv_nsec = 350000000L};
            (void)nanosleep(&recovery_gap, NULL);
            if (send_iq(client, 2u) != 0) return 8;
        }
    }

    (void)close(client);
    (void)close(server);
    (void)unlink(socket_path);
    return 0;
}

static void iq_callback(const int16_t *iq, size_t samples, uint32_t sequence,
                        void *opaque)
{
    callback_state *state = opaque;
    assert(iq != NULL && samples == 4u);
    (void)pthread_mutex_lock(&state->lock);
    ++state->iq_callbacks;
    state->last_sequence = sequence;
    (void)pthread_cond_broadcast(&state->ready);
    (void)pthread_mutex_unlock(&state->lock);
}

static void failure_callback(void *opaque)
{
    callback_state *state = opaque;
    (void)pthread_mutex_lock(&state->lock);
    ++state->failures;
    (void)pthread_cond_broadcast(&state->ready);
    (void)pthread_mutex_unlock(&state->lock);
}

static int wait_for_resumed_iq(callback_state *state)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) return -1;
    deadline.tv_sec += 2;
    (void)pthread_mutex_lock(&state->lock);
    while (state->iq_callbacks < 2u && state->failures == 0u) {
        if (pthread_cond_timedwait(&state->ready, &state->lock, &deadline) != 0) {
            (void)pthread_mutex_unlock(&state->lock);
            return -1;
        }
    }
    int result = state->iq_callbacks == 2u && state->last_sequence == 2u &&
                 state->failures == 0u ? 0 : -1;
    (void)pthread_mutex_unlock(&state->lock);
    return result;
}

int main(void)
{
    char socket_path[104];
    (void)snprintf(socket_path, sizeof(socket_path),
                   "/tmp/openrsp-backend-recovery-%ld.sock", (long)getpid());
    pid_t server = fork();
    assert(server >= 0);
    if (server == 0) _exit(run_server(socket_path));
    for (unsigned int attempt = 0u; attempt < 200u && access(socket_path, F_OK) != 0;
         ++attempt) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 10000000L};
        (void)nanosleep(&delay, NULL);
    }
    assert(access(socket_path, F_OK) == 0);
    assert(setenv("OPENRSPD_SOCKET", socket_path, 1) == 0);

    openrsp_acquire_request identity = {
        .device_index = 1u,
        .vendor_id = 0x1df7u,
        .product_id = 0x3020u
    };
    (void)strcpy(identity.serial, "RECOVERY-TEST");
    openrsp_daemon_backend *backend = NULL;
    assert(openrsp_daemon_backend_open(&backend, &identity) == 0);
    openrsp_radio_config config = {
        .sample_rate_hz = 2000000u,
        .center_frequency_hz = 101100000u,
        .bandwidth_hz = 1536000u,
        .gain_reduction_db = 50
    };
    assert(openrsp_daemon_backend_configure(backend, &config) == 0);
    callback_state state = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .ready = PTHREAD_COND_INITIALIZER
    };
    assert(openrsp_daemon_backend_start(backend, iq_callback, failure_callback,
                                        &state) == 0);
    assert(wait_for_resumed_iq(&state) == 0);
    assert(openrsp_daemon_backend_stop(backend) == 0);
    openrsp_daemon_backend_close(backend);

    int status = 0;
    assert(waitpid(server, &status, 0) == server);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(state.failures == 0u);
    (void)pthread_cond_destroy(&state.ready);
    (void)pthread_mutex_destroy(&state.lock);
    (void)unlink(socket_path);
    puts("OPENRSP_DAEMON_BACKEND_RECOVERY_OK");
    return 0;
}
