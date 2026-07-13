/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "openrsp/protocol.h"
#include "mirisdr.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <fcntl.h>

#define OPENRSPD_MAX_PAYLOAD 4096u
#define OPENRSPD_MAX_CLIENTS 32
#define OPENRSPD_STREAM_SLOT_BYTES (OPENRSP_MAX_IQ_SAMPLES * 4u)

typedef struct {
    mirisdr_dev_t *radio;
    int owner;
    uint32_t acquired_device_index;
    atomic_bool streaming;
    bool stream_thread_started;
    atomic_bool stream_error;
    atomic_int stream_result;
    pthread_t stream_thread;
    pthread_mutex_t write_lock;
    atomic_bool control_write_pending;
    bool logged_usb_iq;
    bool logged_socket_iq;
    atomic_bool first_iq_seen;
    bool recovery_gain_pending;
    uint32_t stream_sequence;
    bool configured;
    openrsp_radio_config config;
} daemon_state;

static volatile sig_atomic_t running = 1;

static void stop_server(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static int read_exact(int descriptor, void *buffer, size_t bytes)
{
    unsigned char *cursor = buffer;
    while (bytes != 0u) {
        ssize_t count = read(descriptor, cursor, bytes);
        if (count == 0) return 0;
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        cursor += (size_t)count;
        bytes -= (size_t)count;
    }
    return 1;
}

static int write_exact(int descriptor, const void *buffer, size_t bytes)
{
    const unsigned char *cursor = buffer;
    while (bytes != 0u) {
        ssize_t count = write(descriptor, cursor, bytes);
        if (count < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        cursor += (size_t)count;
        bytes -= (size_t)count;
    }
    return 0;
}

static int set_nonblocking(int descriptor)
{
    int flags = fcntl(descriptor, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
}

static int set_blocking(int descriptor)
{
    int flags = fcntl(descriptor, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(descriptor, F_SETFL, flags & ~O_NONBLOCK);
}

static int write_frame(daemon_state *state, int descriptor, uint16_t type,
                       uint32_t sequence, const void *payload, uint32_t payload_bytes)
{
    openrsp_message_header header = {
        .magic = OPENRSP_PROTOCOL_MAGIC,
        .version = OPENRSP_PROTOCOL_VERSION,
        .type = type,
        .sequence = sequence,
        .payload_bytes = payload_bytes
    };
    (void)pthread_mutex_lock(&state->write_lock);
    int result = write_exact(descriptor, &header, sizeof(header));
    if (result == 0 && payload_bytes != 0u)
        result = write_exact(descriptor, payload, payload_bytes);
    (void)pthread_mutex_unlock(&state->write_lock);
    return result;
}

static int write_control_frame(daemon_state *state, int descriptor, uint16_t type,
                               uint32_t sequence, const void *payload,
                               uint32_t payload_bytes)
{
    /* IQ is a continuous high-rate producer.  Without an explicit priority
     * gate it can reacquire write_lock indefinitely and leave a tiny command
     * response queued for seconds, making a successful hardware update look
     * like an API timeout. */
    atomic_store(&state->control_write_pending, true);
    int result = write_frame(state, descriptor, type, sequence, payload, payload_bytes);
    atomic_store(&state->control_write_pending, false);
    return result;
}

static void stream_callback(unsigned char *buffer, uint32_t length, void *opaque)
{
    daemon_state *state = opaque;
    while (atomic_load(&state->control_write_pending)) {
        const struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&pause, NULL);
    }
    if (!state->logged_usb_iq) {
        state->logged_usb_iq = true;
        atomic_store(&state->first_iq_seen, true);
        fprintf(stderr, "OPENRSPD_USB_IQ_FIRST bytes=%u\n", length);
        (void)fflush(stderr);
    }
    if (length > OPENRSPD_STREAM_SLOT_BYTES) {
        fprintf(stderr, "OPENRSPD_IQ_OVERSIZE bytes=%u\n", length);
        if (state->radio) (void)mirisdr_cancel_async(state->radio);
        return;
    }
    uint32_t sequence = ++state->stream_sequence;
    if (write_frame(state, state->owner, OPENRSP_EVENT_IQ, sequence,
                    buffer, length) != 0) {
        if (state->radio) (void)mirisdr_cancel_async(state->radio);
        return;
    }
    if (!state->logged_socket_iq) {
        state->logged_socket_iq = true;
        fprintf(stderr, "OPENRSPD_SOCKET_IQ_FIRST bytes=%u sequence=%u\n",
                length, sequence);
        (void)fflush(stderr);
    }
}

static void *stream_main(void *opaque)
{
    daemon_state *state = opaque;
    fprintf(stderr, "OPENRSPD_STREAM_THREAD_START\n");
    (void)fflush(stderr);
    /* Keep the proven 32-transfer USB queue and 64 KiB USB buffers.  Aggregate
     * converted samples into 64 KiB daemon frames: a 256 KiB first socket
     * write can fill the macOS Unix-socket window before API startup finishes. */
    int result = mirisdr_read_async(state->radio, stream_callback, state, 32u, 65536u);
    state->streaming = false;
    state->stream_result = result;
    state->stream_error = result != 0;
    if (result != 0) {
        fprintf(stderr, "RSP stream stopped with error %d\n", result);
        (void)fflush(stderr);
    }
    return NULL;
}

static int clamp_int(int value, int minimum, int maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

static int set_gain(mirisdr_dev_t *radio, const openrsp_radio_config *config)
{
    /* The independently captured RSPduo sequence controls IF gain reduction
     * and the physical LNA GPIO state separately.  It is applied only after
     * streaming has started; startup remains on the proven generic state. */
    if (config->center_frequency_hz >= 420000000u &&
        config->center_frequency_hz < 1000000000u)
        return mirisdr_set_rspduo_gain(radio, config->gain_reduction_db,
                                       config->lna_state);

    /* Other bands retain the calculated total-gain path until their RSPduo
     * GPIO tables are independently captured and verified. */
    static const int below_60mhz[] = {0, 6, 12, 18, 37, 42, 61};
    static const int below_420mhz[] = {0, 6, 12, 18, 20, 26, 32, 38, 57, 62};
    static const int below_2ghz[] = {0, 6, 12, 20, 26, 32, 38, 43, 62};
    const int *table = below_2ghz;
    size_t count = sizeof(below_2ghz) / sizeof(below_2ghz[0]);
    if (config->center_frequency_hz < 60000000u) {
        table = below_60mhz;
        count = sizeof(below_60mhz) / sizeof(below_60mhz[0]);
    } else if (config->center_frequency_hz < 420000000u) {
        table = below_420mhz;
        count = sizeof(below_420mhz) / sizeof(below_420mhz[0]);
    }
    if (config->lna_state >= count || config->gain_reduction_db < 20 ||
        config->gain_reduction_db > 59) return -1;
    int total_reduction = config->gain_reduction_db + table[config->lna_state];
    return mirisdr_set_tuner_gain(radio, clamp_int(102 - total_reduction, 0, 102));
}

static void describe_flags(uint32_t flags, char *buffer, size_t bytes)
{
    if (bytes == 0u) return;
    buffer[0] = '\0';
    struct flag_name {
        uint32_t flag;
        const char *name;
    };
    static const struct flag_name names[] = {
        {OPENRSP_CHANGE_SAMPLE_RATE, "FS"},
        {OPENRSP_CHANGE_RF, "RF"},
        {OPENRSP_CHANGE_BANDWIDTH, "BW"},
        {OPENRSP_CHANGE_IF, "IF"},
        {OPENRSP_CHANGE_GAIN, "GAIN"},
        {OPENRSP_CHANGE_AGC, "AGC"}
    };
    size_t used = 0u;
    for (size_t index = 0u; index < sizeof(names) / sizeof(names[0]); ++index) {
        if ((flags & names[index].flag) == 0u) continue;
        int written = snprintf(buffer + used, bytes - used, "%s%s",
                               used == 0u ? "" : "|", names[index].name);
        if (written < 0) break;
        if ((size_t)written >= bytes - used) {
            buffer[bytes - 1u] = '\0';
            return;
        }
        used += (size_t)written;
    }
    if (used == 0u) (void)snprintf(buffer, bytes, "none");
}

static void log_config(const char *operation, int descriptor, uint32_t flags,
                       uint32_t status, const openrsp_radio_config *config)
{
    char flag_text[64];
    describe_flags(flags, flag_text, sizeof(flag_text));
    fprintf(stderr,
            "OPENRSPD_%s fd=%d status=%u flags=%s fs=%u rf=%u bw=%u if=%d gr=%d lna=%u agc=%d setpoint=%d\n",
            operation, descriptor, status, flag_text, config->sample_rate_hz,
            config->center_frequency_hz, config->bandwidth_hz,
            config->if_frequency_hz, config->gain_reduction_db,
            config->lna_state, config->agc_mode, config->agc_setpoint_dbfs);
    (void)fflush(stderr);
}

static int stop_stream(daemon_state *state)
{
    if (!state->stream_thread_started) return 0;
    if (state->radio) {
        (void)mirisdr_streaming_stop(state->radio);
        (void)mirisdr_cancel_async(state->radio);
    }
    int result = pthread_join(state->stream_thread, NULL);
    state->stream_thread_started = false;
    state->streaming = false;
    return result;
}

static void release_client(daemon_state *state, int descriptor)
{
    if (state->owner != descriptor) return;
    if (state->radio) (void)stop_stream(state);
    if (state->stream_error && state->radio) {
        fprintf(stderr, "OPENRSPD_CLOSE_FAILED_STREAM result=%d\n", state->stream_result);
        (void)fflush(stderr);
        (void)mirisdr_close(state->radio);
        state->radio = NULL;
        state->configured = false;
    }
    state->owner = -1;
    state->streaming = false;
    state->stream_error = false;
    state->stream_result = 0;
}

static void shutdown_radio(daemon_state *state)
{
    if (!state->radio) return;
    (void)stop_stream(state);
    (void)mirisdr_close(state->radio);
    state->radio = NULL;
    state->owner = -1;
    state->configured = false;
    state->stream_error = false;
    state->stream_result = 0;
}

static bool valid_config(const openrsp_radio_config *config)
{
    return config && config->sample_rate_hz != 0u && config->center_frequency_hz != 0u &&
           config->gain_reduction_db >= 20 && config->gain_reduction_db <= 59 &&
           config->lna_state <= 9u && config->agc_mode >= 0 && config->agc_mode <= 4 &&
           config->agc_setpoint_dbfs >= -60 && config->agc_setpoint_dbfs <= -20;
}

static uint32_t apply_config(mirisdr_dev_t *radio, const openrsp_radio_config *config)
{
    if (!valid_config(config)) return OPENRSP_STATUS_BAD_REQUEST;
    /* Keep this order identical to the direct openrsp-iq path.  That path is
     * the hardware gate: it streams at 2.048 MS/s, while the former combined
     * configure helper left endpoint 0x81 stalled before the first callback. */
    if (mirisdr_set_hw_flavour(radio, MIRISDR_HW_SDRPLAY) != 0 ||
        mirisdr_set_sample_rate(radio, config->sample_rate_hz) != 0 ||
        mirisdr_set_center_freq(radio, config->center_frequency_hz) != 0 ||
        mirisdr_set_sample_format(radio, "AUTO") != 0 ||
        mirisdr_set_transfer(radio, "BULK") != 0 ||
        mirisdr_set_if_freq(radio, (uint32_t)config->if_frequency_hz) != 0 ||
        mirisdr_set_bandwidth(radio, config->bandwidth_hz) != 0 ||
        mirisdr_set_tuner_gain_mode(radio, 1) != 0 ||
        /* The direct hardware gate starts at this register state.  The API
         * applies the caller's requested GR/LNA setting after first IQ. */
        mirisdr_set_tuner_gain(radio, 102) != 0)
        return OPENRSP_STATUS_IO_ERROR;
    return OPENRSP_STATUS_OK;
}

static void recover_failed_stream(daemon_state *state)
{
    if (!atomic_load(&state->stream_error) || atomic_load(&state->streaming)) return;

    if (state->stream_thread_started) {
        (void)pthread_join(state->stream_thread, NULL);
        state->stream_thread_started = false;
    }
    if (state->radio) {
        (void)mirisdr_close(state->radio);
        state->radio = NULL;
    }
    state->configured = false;
    state->recovery_gain_pending = false;
    if (state->owner < 0) return;
    if (state->acquired_device_index >= mirisdr_get_device_count()) return;

    fprintf(stderr, "OPENRSPD_RECOVERY_REOPEN index=%u\n", state->acquired_device_index);
    (void)fflush(stderr);
    if (mirisdr_open(&state->radio, state->acquired_device_index) != 0) return;
    uint32_t status = apply_config(state->radio, &state->config);
    if (status != OPENRSP_STATUS_OK) {
        (void)mirisdr_close(state->radio);
        state->radio = NULL;
        return;
    }
    state->configured = true;
    state->logged_usb_iq = false;
    state->logged_socket_iq = false;
    atomic_store(&state->first_iq_seen, false);
    atomic_store(&state->stream_error, false);
    atomic_store(&state->stream_result, 0);
    atomic_store(&state->streaming, true);
    if (pthread_create(&state->stream_thread, NULL, stream_main, state) != 0) {
        atomic_store(&state->streaming, false);
        atomic_store(&state->stream_error, true);
        (void)mirisdr_close(state->radio);
        state->radio = NULL;
        return;
    }
    state->stream_thread_started = true;
    state->recovery_gain_pending = true;
}

static void finish_recovery_gain(daemon_state *state)
{
    if (!state->recovery_gain_pending || !atomic_load(&state->first_iq_seen) ||
        !state->radio) return;
    int result = set_gain(state->radio, &state->config);
    fprintf(stderr, "OPENRSPD_RECOVERY_GAIN status=%d gr=%d lna=%u\n",
            result, state->config.gain_reduction_db, state->config.lna_state);
    (void)fflush(stderr);
    state->recovery_gain_pending = false;
}

static uint32_t apply_update(daemon_state *state, const openrsp_update_request *update)
{
    const openrsp_radio_config *next = &update->config;
    const openrsp_radio_config *old = &state->config;
    uint32_t flags = update->changed_flags;
    int result = 0;
    if ((flags & OPENRSP_CHANGE_AGC) != 0u &&
        (next->agc_mode < 0 || next->agc_mode > 4 ||
         next->agc_setpoint_dbfs < -60 || next->agc_setpoint_dbfs > -20))
        return OPENRSP_STATUS_BAD_REQUEST;
    if ((flags & OPENRSP_CHANGE_SAMPLE_RATE) != 0u &&
        next->sample_rate_hz != old->sample_rate_hz)
        result |= mirisdr_set_sample_rate(state->radio, next->sample_rate_hz);
    if ((flags & OPENRSP_CHANGE_RF) != 0u &&
        next->center_frequency_hz != old->center_frequency_hz)
        result |= mirisdr_set_center_freq(state->radio, next->center_frequency_hz);
    if ((flags & OPENRSP_CHANGE_BANDWIDTH) != 0u &&
        next->bandwidth_hz != old->bandwidth_hz)
        result |= mirisdr_set_bandwidth(state->radio, next->bandwidth_hz);
    if ((flags & OPENRSP_CHANGE_IF) != 0u &&
        next->if_frequency_hz != old->if_frequency_hz)
        result |= mirisdr_set_if_freq(state->radio, (uint32_t)next->if_frequency_hz);
    if ((flags & (OPENRSP_CHANGE_GAIN | OPENRSP_CHANGE_RF)) != 0u)
        result |= set_gain(state->radio, next);
    if (result < 0) return OPENRSP_STATUS_IO_ERROR;
    state->config = *next;
    return OPENRSP_STATUS_OK;
}

static int serve_request(int descriptor, daemon_state *state)
{
    openrsp_message_header request;
    int read_result = read_exact(descriptor, &request, sizeof(request));
    if (read_result != 1) return read_result;
    unsigned char payload[OPENRSPD_MAX_PAYLOAD];
    if (request.payload_bytes > sizeof(payload)) return -1;
    if (request.payload_bytes != 0u &&
        read_exact(descriptor, payload, request.payload_bytes) != 1) return -1;

    openrsp_response response = {0};
    response.sequence = request.sequence;
    if (request.magic != OPENRSP_PROTOCOL_MAGIC || request.version != OPENRSP_PROTOCOL_VERSION) {
        response.status = OPENRSP_STATUS_BAD_REQUEST;
    } else if (request.type == OPENRSP_CMD_PING && request.payload_bytes == 0u) {
        response.status = OPENRSP_STATUS_OK;
    } else if (request.type == OPENRSP_CMD_LIST && request.payload_bytes == 0u) {
        response.status = OPENRSP_STATUS_OK;
        response.changed_flags = mirisdr_get_device_count();
    } else if (request.type == OPENRSP_CMD_ACQUIRE &&
               request.payload_bytes == sizeof(openrsp_acquire_request)) {
        const openrsp_acquire_request *acquire = (const openrsp_acquire_request *)payload;
        if (state->owner >= 0 && state->owner != descriptor) {
            response.status = OPENRSP_STATUS_BUSY;
        } else if (state->owner == descriptor) {
            response.status = OPENRSP_STATUS_OK;
        } else if (acquire->device_index >= mirisdr_get_device_count()) {
            response.status = OPENRSP_STATUS_BAD_REQUEST;
        } else {
            state->owner = descriptor;
            state->acquired_device_index = acquire->device_index;
            response.status = OPENRSP_STATUS_OK;
        }
    } else if (request.type == OPENRSP_CMD_START && request.payload_bytes == 0u &&
               state->owner == descriptor && state->radio && !state->stream_thread_started) {
        state->streaming = true;
        state->stream_error = false;
        state->stream_result = 0;
        state->stream_sequence = 0u;
        response.status = OPENRSP_STATUS_OK;
    } else if (request.type == OPENRSP_CMD_STOP && request.payload_bytes == 0u &&
               state->owner == descriptor && state->radio) {
        response.status = stop_stream(state) == 0 ? OPENRSP_STATUS_OK : OPENRSP_STATUS_IO_ERROR;
    } else if (request.type == OPENRSP_CMD_RELEASE && request.payload_bytes == 0u &&
               state->owner == descriptor) {
        release_client(state, descriptor);
        response.status = OPENRSP_STATUS_OK;
    } else if (request.type == OPENRSP_CMD_CONFIGURE &&
               request.payload_bytes == sizeof(openrsp_radio_config) &&
               state->owner == descriptor) {
        const openrsp_radio_config *config = (const openrsp_radio_config *)payload;
        bool opened_here = false;
        if (!state->radio) {
            if (mirisdr_open(&state->radio, state->acquired_device_index) != 0) {
                response.status = OPENRSP_STATUS_IO_ERROR;
                log_config("CONFIGURE", descriptor, 0u, response.status, config);
                goto send_response;
            }
            opened_here = true;
        }
        response.status = apply_config(state->radio, config);
        if (response.status == OPENRSP_STATUS_OK) {
            state->config = *config;
            state->configured = true;
        } else if (opened_here) {
            (void)mirisdr_close(state->radio);
            state->radio = NULL;
        }
        log_config("CONFIGURE", descriptor, 0u, response.status, config);
    } else if (request.type == OPENRSP_CMD_UPDATE &&
               request.payload_bytes == sizeof(openrsp_update_request) &&
               state->owner == descriptor) {
        const openrsp_update_request *update = (const openrsp_update_request *)payload;
        if (!valid_config(&update->config)) {
            response.status = OPENRSP_STATUS_BAD_REQUEST;
        } else if (!state->radio || !state->configured ||
                   atomic_load(&state->stream_error)) {
            /* Preserve the application's newest state while a physically
             * unplugged receiver is being reopened.  A transient update must
             * not make API clients permanently discard a recoverable tuner. */
            state->config = update->config;
            response.status = OPENRSP_STATUS_OK;
            response.changed_flags = OPENRSP_RESPONSE_RECOVERY_QUEUED;
        } else {
            response.status = apply_update(state, update);
            if (response.status == OPENRSP_STATUS_IO_ERROR) {
                state->config = update->config;
                atomic_store(&state->stream_error, true);
                if (state->radio) (void)mirisdr_cancel_async(state->radio);
                response.status = OPENRSP_STATUS_OK;
                response.changed_flags = OPENRSP_RESPONSE_RECOVERY_QUEUED;
                fprintf(stderr, "OPENRSPD_RECOVERY_QUEUED flags=%u\n",
                        update->changed_flags);
                (void)fflush(stderr);
            }
        }
        log_config("UPDATE", descriptor, update->changed_flags, response.status,
                   &update->config);
    } else {
        response.status = state->owner >= 0 && state->owner != descriptor ?
                          OPENRSP_STATUS_BUSY : OPENRSP_STATUS_UNSUPPORTED;
    }
send_response:
    if (write_control_frame(state, descriptor, OPENRSP_MSG_RESPONSE, request.sequence,
                            &response, sizeof(response)) != 0) return -1;
    if (request.type == OPENRSP_CMD_LIST && response.status == OPENRSP_STATUS_OK) {
        for (uint32_t index = 0; index < response.changed_flags; ++index) {
            openrsp_device_record record = {
                .device_index = index, .vendor_id = 0x1df7u, .product_id = 0x3020u
            };
            char manufacturer[256] = {0};
            char product[256] = {0};
            char serial[256] = {0};
            (void)mirisdr_get_device_usb_strings(index, manufacturer, product, serial);
            (void)snprintf(record.serial, sizeof(record.serial), "%s", serial);
            (void)snprintf(record.model, sizeof(record.model), "%s",
                           mirisdr_get_device_name(index));
            if (write_control_frame(state, descriptor, OPENRSP_EVENT_DEVICE,
                                    request.sequence, &record, sizeof(record)) != 0)
                return -1;
        }
    }
    if (request.type == OPENRSP_CMD_START && response.status == OPENRSP_STATUS_OK) {
        state->logged_usb_iq = false;
        state->logged_socket_iq = false;
        if (pthread_create(&state->stream_thread, NULL, stream_main, state) != 0) {
            state->streaming = false;
            return -1;
        }
        state->stream_thread_started = true;
    }
    return 1;
}

static void remove_client(daemon_state *state, int clients[OPENRSPD_MAX_CLIENTS],
                          size_t index)
{
    int descriptor = clients[index];
    if (descriptor < 0) return;
    release_client(state, descriptor);
    (void)close(descriptor);
    clients[index] = -1;
}

static void close_clients(daemon_state *state, int clients[OPENRSPD_MAX_CLIENTS])
{
    for (size_t index = 0u; index < OPENRSPD_MAX_CLIENTS; ++index)
        remove_client(state, clients, index);
}

static void accept_clients(int server, int clients[OPENRSPD_MAX_CLIENTS])
{
    for (;;) {
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) perror("accept");
            return;
        }
        /* macOS may propagate O_NONBLOCK from the listening socket.  Control
         * frames fit in one write, but IQ frames can otherwise stop at EAGAIN
         * after a partial payload and leave the receiver waiting forever. */
        if (set_blocking(client) != 0) {
            (void)close(client);
            continue;
        }
        bool stored = false;
        for (size_t index = 0u; index < OPENRSPD_MAX_CLIENTS; ++index) {
            if (clients[index] >= 0) continue;
            clients[index] = client;
            stored = true;
            break;
        }
        if (!stored) {
            fprintf(stderr, "OPENRSPD_REJECT fd=%d reason=too_many_clients\n", client);
            (void)close(client);
        }
    }
}

int main(void)
{
    const char *path = getenv("OPENRSPD_SOCKET");
    if (!path || path[0] == '\0') path = OPENRSP_SOCKET_PATH;
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        fprintf(stderr, "Socket path is too long: %s\n", path);
        return EXIT_FAILURE;
    }

    int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }
    struct sockaddr_un address = {0};
    address.sun_family = AF_UNIX;
    (void)strcpy(address.sun_path, path);
    (void)unlink(path);
    if (set_nonblocking(server) != 0 ||
        bind(server, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        chmod(path, 0666) != 0 || listen(server, 8) != 0) {
        perror("openrspd socket setup");
        (void)close(server);
        (void)unlink(path);
        return EXIT_FAILURE;
    }
    struct sigaction stop_action = {0};
    stop_action.sa_handler = stop_server;
    (void)sigemptyset(&stop_action.sa_mask);
    (void)sigaction(SIGINT, &stop_action, NULL);
    (void)sigaction(SIGTERM, &stop_action, NULL);
    (void)signal(SIGPIPE, SIG_IGN);
    printf("OPENRSPD_READY socket=%s protocol=%u\n", path, OPENRSP_PROTOCOL_VERSION);
    (void)fflush(stdout);

    daemon_state state = {
        .owner = -1,
        .write_lock = PTHREAD_MUTEX_INITIALIZER
    };
    atomic_init(&state.streaming, false);
    atomic_init(&state.stream_error, false);
    atomic_init(&state.stream_result, 0);
    atomic_init(&state.first_iq_seen, false);
    atomic_init(&state.control_write_pending, false);
    int clients[OPENRSPD_MAX_CLIENTS];
    for (size_t index = 0u; index < OPENRSPD_MAX_CLIENTS; ++index) clients[index] = -1;
    while (running) {
        recover_failed_stream(&state);
        finish_recovery_gain(&state);
        struct pollfd fds[OPENRSPD_MAX_CLIENTS + 1u];
        size_t client_index[OPENRSPD_MAX_CLIENTS + 1u];
        nfds_t count = 1u;
        fds[0].fd = server;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        client_index[0] = OPENRSPD_MAX_CLIENTS;
        for (size_t index = 0u; index < OPENRSPD_MAX_CLIENTS; ++index) {
            if (clients[index] < 0) continue;
            fds[count].fd = clients[index];
            fds[count].events = POLLIN | POLLHUP | POLLERR;
            fds[count].revents = 0;
            client_index[count] = index;
            ++count;
        }
        int ready = poll(fds, count, 1000);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        if (ready == 0) continue;
        if ((fds[0].revents & POLLIN) != 0) accept_clients(server, clients);
        for (nfds_t index = 1u; index < count; ++index) {
            if (fds[index].revents == 0) continue;
            size_t slot = client_index[index];
            if ((fds[index].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
                remove_client(&state, clients, slot);
                continue;
            }
            if ((fds[index].revents & POLLIN) == 0) continue;
            int result = serve_request(clients[slot], &state);
            if (result != 1) remove_client(&state, clients, slot);
        }
    }
    close_clients(&state, clients);
    shutdown_radio(&state);
    (void)close(server);
    (void)unlink(path);
    (void)pthread_mutex_destroy(&state.write_lock);
    return running ? EXIT_FAILURE : EXIT_SUCCESS;
}
