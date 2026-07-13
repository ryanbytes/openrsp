/* SPDX-License-Identifier: GPL-2.0-or-later */
/* OpenRSP is dedicated to Carolyn, with love. */
#define _POSIX_C_SOURCE 200809L
#include "openrsp/protocol.h"
#include "openrsp/identity.h"
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
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <fcntl.h>

#define OPENRSPD_MAX_PAYLOAD 4096u
#define OPENRSPD_MAX_CLIENTS 32
#define OPENRSPD_MAX_DEVICES 32u
#define OPENRSPD_STREAM_SLOT_BYTES (OPENRSP_MAX_IQ_SAMPLES * 4u)

typedef struct {
    mirisdr_dev_t *radio;
    int owner;
    int api_lock_owner;
    openrsp_acquire_request acquired_identity;
    atomic_bool streaming;
    bool stream_thread_started;
    atomic_bool stream_error;
    atomic_int stream_result;
    pthread_t stream_thread;
    pthread_mutex_t write_lock;
    atomic_bool control_write_pending;
    atomic_bool client_write_error;
    bool logged_usb_iq;
    bool logged_socket_iq;
    atomic_bool first_iq_seen;
    bool recovery_config_pending;
    int recovery_identity_status;
    uint32_t stream_sequence;
    bool configured;
    openrsp_radio_config config;
    bool bootstrap_configured;
    openrsp_radio_config bootstrap_config;
} daemon_state;

static volatile sig_atomic_t running = 1;

static uint32_t snapshot_sdrplay_devices(openrsp_device_record *records,
                                         size_t capacity);
static int resolve_acquired_device(daemon_state *state, uint32_t *device_index);

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
        int write_error = errno;
        if (!atomic_exchange(&state->client_write_error, true)) {
            fprintf(stderr, "OPENRSPD_SOCKET_IQ_ERROR errno=%d\n", write_error);
            (void)fflush(stderr);
        }
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

static void release_api_lock(daemon_state *state, int descriptor,
                             const char *reason)
{
    if (state->api_lock_owner != descriptor) return;
    state->api_lock_owner = -1;
    fprintf(stderr, "OPENRSPD_API_UNLOCK fd=%d reason=%s\n", descriptor, reason);
    (void)fflush(stderr);
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
           config->agc_setpoint_dbfs >= -60 && config->agc_setpoint_dbfs <= -20 &&
           (config->tuner == OPENRSP_TUNER_A || config->tuner == OPENRSP_TUNER_B);
}

static uint32_t apply_config(mirisdr_dev_t *radio, const openrsp_radio_config *config)
{
    if (!valid_config(config)) return OPENRSP_STATUS_BAD_REQUEST;
    /* Keep this order identical to the direct openrsp-iq path.  That path is
     * the hardware gate: it streams at 2.048 MS/s, while the former combined
     * configure helper left endpoint 0x81 stalled before the first callback. */
#define OPENRSPD_CONFIG_STEP(name, expression) do { \
        int step_result = (expression); \
        if (step_result != 0) { \
            fprintf(stderr, "OPENRSPD_CONFIGURE_STAGE stage=%s result=%d\n", \
                    (name), step_result); \
            (void)fflush(stderr); \
            return OPENRSP_STATUS_IO_ERROR; \
        } \
    } while (0)
    OPENRSPD_CONFIG_STEP("hw-flavour",
                        mirisdr_set_hw_flavour(radio, MIRISDR_HW_SDRPLAY));
    OPENRSPD_CONFIG_STEP("sample-rate",
                        mirisdr_set_sample_rate(radio, config->sample_rate_hz));
    OPENRSPD_CONFIG_STEP("center-frequency",
                        mirisdr_set_center_freq(radio, config->center_frequency_hz));
    OPENRSPD_CONFIG_STEP("sample-format", mirisdr_set_sample_format(radio, "AUTO"));
    OPENRSPD_CONFIG_STEP("transfer", mirisdr_set_transfer(radio, "BULK"));
    OPENRSPD_CONFIG_STEP("if-frequency",
                        mirisdr_set_if_freq(radio, (uint32_t)config->if_frequency_hz));
    OPENRSPD_CONFIG_STEP("bandwidth",
                        mirisdr_set_bandwidth(radio, config->bandwidth_hz));
    OPENRSPD_CONFIG_STEP("gain-mode", mirisdr_set_tuner_gain_mode(radio, 1));
    /* The direct hardware gate starts at this register state.  The API
     * applies the caller's requested GR/LNA setting after first IQ. */
    OPENRSPD_CONFIG_STEP("initial-gain", mirisdr_set_tuner_gain(radio, 102));
#undef OPENRSPD_CONFIG_STEP
    return OPENRSP_STATUS_OK;
}

static uint32_t configure_radio(daemon_state *state,
                                const openrsp_radio_config *config,
                                unsigned int max_attempts)
{
    if (!valid_config(config)) return OPENRSP_STATUS_BAD_REQUEST;
    if (max_attempts == 0u) max_attempts = 1u;

    for (unsigned int attempt = 1u; attempt <= max_attempts; ++attempt) {
        if (!state->radio) {
            uint32_t resolved_index = 0u;
            if (resolve_acquired_device(state, &resolved_index) != 0) {
                return OPENRSP_STATUS_BAD_REQUEST;
            }
            int open_result = mirisdr_open_tuner(&state->radio, resolved_index,
                                                 config->tuner);
            if (open_result != 0) {
                fprintf(stderr,
                        "OPENRSPD_CONFIGURE_STAGE stage=open result=%d attempt=%u/%u\n",
                        open_result, attempt, max_attempts);
                (void)fflush(stderr);
            }
        }

        uint32_t status = state->radio ? apply_config(state->radio, config) :
                                        OPENRSP_STATUS_IO_ERROR;
        if (status == OPENRSP_STATUS_OK) {
            if (attempt > 1u) {
                fprintf(stderr, "OPENRSPD_CONFIGURE_RETRY status=recovered attempt=%u/%u\n",
                        attempt, max_attempts);
                (void)fflush(stderr);
            }
            return status;
        }

        if (state->radio) {
            (void)mirisdr_close(state->radio);
            state->radio = NULL;
        }
        if (attempt < max_attempts) {
            fprintf(stderr, "OPENRSPD_CONFIGURE_RETRY status=waiting attempt=%u/%u\n",
                    attempt, max_attempts);
            (void)fflush(stderr);
            (void)usleep(250000u);
        }
    }
    return OPENRSP_STATUS_IO_ERROR;
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
    state->recovery_config_pending = false;
    if (state->owner < 0) return;
    uint32_t previous_index = state->acquired_identity.device_index;
    uint32_t resolved_index = 0u;
    int identity_result = resolve_acquired_device(state, &resolved_index);
    if (identity_result != 0) {
        if (state->recovery_identity_status != identity_result) {
            fprintf(stderr, "OPENRSPD_RECOVERY_IDENTITY status=%s\n",
                    identity_result == OPENRSP_IDENTITY_AMBIGUOUS ?
                    "ambiguous" : "not-found");
            (void)fflush(stderr);
            state->recovery_identity_status = identity_result;
        }
        return;
    }

    if (state->recovery_identity_status != 0) {
        fprintf(stderr, "OPENRSPD_RECOVERY_IDENTITY status=resolved index=%u\n",
                resolved_index);
        (void)fflush(stderr);
        state->recovery_identity_status = 0;
    }

    fprintf(stderr, "OPENRSPD_RECOVERY_REOPEN previous_index=%u index=%u\n",
            previous_index, resolved_index);
    (void)fflush(stderr);
    state->acquired_identity.device_index = resolved_index;
    /* A fresh application session starts the endpoint with its initial
     * configuration and only tunes to the active channel after first IQ.  On
     * the tested RSPduo, configuring a reopened frontend directly at the last
     * 800 MHz channel can produce bulk data that contains no decodable RF.
     * Replay the session's proven bootstrap state, then restore the newest
     * application state after the endpoint is demonstrably alive. */
    const openrsp_radio_config *bootstrap = state->bootstrap_configured ?
                                             &state->bootstrap_config :
                                             &state->config;
    uint32_t status = configure_radio(state, bootstrap, 3u);
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
    state->recovery_config_pending = true;
}

static void finish_recovery_config(daemon_state *state)
{
    if (!state->recovery_config_pending || !atomic_load(&state->first_iq_seen) ||
        !state->radio) return;
    const openrsp_radio_config *bootstrap = state->bootstrap_configured ?
                                             &state->bootstrap_config :
                                             &state->config;
    const openrsp_radio_config *target = &state->config;
    int result = 0;
    if (target->sample_rate_hz != bootstrap->sample_rate_hz)
        result |= mirisdr_set_sample_rate(state->radio, target->sample_rate_hz);
    if (target->center_frequency_hz != bootstrap->center_frequency_hz)
        result |= mirisdr_set_center_freq(state->radio, target->center_frequency_hz);
    if (target->bandwidth_hz != bootstrap->bandwidth_hz)
        result |= mirisdr_set_bandwidth(state->radio, target->bandwidth_hz);
    if (target->if_frequency_hz != bootstrap->if_frequency_hz)
        result |= mirisdr_set_if_freq(state->radio,
                                      (uint32_t)target->if_frequency_hz);
    result |= set_gain(state->radio, target);
    fprintf(stderr,
            "OPENRSPD_RECOVERY_CONFIG status=%d bootstrap_rf=%u fs=%u rf=%u bw=%u if=%d gr=%d lna=%u\n",
            result, bootstrap->center_frequency_hz, target->sample_rate_hz,
            target->center_frequency_hz, target->bandwidth_hz,
            target->if_frequency_hz, target->gain_reduction_db,
            target->lna_state);
    (void)fflush(stderr);
    state->recovery_config_pending = false;
    if (result != 0) {
        atomic_store(&state->stream_error, true);
        if (state->radio) (void)mirisdr_cancel_async(state->radio);
    }
}

static uint32_t apply_update(daemon_state *state, const openrsp_update_request *update)
{
    const openrsp_radio_config *next = &update->config;
    const openrsp_radio_config *old = &state->config;
    uint32_t flags = update->changed_flags;
    int result = 0;
    if (next->tuner != old->tuner) return OPENRSP_STATUS_BAD_REQUEST;
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

static uint32_t snapshot_sdrplay_devices(openrsp_device_record *records,
                                         size_t capacity)
{
    /* A physically replugged RSPduo first appears as the raw 1df7:3020
     * firmware loader with no serial descriptor.  Stable-serial matching is
     * impossible until the driver has loaded the locally installed firmware.
     * Bootstrap only devices whose descriptor explicitly identifies that
     * cold state; never guess from an index or from another receiver. */
    for (uint32_t pass = 0u; pass < OPENRSPD_MAX_DEVICES; ++pass) {
        uint32_t candidate_count = mirisdr_get_device_count();
        bool bootstrapped = false;
        for (uint32_t index = 0u; index < candidate_count; ++index) {
            if (mirisdr_device_requires_firmware(index) != 1) continue;
            fprintf(stderr, "OPENRSPD_COLD_BOOT index=%u status=starting\n", index);
            (void)fflush(stderr);
            mirisdr_dev_t *candidate = NULL;
            int open_result = mirisdr_open(&candidate, index);
            int close_result = candidate ? mirisdr_close(candidate) : 0;
            fprintf(stderr,
                    "OPENRSPD_COLD_BOOT index=%u status=%s open=%d close=%d\n",
                    index, open_result == 0 ? "ready" : "failed",
                    open_result, close_result);
            (void)fflush(stderr);
            if (open_result != 0) break;
            bootstrapped = true;
            break; /* Device indices can change after re-enumeration. */
        }
        if (!bootstrapped) break;
    }

    uint32_t raw_count = mirisdr_get_device_count();
    uint32_t count = 0u;
    for (uint32_t raw_index = 0u; raw_index < raw_count; ++raw_index) {
        uint16_t vendor_id = 0u;
        uint16_t product_id = 0u;
        char manufacturer[256] = {0};
        char product[256] = {0};
        char serial[256] = {0};
        if (mirisdr_get_device_info(raw_index, &vendor_id, &product_id,
                                    manufacturer, product, serial) != 0 ||
            vendor_id != 0x1df7u)
            continue;
        if (records && count >= capacity) break;
        if (records && count < capacity) {
            openrsp_device_record *record = &records[count];
            memset(record, 0, sizeof(*record));
            record->device_index = raw_index;
            record->vendor_id = vendor_id;
            record->product_id = product_id;
            (void)snprintf(record->serial, sizeof(record->serial), "%s", serial);
            (void)snprintf(record->model, sizeof(record->model), "%s", product);
        }
        ++count;
    }
    return count;
}

static int resolve_acquired_device(daemon_state *state, uint32_t *device_index)
{
    openrsp_device_record devices[OPENRSPD_MAX_DEVICES];
    uint32_t count = snapshot_sdrplay_devices(devices, OPENRSPD_MAX_DEVICES);
    int result = openrsp_identity_resolve(&state->acquired_identity, devices,
                                          count, device_index);
    if (result == 0) state->acquired_identity.device_index = *device_index;
    return result;
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

    openrsp_device_record listed_devices[OPENRSPD_MAX_DEVICES];
    uint32_t listed_count = 0u;
    openrsp_response response = {0};
    response.sequence = request.sequence;
    if (request.magic != OPENRSP_PROTOCOL_MAGIC || request.version != OPENRSP_PROTOCOL_VERSION) {
        response.status = OPENRSP_STATUS_BAD_REQUEST;
    } else if (request.type == OPENRSP_CMD_PING && request.payload_bytes == 0u) {
        response.status = OPENRSP_STATUS_OK;
    } else if (request.type == OPENRSP_CMD_LIST && request.payload_bytes == 0u) {
        response.status = OPENRSP_STATUS_OK;
        listed_count = snapshot_sdrplay_devices(listed_devices,
                                                OPENRSPD_MAX_DEVICES);
        response.changed_flags = listed_count;
    } else if (request.type == OPENRSP_CMD_LOCK_API && request.payload_bytes == 0u) {
        if (state->api_lock_owner >= 0 && state->api_lock_owner != descriptor) {
            response.status = OPENRSP_STATUS_BUSY;
        } else {
            state->api_lock_owner = descriptor;
            response.status = OPENRSP_STATUS_OK;
            fprintf(stderr, "OPENRSPD_API_LOCK fd=%d\n", descriptor);
            (void)fflush(stderr);
        }
    } else if (request.type == OPENRSP_CMD_UNLOCK_API && request.payload_bytes == 0u &&
               state->api_lock_owner == descriptor) {
        release_api_lock(state, descriptor, "request");
        response.status = OPENRSP_STATUS_OK;
    } else if (request.type == OPENRSP_CMD_ACQUIRE &&
               request.payload_bytes == sizeof(openrsp_acquire_request)) {
        const openrsp_acquire_request *acquire = (const openrsp_acquire_request *)payload;
        if (state->owner >= 0 && state->owner != descriptor) {
            response.status = OPENRSP_STATUS_BUSY;
        } else if (state->owner == descriptor) {
            response.status = OPENRSP_STATUS_OK;
        } else {
            openrsp_device_record devices[OPENRSPD_MAX_DEVICES];
            uint32_t count = snapshot_sdrplay_devices(devices, OPENRSPD_MAX_DEVICES);
            uint32_t resolved_index = 0u;
            int identity_result = openrsp_identity_resolve(acquire, devices, count,
                                                           &resolved_index);
            if (identity_result != 0) {
                fprintf(stderr, "OPENRSPD_ACQUIRE_IDENTITY status=%s\n",
                        identity_result == OPENRSP_IDENTITY_AMBIGUOUS ?
                        "ambiguous" : "not-found");
                (void)fflush(stderr);
                response.status = OPENRSP_STATUS_BAD_REQUEST;
            } else {
                const openrsp_device_record *selected = NULL;
                for (uint32_t index = 0u; index < count; ++index) {
                    if (devices[index].device_index == resolved_index) {
                        selected = &devices[index];
                        break;
                    }
                }
                if (state->radio &&
                    (!selected || !openrsp_identity_matches(&state->acquired_identity,
                                                             selected))) {
                    (void)mirisdr_close(state->radio);
                    state->radio = NULL;
                    state->configured = false;
                }
                state->owner = descriptor;
                state->acquired_identity = *acquire;
                state->acquired_identity.device_index = resolved_index;
                response.status = OPENRSP_STATUS_OK;
            }
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
        response.status = configure_radio(state, config, 3u);
        if (response.status == OPENRSP_STATUS_OK) {
            state->config = *config;
            state->bootstrap_config = *config;
            state->bootstrap_configured = true;
            state->configured = true;
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
    if (write_control_frame(state, descriptor, OPENRSP_MSG_RESPONSE, request.sequence,
                            &response, sizeof(response)) != 0) return -1;
    if (request.type == OPENRSP_CMD_LIST && response.status == OPENRSP_STATUS_OK) {
        for (uint32_t index = 0; index < listed_count; ++index) {
            if (write_control_frame(state, descriptor, OPENRSP_EVENT_DEVICE,
                                    request.sequence, &listed_devices[index],
                                    sizeof(listed_devices[index])) != 0)
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
    release_api_lock(state, descriptor, "disconnect");
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
        /* A client can stop draining IQ after a device-removal callback while
         * leaving its socket open.  Never let that wedge the USB callback and
         * libusb event lock indefinitely. */
        const struct timeval send_timeout = {.tv_sec = 2, .tv_usec = 0};
        if (setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
                       sizeof(send_timeout)) != 0) {
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
        .api_lock_owner = -1,
        .write_lock = PTHREAD_MUTEX_INITIALIZER
    };
    atomic_init(&state.streaming, false);
    atomic_init(&state.stream_error, false);
    atomic_init(&state.stream_result, 0);
    atomic_init(&state.first_iq_seen, false);
    atomic_init(&state.control_write_pending, false);
    atomic_init(&state.client_write_error, false);
    int clients[OPENRSPD_MAX_CLIENTS];
    for (size_t index = 0u; index < OPENRSPD_MAX_CLIENTS; ++index) clients[index] = -1;
    while (running) {
        if (atomic_exchange(&state.client_write_error, false)) {
            for (size_t index = 0u; index < OPENRSPD_MAX_CLIENTS; ++index) {
                if (clients[index] == state.owner) {
                    fprintf(stderr, "OPENRSPD_CLIENT_EVICT fd=%d reason=iq-write-timeout\n",
                            state.owner);
                    (void)fflush(stderr);
                    remove_client(&state, clients, index);
                    break;
                }
            }
            continue;
        }
        recover_failed_stream(&state);
        finish_recovery_config(&state);
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
        /* Ownership cleanup has priority over new commands. A dead lock owner
         * and a waiting contender can become ready in the same poll cycle;
         * serving by descriptor-slot order would otherwise return a spurious
         * BUSY before processing the owner's hangup. */
        for (nfds_t index = 1u; index < count; ++index) {
            size_t slot = client_index[index];
            if ((fds[index].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
                remove_client(&state, clients, slot);
            }
        }
        for (nfds_t index = 1u; index < count; ++index) {
            size_t slot = client_index[index];
            if (clients[slot] < 0) continue;
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
