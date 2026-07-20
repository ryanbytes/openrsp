/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "daemon_backend.h"
#include "openrsp/client.h"

#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define OPENRSP_DAEMON_RESPONSE_TIMEOUT_SECONDS 5
#define OPENRSP_DAEMON_IQ_QUEUE_CAPACITY 32u
#define OPENRSP_DAEMON_IQ_QUEUE_WAIT_NANOSECONDS 20000000L

typedef struct {
    int state;
    size_t samples;
    uint32_t sequence;
    uint32_t tuner;
    int16_t interleaved[OPENRSP_MAX_IQ_SAMPLES * 2u];
} openrsp_daemon_iq_frame;

enum {
    OPENRSP_DAEMON_IQ_FREE = 0,
    OPENRSP_DAEMON_IQ_QUEUED = 1,
    OPENRSP_DAEMON_IQ_ACTIVE = 2
};

struct openrsp_daemon_backend {
    openrsp_client *client;
    pthread_t reader_thread;
    int reader_started;
    pthread_t iq_thread;
    int iq_started;
    pthread_mutex_t iq_lock;
    pthread_cond_t iq_ready;
    int iq_stop;
    unsigned int iq_queue[OPENRSP_DAEMON_IQ_QUEUE_CAPACITY];
    unsigned int iq_queue_head;
    unsigned int iq_queue_count;
    unsigned int iq_active;
    unsigned int iq_drop_count;
    openrsp_daemon_iq_frame *iq_frames;
    pthread_mutex_t request_lock;
    pthread_mutex_t response_lock;
    pthread_cond_t response_ready;
    atomic_uint next_sequence;
    uint32_t waiting_sequence;
    uint32_t waiting_status;
    uint32_t waiting_changed_flags;
    int response_received;
    atomic_int reader_failed;
    atomic_int closing;
    openrsp_daemon_iq_callback callback;
    openrsp_daemon_status_callback status_callback;
    openrsp_daemon_duo_callback duo_callback;
    openrsp_daemon_failure_callback failure_callback;
    void *callback_context;
    uint32_t duo_role;
};

struct openrsp_daemon_api_lock {
    openrsp_client *client;
    uint32_t next_sequence;
};

static int receive_response(openrsp_client *client, uint32_t sequence,
                            openrsp_response *response)
{
    openrsp_message_header header;
    if (openrsp_client_receive(client, &header, response, sizeof(*response)) != 0 ||
        header.type != OPENRSP_MSG_RESPONSE || header.sequence != sequence ||
        header.payload_bytes != sizeof(*response) || response->sequence != sequence)
        return -1;
    return 0;
}

static int request_on_client(openrsp_client *client, uint16_t command,
                             uint32_t sequence, const void *payload,
                             uint32_t payload_bytes)
{
    openrsp_response response;
    if (openrsp_client_send(client, command, sequence, payload, payload_bytes) != 0 ||
        receive_response(client, sequence, &response) != 0)
        return -1;
    return response.status == OPENRSP_STATUS_OK ? 0 : -(int)response.status;
}

static int list_on_client(openrsp_client *client, uint32_t sequence,
                          openrsp_device_record *devices, size_t capacity)
{
    openrsp_response response;
    if (openrsp_client_send(client, OPENRSP_CMD_LIST, sequence, NULL, 0u) != 0 ||
        receive_response(client, sequence, &response) != 0 ||
        response.status != OPENRSP_STATUS_OK)
        return -1;
    uint32_t count = response.changed_flags;
    for (uint32_t index = 0; index < count; ++index) {
        openrsp_message_header header;
        openrsp_device_record record;
        if (openrsp_client_receive(client, &header, &record, sizeof(record)) != 0 ||
            header.type != OPENRSP_EVENT_DEVICE || header.sequence != sequence ||
            header.payload_bytes != sizeof(record))
            return -1;
        if (devices && index < capacity) devices[index] = record;
    }
    return (int)count;
}

int openrsp_daemon_backend_list(openrsp_device_record *devices, size_t capacity)
{
    openrsp_client *client = NULL;
    const char *socket_path = getenv("OPENRSPD_SOCKET");
    if (openrsp_client_connect(socket_path, &client) != 0) {
        openrsp_client_close(client);
        return -1;
    }
    int count = list_on_client(client, 1u, devices, capacity);
    openrsp_client_close(client);
    return count;
}

int openrsp_daemon_api_lock_acquire(openrsp_daemon_api_lock **out_lock)
{
    if (!out_lock) return -1;
    *out_lock = NULL;
    openrsp_daemon_api_lock *lock = calloc(1, sizeof(*lock));
    if (!lock) return -1;
    const char *socket_path = getenv("OPENRSPD_SOCKET");
    if (openrsp_client_connect(socket_path, &lock->client) != 0) {
        free(lock);
        return -1;
    }
    lock->next_sequence = 1u;
    int result = request_on_client(lock->client, OPENRSP_CMD_LOCK_API,
                                   lock->next_sequence++, NULL, 0u);
    if (result != 0) {
        openrsp_client_close(lock->client);
        free(lock);
        return result;
    }
    *out_lock = lock;
    return 0;
}

int openrsp_daemon_api_lock_list(openrsp_daemon_api_lock *lock,
                                 openrsp_device_record *devices, size_t capacity)
{
    if (!lock || !lock->client) return -1;
    return list_on_client(lock->client, lock->next_sequence++, devices, capacity);
}

int openrsp_daemon_api_lock_release(openrsp_daemon_api_lock *lock)
{
    if (!lock) return -1;
    int result = lock->client ?
        request_on_client(lock->client, OPENRSP_CMD_UNLOCK_API,
                          lock->next_sequence++, NULL, 0u) : -1;
    openrsp_client_close(lock->client);
    free(lock);
    return result;
}

static int direct_request(openrsp_daemon_backend *backend, uint16_t command,
                          const void *payload, uint32_t payload_bytes)
{
    uint32_t sequence = atomic_fetch_add(&backend->next_sequence, 1u);
    return request_on_client(backend->client, command, sequence, payload, payload_bytes);
}

static void *iq_main(void *opaque)
{
    openrsp_daemon_backend *backend = opaque;
    for (;;) {
        (void)pthread_mutex_lock(&backend->iq_lock);
        while (backend->iq_queue_count == 0u && !backend->iq_stop)
            (void)pthread_cond_wait(&backend->iq_ready, &backend->iq_lock);
        if (backend->iq_stop) {
            (void)pthread_mutex_unlock(&backend->iq_lock);
            break;
        }
        unsigned int slot = backend->iq_queue[backend->iq_queue_head];
        backend->iq_queue_head =
            (backend->iq_queue_head + 1u) % OPENRSP_DAEMON_IQ_QUEUE_CAPACITY;
        --backend->iq_queue_count;
        openrsp_daemon_iq_frame *frame = &backend->iq_frames[slot];
        frame->state = OPENRSP_DAEMON_IQ_ACTIVE;
        backend->iq_active = 1u;
        (void)pthread_mutex_unlock(&backend->iq_lock);

        openrsp_daemon_iq_callback callback = backend->callback;
        if (callback)
            callback(frame->interleaved, frame->samples, frame->sequence,
                     frame->tuner, backend->callback_context);

        (void)pthread_mutex_lock(&backend->iq_lock);
        frame->state = OPENRSP_DAEMON_IQ_FREE;
        backend->iq_active = 0u;
        (void)pthread_cond_broadcast(&backend->iq_ready);
        (void)pthread_mutex_unlock(&backend->iq_lock);
    }
    return NULL;
}

static void drain_iq_before_response(openrsp_daemon_backend *backend)
{
    (void)pthread_mutex_lock(&backend->iq_lock);
    while ((backend->iq_queue_count != 0u || backend->iq_active != 0u) &&
           !backend->iq_stop)
        (void)pthread_cond_wait(&backend->iq_ready, &backend->iq_lock);
    (void)pthread_mutex_unlock(&backend->iq_lock);
}

static int start_iq_worker(openrsp_daemon_backend *backend)
{
    if (backend->iq_started) return 0;
    backend->iq_stop = 0;
    if (pthread_create(&backend->iq_thread, NULL, iq_main, backend) != 0)
        return -1;
    backend->iq_started = 1;
    return 0;
}

static void stop_iq_worker(openrsp_daemon_backend *backend)
{
    if (!backend->iq_started) return;
    (void)pthread_mutex_lock(&backend->iq_lock);
    backend->iq_stop = 1;
    backend->iq_queue_count = 0u;
    (void)pthread_cond_signal(&backend->iq_ready);
    (void)pthread_mutex_unlock(&backend->iq_lock);
    (void)pthread_join(backend->iq_thread, NULL);
    backend->iq_started = 0;
}

static void queue_iq(openrsp_daemon_backend *backend, const int16_t *interleaved,
                     size_t samples, uint32_t sequence, uint32_t tuner)
{
    if (!backend->callback || samples > OPENRSP_MAX_IQ_SAMPLES) return;
    (void)pthread_mutex_lock(&backend->iq_lock);
    unsigned int slot = OPENRSP_DAEMON_IQ_QUEUE_CAPACITY;
    for (unsigned int index = 0u; index < OPENRSP_DAEMON_IQ_QUEUE_CAPACITY; ++index) {
        if (backend->iq_frames[index].state == OPENRSP_DAEMON_IQ_FREE) {
            slot = index;
            break;
        }
    }
    if (slot == OPENRSP_DAEMON_IQ_QUEUE_CAPACITY &&
        backend->iq_queue_count != 0u) {
        struct timespec deadline;
        if (clock_gettime(CLOCK_REALTIME, &deadline) == 0) {
            deadline.tv_nsec += OPENRSP_DAEMON_IQ_QUEUE_WAIT_NANOSECONDS;
            if (deadline.tv_nsec >= 1000000000L) {
                ++deadline.tv_sec;
                deadline.tv_nsec -= 1000000000L;
            }
            (void)pthread_cond_timedwait(&backend->iq_ready, &backend->iq_lock,
                                         &deadline);
            for (unsigned int index = 0u;
                 index < OPENRSP_DAEMON_IQ_QUEUE_CAPACITY; ++index) {
                if (backend->iq_frames[index].state == OPENRSP_DAEMON_IQ_FREE) {
                    slot = index;
                    break;
                }
            }
        }
    }
    if (slot == OPENRSP_DAEMON_IQ_QUEUE_CAPACITY && backend->iq_queue_count != 0u) {
        slot = backend->iq_queue[backend->iq_queue_head];
        backend->iq_queue_head =
            (backend->iq_queue_head + 1u) % OPENRSP_DAEMON_IQ_QUEUE_CAPACITY;
        --backend->iq_queue_count;
        ++backend->iq_drop_count;
        unsigned int drops = backend->iq_drop_count;
        if ((drops & (drops - 1u)) == 0u)
            fprintf(stderr, "OPENRSP_DAEMON_IQ_DROP count=%u\n", drops);
    }
    if (slot != OPENRSP_DAEMON_IQ_QUEUE_CAPACITY) {
        openrsp_daemon_iq_frame *frame = &backend->iq_frames[slot];
        frame->samples = samples;
        frame->sequence = sequence;
        frame->tuner = tuner;
        memcpy(frame->interleaved, interleaved,
               samples * 2u * sizeof(*interleaved));
        frame->state = OPENRSP_DAEMON_IQ_QUEUED;
        unsigned int tail = (backend->iq_queue_head + backend->iq_queue_count) %
                            OPENRSP_DAEMON_IQ_QUEUE_CAPACITY;
        backend->iq_queue[tail] = slot;
        ++backend->iq_queue_count;
        (void)pthread_cond_signal(&backend->iq_ready);
    }
    (void)pthread_mutex_unlock(&backend->iq_lock);
}

static void *reader_main(void *opaque)
{
    openrsp_daemon_backend *backend = opaque;
    unsigned char payload[262144];
    openrsp_message_header header;
    for (;;) {
        int receive_result = openrsp_client_receive(backend->client, &header, payload,
                                                    sizeof(payload));
        if (receive_result == OPENRSP_CLIENT_TIMEOUT) continue;
        if (receive_result != OPENRSP_CLIENT_OK) break;
        if ((header.type == OPENRSP_EVENT_IQ || header.type == OPENRSP_EVENT_IQ_B) &&
            (header.payload_bytes % 4u) == 0u) {
            static int logged_first_iq;
            if (!logged_first_iq) {
                logged_first_iq = 1;
                fprintf(stderr, "OPENRSP_API_IQ_FIRST bytes=%u\n", header.payload_bytes);
            }
            queue_iq(backend, (const int16_t *)payload, header.payload_bytes / 4u,
                     header.sequence,
                     header.type == OPENRSP_EVENT_IQ_B ? OPENRSP_TUNER_B :
                                                        OPENRSP_TUNER_A);
        } else if (header.type == OPENRSP_EVENT_STATUS &&
                   header.payload_bytes == sizeof(openrsp_device_status)) {
            const openrsp_device_status *status =
                (const openrsp_device_status *)payload;
            openrsp_daemon_status_callback callback = backend->status_callback;
            if (callback)
                callback(status->reason, status->tuner,
                         backend->callback_context);
        } else if (header.type == OPENRSP_EVENT_DUO &&
                   header.payload_bytes == sizeof(openrsp_duo_event)) {
            const openrsp_duo_event *event = (const openrsp_duo_event *)payload;
            openrsp_daemon_duo_callback callback = backend->duo_callback;
            if (callback)
                callback(event->kind, event->tuner,
                         backend->callback_context);
        } else if (header.type == OPENRSP_MSG_RESPONSE &&
                   header.payload_bytes == sizeof(openrsp_response)) {
            const openrsp_response *response = (const openrsp_response *)payload;
            /* Preserve socket ordering at the API boundary: an update response
             * cannot become visible while IQ frames that preceded it on the
             * wire still belong to the old configuration epoch. */
            drain_iq_before_response(backend);
            (void)pthread_mutex_lock(&backend->response_lock);
            if (response->sequence == backend->waiting_sequence) {
                backend->waiting_status = response->status;
                backend->waiting_changed_flags = response->changed_flags;
                backend->response_received = 1;
                (void)pthread_cond_signal(&backend->response_ready);
            }
            (void)pthread_mutex_unlock(&backend->response_lock);
        }
    }
    atomic_store(&backend->reader_failed, 1);
    (void)pthread_mutex_lock(&backend->response_lock);
    (void)pthread_cond_broadcast(&backend->response_ready);
    (void)pthread_mutex_unlock(&backend->response_lock);
    if (!atomic_load(&backend->closing) && backend->failure_callback)
        backend->failure_callback(backend->callback_context);
    return NULL;
}

static int async_request(openrsp_daemon_backend *backend, uint16_t command,
                         const void *payload, uint32_t payload_bytes)
{
    (void)pthread_mutex_lock(&backend->request_lock);
    uint32_t sequence = atomic_fetch_add(&backend->next_sequence, 1u);
    (void)pthread_mutex_lock(&backend->response_lock);
    backend->waiting_sequence = sequence;
    backend->waiting_changed_flags = 0u;
    backend->response_received = 0;
    (void)pthread_mutex_unlock(&backend->response_lock);
    int result = openrsp_client_send(backend->client, command, sequence, payload, payload_bytes);
    if (result == 0) {
        (void)pthread_mutex_lock(&backend->response_lock);
        struct timespec deadline;
        if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
            result = -1;
        } else {
            deadline.tv_sec += OPENRSP_DAEMON_RESPONSE_TIMEOUT_SECONDS;
            while (!backend->response_received && !atomic_load(&backend->reader_failed)) {
                int wait_result = pthread_cond_timedwait(&backend->response_ready,
                                                         &backend->response_lock, &deadline);
                if (wait_result == ETIMEDOUT) {
                    result = -1;
                    break;
                }
                if (wait_result != 0) {
                    result = -1;
                    break;
                }
            }
        }
        if (result == 0) {
            if (backend->response_received && backend->waiting_status == OPENRSP_STATUS_OK) {
                result = (backend->waiting_changed_flags &
                          OPENRSP_RESPONSE_RECOVERY_QUEUED) != 0u ? 1 : 0;
            } else {
                result = -(int)(backend->response_received ? backend->waiting_status :
                                                           OPENRSP_STATUS_IO_ERROR);
            }
        }
        (void)pthread_mutex_unlock(&backend->response_lock);
    }
    (void)pthread_mutex_unlock(&backend->request_lock);
    return result;
}

int openrsp_daemon_backend_open(openrsp_daemon_backend **out_backend,
                                const openrsp_acquire_request *identity)
{
    if (!out_backend || !identity) return -1;
    openrsp_daemon_backend *backend = calloc(1, sizeof(*backend));
    if (!backend) return -1;
    (void)pthread_mutex_init(&backend->request_lock, NULL);
    (void)pthread_mutex_init(&backend->response_lock, NULL);
    (void)pthread_cond_init(&backend->response_ready, NULL);
    (void)pthread_mutex_init(&backend->iq_lock, NULL);
    (void)pthread_cond_init(&backend->iq_ready, NULL);
    backend->iq_frames = calloc(OPENRSP_DAEMON_IQ_QUEUE_CAPACITY,
                                sizeof(*backend->iq_frames));
    if (!backend->iq_frames) {
        openrsp_daemon_backend_close(backend);
        return -1;
    }
    atomic_init(&backend->next_sequence, 1u);
    const char *socket_path = getenv("OPENRSPD_SOCKET");
    if (openrsp_client_connect(socket_path, &backend->client) != 0 ||
        direct_request(backend, OPENRSP_CMD_ACQUIRE, identity, sizeof(*identity)) != 0) {
        openrsp_daemon_backend_close(backend);
        return -1;
    }
    *out_backend = backend;
    return 0;
}

int openrsp_daemon_backend_open_duo(openrsp_daemon_backend **out_backend,
                                    const openrsp_acquire_request *identity,
                                    uint32_t role, uint32_t tuner,
                                    uint32_t sample_rate_hz)
{
    if (!out_backend || !identity ||
        (role != OPENRSP_DUO_ROLE_MASTER && role != OPENRSP_DUO_ROLE_SLAVE))
        return -1;
    openrsp_daemon_backend *backend = calloc(1, sizeof(*backend));
    if (!backend) return -1;
    (void)pthread_mutex_init(&backend->request_lock, NULL);
    (void)pthread_mutex_init(&backend->response_lock, NULL);
    (void)pthread_cond_init(&backend->response_ready, NULL);
    (void)pthread_mutex_init(&backend->iq_lock, NULL);
    (void)pthread_cond_init(&backend->iq_ready, NULL);
    backend->iq_frames = calloc(OPENRSP_DAEMON_IQ_QUEUE_CAPACITY,
                                sizeof(*backend->iq_frames));
    if (!backend->iq_frames) {
        openrsp_daemon_backend_close(backend);
        return -1;
    }
    atomic_init(&backend->next_sequence, 1u);
    const char *socket_path = getenv("OPENRSPD_SOCKET");
    openrsp_duo_acquire_request acquire = {
        .identity = *identity,
        .role = role,
        .tuner = tuner,
        .sample_rate_hz = sample_rate_hz
    };
    if (openrsp_client_connect(socket_path, &backend->client) != 0 ||
        direct_request(backend, OPENRSP_CMD_DUO_ACQUIRE,
                       &acquire, sizeof(acquire)) != 0) {
        openrsp_daemon_backend_close(backend);
        return -1;
    }
    backend->duo_role = role;
    *out_backend = backend;
    return 0;
}

int openrsp_daemon_backend_configure(openrsp_daemon_backend *backend,
                                     const openrsp_radio_config *config)
{
    return backend && config ? direct_request(backend, OPENRSP_CMD_CONFIGURE,
                                               config, sizeof(*config)) : -1;
}

int openrsp_daemon_backend_configure_dual(openrsp_daemon_backend *backend,
                                          const openrsp_dual_config *config)
{
    return backend && config ? direct_request(backend, OPENRSP_CMD_CONFIGURE_DUAL,
                                               config, sizeof(*config)) : -1;
}

int openrsp_daemon_backend_start(openrsp_daemon_backend *backend,
                                 openrsp_daemon_iq_callback callback,
                                 openrsp_daemon_status_callback status_callback,
                                 openrsp_daemon_failure_callback failure_callback,
                                 void *context)
{
    if (!backend || !callback || backend->reader_started) return -1;
    backend->callback = callback;
    backend->status_callback = status_callback;
    backend->failure_callback = failure_callback;
    backend->callback_context = context;
    /* START's response is emitted before the daemon launches its stream
     * thread, so receive it synchronously.  Starting the shared response/IQ
     * reader first creates an avoidable initialization race.  IQ produced
     * between the response and pthread_create remains buffered by the socket. */
    int start_result = direct_request(backend, OPENRSP_CMD_START, NULL, 0u);
    if (start_result != 0) return -1;
    if (start_iq_worker(backend) != 0) {
        (void)direct_request(backend, OPENRSP_CMD_STOP, NULL, 0u);
        return -1;
    }
    if (pthread_create(&backend->reader_thread, NULL, reader_main, backend) != 0) {
        stop_iq_worker(backend);
        (void)direct_request(backend, OPENRSP_CMD_STOP, NULL, 0u);
        return -1;
    }
    backend->reader_started = 1;
    return 0;
}

int openrsp_daemon_backend_start_duo(
    openrsp_daemon_backend *backend, openrsp_daemon_iq_callback callback,
    openrsp_daemon_status_callback status_callback,
    openrsp_daemon_duo_callback duo_callback,
    openrsp_daemon_failure_callback failure_callback, void *context)
{
    if (!backend || !callback) return -1;
    backend->callback = callback;
    backend->status_callback = status_callback;
    backend->duo_callback = duo_callback;
    backend->failure_callback = failure_callback;
    backend->callback_context = context;
    /* A peer event can already be queued on a selected slave socket before
     * its first Init call.  Start the shared reader before DUO_START so that
     * an unsolicited mode event cannot be mistaken for the command response. */
    if (!backend->reader_started) {
        if (start_iq_worker(backend) != 0) return -1;
        if (pthread_create(&backend->reader_thread, NULL, reader_main, backend) != 0) {
            stop_iq_worker(backend);
            return -1;
        }
        backend->reader_started = 1;
    }
    return async_request(backend, OPENRSP_CMD_DUO_START, NULL, 0u);
}

int openrsp_daemon_backend_update(openrsp_daemon_backend *backend,
                                  const openrsp_radio_config *config, uint32_t changed_flags)
{
    openrsp_update_request update = {.changed_flags = changed_flags};
    if (!backend || !config) return -1;
    update.config = *config;
    return async_request(backend, OPENRSP_CMD_UPDATE, &update, sizeof(update));
}

int openrsp_daemon_backend_swap(openrsp_daemon_backend *backend,
                                const openrsp_swap_request *swap)
{
    return backend && swap ? async_request(backend, OPENRSP_CMD_SWAP_TUNER,
                                            swap, sizeof(*swap)) : -1;
}

int openrsp_daemon_backend_swap_mode(openrsp_daemon_backend *backend,
                                     const openrsp_mode_swap_request *swap)
{
    return backend && swap ? async_request(backend, OPENRSP_CMD_SWAP_MODE,
                                            swap, sizeof(*swap)) : -1;
}

int openrsp_daemon_backend_resume_mode(openrsp_daemon_backend *backend)
{
    return backend ? async_request(backend, OPENRSP_CMD_RESUME_MODE,
                                    NULL, 0u) : -1;
}

int openrsp_daemon_backend_stop(openrsp_daemon_backend *backend)
{
    return backend && backend->reader_started ?
           async_request(backend, OPENRSP_CMD_STOP, NULL, 0u) : -1;
}

int openrsp_daemon_backend_configure_duo(openrsp_daemon_backend *backend,
                                         const openrsp_radio_config *config)
{
    return backend && config ?
        (backend->reader_started ?
         async_request(backend, OPENRSP_CMD_DUO_CONFIGURE,
                       config, sizeof(*config)) :
         direct_request(backend, OPENRSP_CMD_DUO_CONFIGURE,
                        config, sizeof(*config))) : -1;
}

int openrsp_daemon_backend_update_duo(openrsp_daemon_backend *backend,
                                      const openrsp_radio_config *config,
                                      uint32_t changed_flags)
{
    openrsp_update_request update = {.changed_flags = changed_flags};
    if (!backend || !config) return -1;
    update.config = *config;
    return async_request(backend, OPENRSP_CMD_DUO_UPDATE,
                         &update, sizeof(update));
}

int openrsp_daemon_backend_stop_duo(openrsp_daemon_backend *backend)
{
    return backend && backend->reader_started ?
        async_request(backend, OPENRSP_CMD_DUO_STOP, NULL, 0u) : -1;
}

int openrsp_daemon_backend_swap_duo_rate(openrsp_daemon_backend *backend,
                                         uint32_t sample_rate_hz)
{
    openrsp_duo_rate_request request = {.sample_rate_hz = sample_rate_hz};
    return backend && backend->duo_role == OPENRSP_DUO_ROLE_MASTER &&
                   backend->reader_started ?
        async_request(backend, OPENRSP_CMD_DUO_SWAP_RATE,
                      &request, sizeof(request)) : -1;
}

void openrsp_daemon_backend_close(openrsp_daemon_backend *backend)
{
    if (!backend) return;
    if (backend->client) {
        if (backend->reader_started) {
            (void)async_request(
                backend, backend->duo_role != 0u ? OPENRSP_CMD_DUO_RELEASE :
                                                   OPENRSP_CMD_RELEASE,
                NULL, 0u);
            atomic_store(&backend->closing, 1);
            openrsp_client_close(backend->client);
            backend->client = NULL;
            (void)pthread_join(backend->reader_thread, NULL);
            stop_iq_worker(backend);
        } else {
            (void)direct_request(
                backend, backend->duo_role != 0u ? OPENRSP_CMD_DUO_RELEASE :
                                                   OPENRSP_CMD_RELEASE,
                NULL, 0u);
            openrsp_client_close(backend->client);
        }
    }
    (void)pthread_cond_destroy(&backend->response_ready);
    (void)pthread_mutex_destroy(&backend->response_lock);
    (void)pthread_mutex_destroy(&backend->request_lock);
    stop_iq_worker(backend);
    (void)pthread_cond_destroy(&backend->iq_ready);
    (void)pthread_mutex_destroy(&backend->iq_lock);
    free(backend->iq_frames);
    free(backend);
}
