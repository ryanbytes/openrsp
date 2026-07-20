/* SPDX-License-Identifier: GPL-2.0-or-later */
/* OpenRSP is dedicated to Carolyn, with love. */
#define _POSIX_C_SOURCE 200809L
#include "openrsp/protocol.h"
#include "openrsp/identity.h"
#include "openrsp/socket_compat.h"
#include "config_validation.h"
#include "removal_tracker.h"
#include "duo_session.h"
#include "windows_device_recovery.h"
#include "mirisdr.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#if !defined(_WIN32)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif
#include <pthread.h>
#if defined(_WIN32)
#include <windows.h>
#endif

#define OPENRSPD_MAX_PAYLOAD 4096u
#define OPENRSPD_MAX_CLIENTS 32
#define OPENRSPD_MAX_DEVICES 32u
#define OPENRSPD_STREAM_SLOT_BYTES (OPENRSP_MAX_IQ_SAMPLES * 4u)
#define OPENRSPD_DEVICE_SNAPSHOT_CACHE_NANOSECONDS 1000000000L
#define OPENRSPD_DEVICE_REMOVAL_GRACE_MILLISECONDS 5000u

typedef struct {
    mirisdr_dev_t *radio;
    openrsp_socket_t owner;
    openrsp_socket_t api_lock_owner;
    openrsp_acquire_request acquired_identity;
    atomic_bool streaming;
    bool stream_thread_started;
    atomic_bool stream_error;
    atomic_int stream_result;
    pthread_t stream_thread;
    pthread_mutex_t write_lock;
    atomic_bool control_write_pending;
    atomic_bool client_write_error;
    atomic_intptr_t client_write_error_fd;
    bool logged_usb_iq;
    bool logged_socket_iq;
    atomic_bool first_iq_seen;
    bool recovery_config_pending;
    bool mode_swap_paused;
    int recovery_identity_status;
    uint32_t stream_sequence[2];
    bool configured;
    openrsp_radio_config config;
    bool dual_mode;
    openrsp_dual_config dual_config;
    bool bootstrap_configured;
    openrsp_radio_config bootstrap_config;
    bool device_snapshot_valid;
    struct timespec device_snapshot_time;
    uint32_t device_snapshot_count;
    openrsp_device_record device_snapshot[OPENRSPD_MAX_DEVICES];
    openrspd_removal_tracker removal_tracker;
    openrspd_duo_session duo;
    bool duo_active;
    bool duo_config_a;
    bool duo_config_b;
    openrsp_dual_config duo_config;
} daemon_state;

static volatile sig_atomic_t running = 1;

#if defined(_WIN32)
static SERVICE_STATUS_HANDLE service_status_handle;
static SERVICE_STATUS service_status;
static int service_mode;

static void report_service_status(DWORD state, DWORD exit_code,
                                  DWORD wait_hint)
{
    if (!service_status_handle) return;
    service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    service_status.dwCurrentState = state;
    service_status.dwControlsAccepted =
        state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0u;
    service_status.dwWin32ExitCode = exit_code;
    service_status.dwServiceSpecificExitCode = 0u;
    service_status.dwCheckPoint = 0u;
    service_status.dwWaitHint = wait_hint;
    (void)SetServiceStatus(service_status_handle, &service_status);
}

static void WINAPI service_control(DWORD control)
{
    if (control != SERVICE_CONTROL_STOP && control != SERVICE_CONTROL_SHUTDOWN)
        return;
    report_service_status(SERVICE_STOP_PENDING, NO_ERROR, 2000u);
    running = 0;
}
#endif

static unsigned long long wall_clock_milliseconds(void)
{
    struct timeval now;
    if (gettimeofday(&now, NULL) != 0) return 0u;
    return (unsigned long long)now.tv_sec * 1000u +
           (unsigned long long)now.tv_usec / 1000u;
}

static uint64_t monotonic_milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0u;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static void sleep_milliseconds(uint32_t milliseconds)
{
    const struct timespec delay = {
        .tv_sec = (time_t)(milliseconds / 1000u),
        .tv_nsec = (long)(milliseconds % 1000u) * 1000000L
    };
    (void)nanosleep(&delay, NULL);
}

static uint32_t snapshot_sdrplay_devices(daemon_state *state,
                                         openrsp_device_record *records,
                                         size_t capacity);
static int resolve_acquired_device(daemon_state *state, uint32_t *device_index);

static void stop_server(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static int read_exact(openrsp_socket_t descriptor, void *buffer, size_t bytes)
{
    unsigned char *cursor = buffer;
    while (bytes != 0u) {
        ptrdiff_t count = openrsp_socket_read(descriptor, cursor, bytes);
        if (count == 0) return 0;
        if (count < 0) {
            if (openrsp_socket_interrupted(openrsp_socket_last_error())) continue;
            return -1;
        }
        cursor += (size_t)count;
        bytes -= (size_t)count;
    }
    return 1;
}

static int write_exact(openrsp_socket_t descriptor, const void *buffer,
                       size_t bytes)
{
    const unsigned char *cursor = buffer;
    while (bytes != 0u) {
        ptrdiff_t count = openrsp_socket_write(descriptor, cursor, bytes);
        if (count < 0) {
            if (openrsp_socket_interrupted(openrsp_socket_last_error())) continue;
            return -1;
        }
        cursor += (size_t)count;
        bytes -= (size_t)count;
    }
    return 0;
}

static int set_nonblocking(openrsp_socket_t descriptor)
{
    return openrsp_socket_set_blocking(descriptor, 0);
}

static int set_blocking(openrsp_socket_t descriptor)
{
    return openrsp_socket_set_blocking(descriptor, 1);
}

static int write_frame(daemon_state *state, openrsp_socket_t descriptor,
                       uint16_t type,
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

static int write_control_frame(daemon_state *state, openrsp_socket_t descriptor,
                               uint16_t type,
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

static int send_duo_event(daemon_state *state, const openrspd_duo_event *event)
{
    if (state == NULL || event == NULL || event->kind == OPENRSPD_DUO_EVENT_NONE ||
        event->target_fd < 0)
        return 0;
    openrsp_duo_event payload = {
        .kind = (uint32_t)event->kind,
        .tuner = event->tuner
    };
    return write_control_frame(state, event->target_fd, OPENRSP_EVENT_DUO,
                               0u, &payload, sizeof(payload));
}

static void send_stream(daemon_state *state, unsigned int tuner,
                        unsigned char *buffer, uint32_t length)
{
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
    unsigned int slot = tuner == OPENRSP_TUNER_B ? 1u : 0u;
    uint32_t sequence = ++state->stream_sequence[slot];
    uint16_t event = tuner == OPENRSP_TUNER_B ? OPENRSP_EVENT_IQ_B : OPENRSP_EVENT_IQ;
    openrspd_client_id target = state->duo_active ? OPENRSPD_INVALID_CLIENT :
                                                   state->owner;
    if (state->duo_active)
        (void)openrspd_duo_route(&state->duo, tuner, &target);
    if (target < 0) return;
    if (write_frame(state, target, event, sequence,
                    buffer, length) != 0) {
        int write_error = openrsp_socket_last_error();
        if (!atomic_exchange(&state->client_write_error, true)) {
            atomic_store(&state->client_write_error_fd, target);
            fprintf(stderr, "OPENRSPD_SOCKET_IQ_ERROR error=%d fd=%lld\n",
                    write_error, (long long)target);
            (void)fflush(stderr);
        }
        /* A slave owns only its socket route.  Cancelling the shared USB
         * stream here also kills a healthy master and can turn normal peer
         * loss into a WinUSB endpoint stall.  Eviction clears the slave route
         * on the main loop; master and single-owner failures still stop USB. */
        if (state->radio &&
            (!state->duo_active ||
             openrspd_duo_write_failure_stops_stream(&state->duo, target)))
            (void)mirisdr_cancel_async(state->radio);
        return;
    }
    if (!state->logged_socket_iq) {
        state->logged_socket_iq = true;
        fprintf(stderr, "OPENRSPD_SOCKET_IQ_FIRST bytes=%u sequence=%u\n",
                length, sequence);
        (void)fflush(stderr);
    }
}

static void stream_callback(unsigned char *buffer, uint32_t length, void *opaque)
{
    send_stream(opaque, OPENRSP_TUNER_A, buffer, length);
}

static void dual_stream_callback(unsigned int tuner, unsigned char *buffer,
                                 uint32_t length, void *opaque)
{
    send_stream(opaque, tuner, buffer, length);
}

static void *stream_main(void *opaque)
{
    daemon_state *state = opaque;
#if defined(_WIN32)
    const uint32_t usb_queue_depth = 8u;
    const uint32_t stream_frame_bytes = OPENRSPD_STREAM_SLOT_BYTES;
#else
    const uint32_t usb_queue_depth = 32u;
    const uint32_t stream_frame_bytes = 65536u;
#endif
    fprintf(stderr, "OPENRSPD_STREAM_THREAD_START\n");
    (void)fflush(stderr);
    /* WinUSB reliably drains eight outstanding requests. Use larger loopback
     * frames there to reduce socket backpressure without increasing USB
     * ownership. macOS retains its proven 32-request, 64 KiB path. */
    int result = state->dual_mode ?
        mirisdr_read_async_dual(state->radio, dual_stream_callback, state,
                                usb_queue_depth) :
        mirisdr_read_async(state->radio, stream_callback, state,
                           usb_queue_depth, stream_frame_bytes);
    state->streaming = false;
    state->stream_result = result;
    state->stream_error = result != 0;
    if (result != 0) {
        fprintf(stderr, "RSP stream stopped with error %d time_unix_ms=%llu\n",
                result, wall_clock_milliseconds());
        (void)fflush(stderr);
    }
    return NULL;
}

static int set_gain(mirisdr_dev_t *radio, const openrsp_radio_config *config)
{
    /* API 3.15.1 controls IF gain and the external LNA GPIOs separately in
     * all four RSPduo RF bands. */
    return mirisdr_set_rspduo_gain(radio, config->gain_reduction_db,
                                   config->lna_state);
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
        {OPENRSP_CHANGE_AGC, "AGC"},
        {OPENRSP_CHANGE_BIAS_TEE, "BIAS"},
        {OPENRSP_CHANGE_RF_NOTCH, "RF_NOTCH"},
        {OPENRSP_CHANGE_DAB_NOTCH, "DAB_NOTCH"},
        {OPENRSP_CHANGE_EXT_REF, "EXT_REF"},
        {OPENRSP_CHANGE_AM_PORT, "AM_PORT"},
        {OPENRSP_CHANGE_AM_NOTCH, "AM_NOTCH"}
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

static void log_config(const char *operation, openrsp_socket_t descriptor,
                       uint32_t flags,
                       uint32_t status, const openrsp_radio_config *config)
{
    char flag_text[64];
    describe_flags(flags, flag_text, sizeof(flag_text));
    fprintf(stderr,
            "OPENRSPD_%s fd=%lld status=%u flags=%s fs=%u rf=%u bw=%u if=%d gr=%d lna=%u agc=%d setpoint=%d bias=%u rf_notch=%u dab_notch=%u ext_ref=%u am_port=%u am_notch=%u\n",
            operation, (long long)descriptor, status, flag_text,
            config->sample_rate_hz,
            config->center_frequency_hz, config->bandwidth_hz,
            config->if_frequency_hz, config->gain_reduction_db,
            config->lna_state, config->agc_mode, config->agc_setpoint_dbfs,
            config->bias_tee_enabled, config->rf_notch_enabled,
            config->dab_notch_enabled, config->external_reference_enabled,
            config->am_port_select, config->am_notch_enabled);
    (void)fflush(stderr);
}

static int stop_stream(daemon_state *state)
{
    if (!state->stream_thread_started) return 0;
    int result = 0;
#if defined(_WIN32)
    /* The async event thread stops the RSPduo bulk engine, then cancels and
     * drains WinUSB transfers. Keeping that complete sequence on one thread
     * avoids concurrent libusb event handling and leaves the endpoint idle. */
    if (state->radio) (void)mirisdr_cancel_async(state->radio);
    if (pthread_join(state->stream_thread, NULL) != 0) result = -1;
    if (atomic_load(&state->stream_result) != 0) result = -1;
#else
    /* Stop the RSPduo endpoint while libusb still owns its submitted
     * transfers, then cancel and drain every completion before freeing them.
     * Reversing this order leaves the device-side bulk engine active during
     * teardown and the next session can fail its initial submits with PIPE. */
    if (state->radio && mirisdr_streaming_stop(state->radio) < 0) result = -1;
    if (state->radio) (void)mirisdr_cancel_async(state->radio);
    if (pthread_join(state->stream_thread, NULL) != 0) result = -1;
    if (atomic_load(&state->stream_result) != 0) result = -1;
#endif
    state->stream_thread_started = false;
    state->streaming = false;
    return result;
}

static int close_radio(daemon_state *state, const char *reason)
{
    if (!state || !state->radio) return 0;
    int result = mirisdr_close(state->radio);
    state->radio = NULL;
    if (result != 0) {
        fprintf(stderr, "OPENRSPD_CLOSE_FAILED result=%d reason=%s\n",
                result, reason ? reason : "unknown");
        (void)fflush(stderr);
    }
    /* -EBUSY means libusb still owns asynchronous transfer storage.  Do not
     * continue in a process that has deliberately quarantined that handle;
     * process teardown lets the OS reclaim it without a use-after-free. */
    if (result == -EBUSY) running = 0;
    return result;
}

static void release_client(daemon_state *state, openrsp_socket_t descriptor)
{
    if (state->duo_active &&
        openrspd_duo_role_for_descriptor(&state->duo, descriptor) !=
            OPENRSPD_DUO_ROLE_NONE) {
        openrspd_duo_event event;
        (void)openrspd_duo_disconnect(&state->duo, descriptor, &event);
        (void)send_duo_event(state, &event);
        if (!state->duo.master_selected && !state->duo.slave_selected) {
            if (state->radio) (void)stop_stream(state);
            (void)close_radio(state, "duo-disconnect");
            state->configured = false;
            state->dual_mode = false;
            state->duo_active = false;
            state->duo_config_a = false;
            state->duo_config_b = false;
            state->device_snapshot_valid = false;
        }
        return;
    }
    if (state->owner != descriptor) return;
    if (state->radio) (void)stop_stream(state);
    if (state->dual_mode && state->radio) {
        (void)close_radio(state, "dual-release");
        state->configured = false;
    }
    if (state->stream_error && state->radio) {
        fprintf(stderr, "OPENRSPD_CLOSE_FAILED_STREAM result=%d\n", state->stream_result);
        (void)fflush(stderr);
        (void)close_radio(state, "failed-stream-release");
        state->configured = false;
    }
    state->owner = -1;
    state->streaming = false;
    state->stream_error = false;
    state->stream_result = 0;
    state->dual_mode = false;
    state->mode_swap_paused = false;
    openrspd_removal_tracker_present(&state->removal_tracker);
}

static void release_api_lock(daemon_state *state, openrsp_socket_t descriptor,
                             const char *reason)
{
    if (state->api_lock_owner != descriptor) return;
    state->api_lock_owner = -1;
    fprintf(stderr, "OPENRSPD_API_UNLOCK fd=%lld reason=%s\n",
            (long long)descriptor, reason);
    (void)fflush(stderr);
}

static void shutdown_radio(daemon_state *state)
{
    if (!state->radio) return;
    (void)stop_stream(state);
    (void)close_radio(state, "daemon-shutdown");
    state->owner = -1;
    state->configured = false;
    state->stream_error = false;
    state->stream_result = 0;
}

static unsigned int rspduo_control_flags(uint32_t flags)
{
    unsigned int result = 0u;
    if ((flags & OPENRSP_CHANGE_BIAS_TEE) != 0u)
        result |= MIRISDR_RSPDUO_CHANGE_BIAS_TEE;
    if ((flags & OPENRSP_CHANGE_RF_NOTCH) != 0u)
        result |= MIRISDR_RSPDUO_CHANGE_RF_NOTCH;
    if ((flags & OPENRSP_CHANGE_DAB_NOTCH) != 0u)
        result |= MIRISDR_RSPDUO_CHANGE_DAB_NOTCH;
    if ((flags & OPENRSP_CHANGE_EXT_REF) != 0u)
        result |= MIRISDR_RSPDUO_CHANGE_EXT_REF;
    if ((flags & OPENRSP_CHANGE_AM_PORT) != 0u)
        result |= MIRISDR_RSPDUO_CHANGE_AM_PORT;
    if ((flags & OPENRSP_CHANGE_AM_NOTCH) != 0u)
        result |= MIRISDR_RSPDUO_CHANGE_AM_NOTCH;
    return result;
}

static int set_rspduo_controls(mirisdr_dev_t *radio,
                               const openrsp_radio_config *config,
                               uint32_t flags)
{
    if (config->tuner == OPENRSP_TUNER_B)
        flags &= ~(OPENRSP_CHANGE_AM_PORT | OPENRSP_CHANGE_AM_NOTCH);
    unsigned int control_flags = rspduo_control_flags(flags);
    if (control_flags == 0u) return 0;
    return mirisdr_set_rspduo_controls(
        radio, config->tuner, config->bias_tee_enabled,
        config->rf_notch_enabled, config->dab_notch_enabled,
        config->external_reference_enabled,
        config->am_port_select != 0u ? 1u : 2u,
        config->am_notch_enabled, control_flags);
}

static int set_rspduo_sample_rate(mirisdr_dev_t *radio,
                                  const openrsp_radio_config *config)
{
#if defined(_WIN32)
    /* A stopped WinUSB session does not reliably latch a new converter clock
     * from the two PLL writes alone. Reuse the captured cold-config sequence,
     * including its format/ADC setup and stream pulses, then restore controls. */
    int result = mirisdr_configure_rspduo(
        radio, config->sample_rate_hz, config->center_frequency_hz,
        (uint32_t)config->if_frequency_hz, config->bandwidth_hz,
        config->gain_reduction_db, config->lna_state);
    if (result == 0)
        result = set_rspduo_controls(radio, config,
                                     OPENRSP_CHANGE_RSPDUO_CONTROLS);
    return result;
#else
    return mirisdr_set_sample_rate(radio, config->sample_rate_hz);
#endif
}

static uint32_t apply_config(mirisdr_dev_t *radio, const openrsp_radio_config *config)
{
    if (!openrspd_config_valid(config)) return OPENRSP_STATUS_BAD_REQUEST;
#define OPENRSPD_CONFIG_STEP(name, expression) do { \
        int step_result = (expression); \
        if (step_result != 0) { \
            fprintf(stderr, "OPENRSPD_CONFIGURE_STAGE stage=%s result=%d\n", \
                    (name), step_result); \
            (void)fflush(stderr); \
            return OPENRSP_STATUS_IO_ERROR; \
        } \
    } while (0)
    OPENRSPD_CONFIG_STEP(
        "rspduo",
        mirisdr_configure_rspduo(radio, config->sample_rate_hz,
                                 config->center_frequency_hz,
                                 (uint32_t)config->if_frequency_hz,
                                 config->bandwidth_hz,
                                 config->gain_reduction_db,
                                 config->lna_state));
#undef OPENRSPD_CONFIG_STEP
    return OPENRSP_STATUS_OK;
}

static uint32_t configure_radio(daemon_state *state,
                                const openrsp_radio_config *config,
                                unsigned int max_attempts)
{
    if (!openrspd_config_valid(config)) return OPENRSP_STATUS_BAD_REQUEST;
    if (state->dual_mode && state->radio) {
        if (close_radio(state, "single-config-transition") == -EBUSY)
            return OPENRSP_STATUS_IO_ERROR;
        state->configured = false;
    }
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
            state->dual_mode = false;
            if (attempt > 1u) {
                fprintf(stderr, "OPENRSPD_CONFIGURE_RETRY status=recovered attempt=%u/%u\n",
                        attempt, max_attempts);
                (void)fflush(stderr);
            }
            return status;
        }

        if (state->radio) {
            if (close_radio(state, "single-config-failure") == -EBUSY)
                return OPENRSP_STATUS_IO_ERROR;
        }
        if (attempt < max_attempts) {
            fprintf(stderr, "OPENRSPD_CONFIGURE_RETRY status=waiting attempt=%u/%u\n",
                    attempt, max_attempts);
            (void)fflush(stderr);
            sleep_milliseconds(250u);
        }
    }
    return OPENRSP_STATUS_IO_ERROR;
}

static bool valid_dual_config(const openrsp_dual_config *config)
{
    if (!config || (config->sample_rate_hz != 6000000u &&
                    config->sample_rate_hz != 8000000u)) return false;
    int32_t required_if = config->sample_rate_hz == 6000000u ? 1620000 : 2048000;
    return openrspd_config_valid(&config->channel_a) &&
           openrspd_config_valid(&config->channel_b) &&
           config->channel_a.tuner == OPENRSP_TUNER_A &&
           config->channel_b.tuner == OPENRSP_TUNER_B &&
           config->channel_a.sample_rate_hz == config->sample_rate_hz &&
           config->channel_b.sample_rate_hz == config->sample_rate_hz &&
           config->channel_a.if_frequency_hz == required_if &&
           config->channel_b.if_frequency_hz == required_if &&
           config->channel_a.bandwidth_hz <= 1536000u &&
           config->channel_b.bandwidth_hz <= 1536000u;
}

static uint32_t configure_dual_radio(daemon_state *state,
                                     const openrsp_dual_config *config)
{
    if (!valid_dual_config(config)) return OPENRSP_STATUS_BAD_REQUEST;
    if (!state->dual_mode && state->radio) {
        if (close_radio(state, "dual-config-transition") == -EBUSY)
            return OPENRSP_STATUS_IO_ERROR;
        state->configured = false;
    }
    if (!state->radio) {
        uint32_t resolved_index = 0u;
        if (resolve_acquired_device(state, &resolved_index) != 0 ||
            mirisdr_open_tuner(&state->radio, resolved_index,
                               OPENRSP_TUNER_BOTH) != 0)
            return OPENRSP_STATUS_IO_ERROR;
    }
    int result = mirisdr_configure_rspduo_dual(
        state->radio, config->sample_rate_hz,
        config->channel_a.center_frequency_hz,
        config->channel_b.center_frequency_hz,
        (uint32_t)config->channel_a.if_frequency_hz,
        config->channel_a.bandwidth_hz,
        config->channel_a.gain_reduction_db, config->channel_a.lna_state,
        config->channel_b.gain_reduction_db, config->channel_b.lna_state);
    if (result == 0)
        result |= set_rspduo_controls(state->radio, &config->channel_a,
                                      OPENRSP_CHANGE_RSPDUO_CONTROLS);
    if (result == 0)
        result |= set_rspduo_controls(state->radio, &config->channel_b,
                                      OPENRSP_CHANGE_RSPDUO_CONTROLS);
    if (result < 0) {
        (void)close_radio(state, "dual-config-failure");
        return OPENRSP_STATUS_IO_ERROR;
    }
    state->dual_mode = true;
    state->dual_config = *config;
    state->configured = true;
    return OPENRSP_STATUS_OK;
}

static void report_removed_device(daemon_state *state)
{
    if (state->duo_active) {
        const openrsp_socket_t descriptors[2] = {
            state->duo.master_fd, state->duo.slave_fd
        };
        const uint32_t tuners[2] = {state->duo.master_tuner,
                                    state->duo.slave_tuner};
        for (unsigned int index = 0u; index < 2u; ++index) {
            if (descriptors[index] < 0) continue;
            openrsp_device_status status = {
                .reason = OPENRSP_DEVICE_STATUS_REMOVED,
                .tuner = tuners[index]
            };
            if (write_control_frame(state, descriptors[index],
                                    OPENRSP_EVENT_STATUS, 0u,
                                    &status, sizeof(status)) != 0) {
                atomic_store(&state->client_write_error_fd,
                             descriptors[index]);
                atomic_store(&state->client_write_error, true);
            }
        }
        return;
    }
    if (state->owner >= 0) {
        openrsp_device_status status = {
            .reason = OPENRSP_DEVICE_STATUS_REMOVED,
            .tuner = state->dual_mode ? OPENRSP_TUNER_BOTH : state->config.tuner
        };
        if (write_control_frame(state, state->owner, OPENRSP_EVENT_STATUS, 0u,
                                &status, sizeof(status)) != 0) {
            atomic_store(&state->client_write_error_fd, state->owner);
            atomic_store(&state->client_write_error, true);
        }
    }
}

static void recover_failed_stream(daemon_state *state)
{
    if (!atomic_load(&state->stream_error) || atomic_load(&state->streaming)) return;

    if (state->stream_thread_started) {
        (void)pthread_join(state->stream_thread, NULL);
        state->stream_thread_started = false;
    }
    if (state->radio) {
        if (close_radio(state, "stream-recovery") == -EBUSY) return;
    }
    state->configured = false;
    state->recovery_config_pending = false;
    if (state->owner < 0 &&
        !(state->duo_active && state->duo.master_selected)) return;
    uint32_t previous_index = state->acquired_identity.device_index;
    uint32_t resolved_index = 0u;
    /* A cached pre-failure snapshot can falsely report the unplugged receiver
     * as present for up to one second.  Recovery classification always uses a
     * fresh USB inventory. */
    state->device_snapshot_valid = false;
    int identity_result = resolve_acquired_device(state, &resolved_index);
    if (identity_result != 0) {
        if (identity_result == OPENRSP_IDENTITY_NOT_FOUND) {
            uint64_t now_ms = monotonic_milliseconds();
            if (openrspd_removal_tracker_absent(
                    &state->removal_tracker, now_ms,
                    OPENRSPD_DEVICE_REMOVAL_GRACE_MILLISECONDS)) {
                report_removed_device(state);
            }
        } else {
            /* Ambiguous identity is not proof of physical removal. */
            openrspd_removal_tracker_present(&state->removal_tracker);
        }
        if (state->recovery_identity_status != identity_result) {
            fprintf(stderr,
                    "OPENRSPD_RECOVERY_IDENTITY status=%s time_unix_ms=%llu\n",
                    identity_result == OPENRSP_IDENTITY_AMBIGUOUS ?
                    "ambiguous" : "not-found", wall_clock_milliseconds());
            (void)fflush(stderr);
            state->recovery_identity_status = identity_result;
        }
        return;
    }

    openrspd_removal_tracker_present(&state->removal_tracker);

    if (state->recovery_identity_status != 0) {
        fprintf(stderr,
                "OPENRSPD_RECOVERY_IDENTITY status=resolved index=%u time_unix_ms=%llu\n",
                resolved_index, wall_clock_milliseconds());
        (void)fflush(stderr);
        state->recovery_identity_status = 0;
    }

    fprintf(stderr,
            "OPENRSPD_RECOVERY_REOPEN previous_index=%u index=%u time_unix_ms=%llu\n",
            previous_index, resolved_index, wall_clock_milliseconds());
    (void)fflush(stderr);
    state->acquired_identity.device_index = resolved_index;
    /* A fresh application session starts the endpoint with its initial
     * configuration and only tunes to the active channel after first IQ.  On
     * the tested RSPduo, configuring a reopened frontend directly at the last
     * 800 MHz channel can produce bulk data that contains no decodable RF.
     * Replay the session's proven bootstrap state, then restore the newest
     * application state after the endpoint is demonstrably alive. */
    if (state->dual_mode) {
        uint32_t dual_status = configure_dual_radio(state, &state->dual_config);
        if (dual_status != OPENRSP_STATUS_OK) return;
        state->logged_usb_iq = false;
        state->logged_socket_iq = false;
        atomic_store(&state->first_iq_seen, false);
        atomic_store(&state->stream_error, false);
        atomic_store(&state->stream_result, 0);
        atomic_store(&state->streaming, true);
        if (pthread_create(&state->stream_thread, NULL, stream_main, state) != 0) {
            atomic_store(&state->streaming, false);
            atomic_store(&state->stream_error, true);
            return;
        }
        state->stream_thread_started = true;
        return;
    }
    const openrsp_radio_config *bootstrap = state->bootstrap_configured ?
                                             &state->bootstrap_config :
                                             &state->config;
    uint32_t status = configure_radio(state, bootstrap, 3u);
    if (status != OPENRSP_STATUS_OK) {
        (void)close_radio(state, "recovery-config-failure");
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
        (void)close_radio(state, "recovery-thread-failure");
        return;
    }
    state->stream_thread_started = true;
    state->recovery_config_pending = true;
}

static void finish_recovery_config(daemon_state *state)
{
    if (state->dual_mode) return;
    if (!state->recovery_config_pending || !atomic_load(&state->first_iq_seen) ||
        !state->radio) return;
    const openrsp_radio_config *bootstrap = state->bootstrap_configured ?
                                             &state->bootstrap_config :
                                             &state->config;
    const openrsp_radio_config *target = &state->config;
    int result = 0;
#if defined(_WIN32)
    bool restart_for_sample_rate =
        state->stream_thread_started &&
        target->sample_rate_hz != bootstrap->sample_rate_hz;
    if (restart_for_sample_rate) result = stop_stream(state);
#else
    const bool restart_for_sample_rate = false;
#endif
    if (result == 0 && target->sample_rate_hz != bootstrap->sample_rate_hz)
        result |= set_rspduo_sample_rate(state->radio, target);
    if (result == 0 &&
        target->center_frequency_hz != bootstrap->center_frequency_hz)
        result |= mirisdr_set_center_freq(state->radio, target->center_frequency_hz);
    if (result == 0 && target->bandwidth_hz != bootstrap->bandwidth_hz)
        result |= mirisdr_set_bandwidth(state->radio, target->bandwidth_hz);
    if (result == 0 && target->if_frequency_hz != bootstrap->if_frequency_hz)
        result |= mirisdr_set_if_freq(state->radio,
                                      (uint32_t)target->if_frequency_hz);
    if (result == 0) result |= set_gain(state->radio, target);
    if (result == 0)
        result |= set_rspduo_controls(state->radio, target,
                                      OPENRSP_CHANGE_RSPDUO_CONTROLS);
    fprintf(stderr,
            "OPENRSPD_RECOVERY_CONFIG status=%d bootstrap_rf=%u fs=%u rf=%u bw=%u if=%d gr=%d lna=%u time_unix_ms=%llu\n",
            result, bootstrap->center_frequency_hz, target->sample_rate_hz,
            target->center_frequency_hz, target->bandwidth_hz,
            target->if_frequency_hz, target->gain_reduction_db,
            target->lna_state, wall_clock_milliseconds());
    (void)fflush(stderr);
    state->recovery_config_pending = false;
    if (result != 0) {
        atomic_store(&state->stream_error, true);
        if (state->radio) (void)mirisdr_cancel_async(state->radio);
    } else if (restart_for_sample_rate) {
        state->logged_usb_iq = false;
        state->logged_socket_iq = false;
        atomic_store(&state->first_iq_seen, false);
        atomic_store(&state->stream_error, false);
        atomic_store(&state->stream_result, 0);
        atomic_store(&state->streaming, true);
        if (pthread_create(&state->stream_thread, NULL, stream_main, state) != 0) {
            atomic_store(&state->streaming, false);
            atomic_store(&state->stream_error, true);
            return;
        }
        state->stream_thread_started = true;
    }
}

static uint32_t apply_update(daemon_state *state, const openrsp_update_request *update)
{
    const openrsp_radio_config *next = &update->config;
    const openrsp_radio_config *old = &state->config;
    uint32_t flags = update->changed_flags;
    if (next->tuner == OPENRSP_TUNER_B &&
        (flags & (OPENRSP_CHANGE_AM_PORT | OPENRSP_CHANGE_AM_NOTCH)) != 0u)
        return OPENRSP_STATUS_BAD_REQUEST;
    int result = 0;
    if (state->dual_mode) {
        if (next->tuner != OPENRSP_TUNER_A && next->tuner != OPENRSP_TUNER_B)
            return OPENRSP_STATUS_BAD_REQUEST;
        openrsp_radio_config *old_channel = next->tuner == OPENRSP_TUNER_B ?
            &state->dual_config.channel_b : &state->dual_config.channel_a;
        if (next->sample_rate_hz != state->dual_config.sample_rate_hz ||
            next->if_frequency_hz != old_channel->if_frequency_hz ||
            next->bandwidth_hz != old_channel->bandwidth_hz)
            return OPENRSP_STATUS_BAD_REQUEST;
        if (!openrspd_config_valid(next)) return OPENRSP_STATUS_BAD_REQUEST;
        int dual_result = 0;
        if ((flags & (OPENRSP_CHANGE_RF | OPENRSP_CHANGE_GAIN)) != 0u)
            dual_result |= mirisdr_update_rspduo_dual(
                state->radio, next->tuner, next->center_frequency_hz,
                next->gain_reduction_db, next->lna_state);
        dual_result |= set_rspduo_controls(state->radio, next, flags);
        if (dual_result < 0) return OPENRSP_STATUS_IO_ERROR;
        *old_channel = *next;
        return OPENRSP_STATUS_OK;
    }
    if (next->tuner != old->tuner) return OPENRSP_STATUS_BAD_REQUEST;
    if (!openrspd_config_valid(next)) return OPENRSP_STATUS_BAD_REQUEST;
    if ((flags & OPENRSP_CHANGE_SAMPLE_RATE) != 0u &&
        next->sample_rate_hz != old->sample_rate_hz)
        result |= set_rspduo_sample_rate(state->radio, next);
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
    result |= set_rspduo_controls(state->radio, next, flags);
    if (result < 0) return OPENRSP_STATUS_IO_ERROR;
    state->config = *next;
    return OPENRSP_STATUS_OK;
}

static uint32_t snapshot_sdrplay_devices(daemon_state *state,
                                         openrsp_device_record *records,
                                         size_t capacity)
{
    struct timespec now = {0};
    /* On Windows, enumerating through a second libusb context after the async
     * event thread has stopped can corrupt the retained device context.  A
     * held radio is the same receiver already represented by this snapshot;
     * keep serving that verified identity until the handle is released. */
    if (state->radio && state->device_snapshot_valid) {
        uint32_t copied = state->device_snapshot_count;
        if (copied > capacity) copied = (uint32_t)capacity;
        if (records && copied != 0u)
            memcpy(records, state->device_snapshot,
                   copied * sizeof(*records));
        return state->device_snapshot_count;
    }
    if (state->device_snapshot_valid &&
        clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
        time_t seconds = now.tv_sec - state->device_snapshot_time.tv_sec;
        long nanoseconds = now.tv_nsec - state->device_snapshot_time.tv_nsec;
        if (nanoseconds < 0) {
            --seconds;
            nanoseconds += 1000000000L;
        }
        if (seconds == 0 &&
            nanoseconds < OPENRSPD_DEVICE_SNAPSHOT_CACHE_NANOSECONDS) {
            uint32_t copied = state->device_snapshot_count;
            if (copied > capacity) copied = (uint32_t)capacity;
            if (records && copied != 0u)
                memcpy(records, state->device_snapshot,
                       copied * sizeof(*records));
            return state->device_snapshot_count;
        }
    }

    /* A physically replugged RSPduo first appears as the raw 1df7:3020
     * firmware loader with no serial descriptor.  Stable-serial matching is
     * impossible until the driver has loaded the locally installed firmware.
     * Bootstrap only devices whose descriptor explicitly identifies that
     * cold state; never guess from an index or from another receiver. */
    for (uint32_t pass = 0u; !state->radio && pass < OPENRSPD_MAX_DEVICES; ++pass) {
        uint32_t candidate_count = mirisdr_get_device_count();
        bool bootstrapped = false;
        for (uint32_t index = 0u; index < candidate_count; ++index) {
            int identity_state = mirisdr_device_requires_firmware(index);
            bool cold_boot = identity_state == MIRISDR_RSPDUO_IDENTITY_COLD;
#if defined(_WIN32)
            bool transient_recovery =
                identity_state == MIRISDR_RSPDUO_IDENTITY_UNREADABLE;
#else
            bool transient_recovery = false;
#endif
            if (!cold_boot && !transient_recovery) continue;
            if (transient_recovery) {
                int restart_result = openrsp_windows_restart_usb_device(
                    0x1df7u, 0x3020u,
                    state->acquired_identity.serial[0] != '\0' ?
                        state->acquired_identity.serial : NULL);
                fprintf(stderr,
                        "OPENRSPD_PNP_RESTART index=%u status=%s result=%d\n",
                        index, restart_result == 0 ? "requested" : "failed",
                        restart_result);
                (void)fflush(stderr);
                if (restart_result == 0) {
                    const struct timespec restart_delay = {.tv_sec = 1};
                    (void)nanosleep(&restart_delay, NULL);
                    bootstrapped = true;
                    break;
                }
                continue;
            }
            const char *operation = "COLD_BOOT";
            fprintf(stderr, "OPENRSPD_%s index=%u status=starting\n",
                    operation, index);
            (void)fflush(stderr);
            mirisdr_dev_t *candidate = NULL;
            int open_result = mirisdr_open(&candidate, index);
            int close_result = candidate ? mirisdr_close(candidate) : 0;
            fprintf(stderr,
                    "OPENRSPD_%s index=%u status=%s open=%d close=%d\n",
                    operation, index, open_result == 0 ? "ready" : "failed",
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
            /* The Windows backend can temporarily reject a serial-string
             * request while this daemon already owns the WinUSB interface.
             * Keep the identity acquired from the same physical device
             * instead of replacing it with the port-path fallback. */
            if (state->radio &&
                raw_index == state->acquired_identity.device_index &&
                vendor_id == state->acquired_identity.vendor_id &&
                product_id == state->acquired_identity.product_id &&
                state->acquired_identity.serial[0] != '\0')
                (void)snprintf(record->serial, sizeof(record->serial), "%s",
                               state->acquired_identity.serial);
            if (product_id == 0x3020u) {
                record->rspduo_mode_mask = state->duo_active &&
                    state->duo.master_selected ? OPENRSP_MODE_CAP_SLAVE :
                    (OPENRSP_MODE_CAP_SINGLE | OPENRSP_MODE_CAP_DUAL |
                     OPENRSP_MODE_CAP_MASTER);
                record->rspduo_sample_rate_hz = state->duo_active ?
                    state->duo.sample_rate_hz : 0u;
            }
        }
        ++count;
    }
    uint32_t cached = count;
    if (cached > capacity) cached = (uint32_t)capacity;
    if (cached > OPENRSPD_MAX_DEVICES) cached = OPENRSPD_MAX_DEVICES;
    if (records && cached != 0u)
        memcpy(state->device_snapshot, records,
               cached * sizeof(*records));
    state->device_snapshot_count = cached;
    if (clock_gettime(CLOCK_MONOTONIC, &state->device_snapshot_time) == 0)
        state->device_snapshot_valid = true;
    return count;
}

static int resolve_acquired_device(daemon_state *state, uint32_t *device_index)
{
    openrsp_device_record devices[OPENRSPD_MAX_DEVICES];
    uint32_t count = snapshot_sdrplay_devices(state, devices, OPENRSPD_MAX_DEVICES);
    int result = openrsp_identity_resolve(&state->acquired_identity, devices,
                                          count, device_index);
    if (result == 0) state->acquired_identity.device_index = *device_index;
    return result;
}

static int duo_role_descriptor(const daemon_state *state,
                               openrsp_socket_t descriptor,
                               openrspd_duo_role *role)
{
    if (role != NULL) *role = OPENRSPD_DUO_ROLE_NONE;
    if (state == NULL || !state->duo_active) return 0;
    openrspd_duo_role found = openrspd_duo_role_for_descriptor(
        &state->duo, descriptor);
    if (role != NULL) *role = found;
    return found != OPENRSPD_DUO_ROLE_NONE;
}

static int duo_prepare_config(daemon_state *state)
{
    if (state == NULL || (!state->duo_config_a && !state->duo_config_b)) return -1;
    if (!state->duo_config_a) {
        state->duo_config.channel_a = state->duo_config.channel_b;
        state->duo_config.channel_a.tuner = OPENRSP_TUNER_A;
        state->duo_config.channel_a.bias_tee_enabled = 0u;
        state->duo_config.channel_a.am_port_select = 0u;
        state->duo_config.channel_a.am_notch_enabled = 0u;
    }
    if (!state->duo_config_b) {
        state->duo_config.channel_b = state->duo_config.channel_a;
        state->duo_config.channel_b.tuner = OPENRSP_TUNER_B;
        state->duo_config.channel_b.bias_tee_enabled = 0u;
        state->duo_config.channel_b.am_port_select = 0u;
        state->duo_config.channel_b.am_notch_enabled = 0u;
    }
    state->duo_config.sample_rate_hz = state->duo.sample_rate_hz;
    state->duo_config.channel_a.sample_rate_hz = state->duo.sample_rate_hz;
    state->duo_config.channel_b.sample_rate_hz = state->duo.sample_rate_hz;
    int32_t required_if = state->duo.sample_rate_hz == 6000000u ? 1620000 : 2048000;
    state->duo_config.channel_a.if_frequency_hz = required_if;
    state->duo_config.channel_b.if_frequency_hz = required_if;
    state->duo_config.channel_a.bandwidth_hz =
        state->duo_config.channel_a.bandwidth_hz > 1536000u ? 1536000u :
        state->duo_config.channel_a.bandwidth_hz;
    state->duo_config.channel_b.bandwidth_hz =
        state->duo_config.channel_b.bandwidth_hz > 1536000u ? 1536000u :
        state->duo_config.channel_b.bandwidth_hz;
    return valid_dual_config(&state->duo_config) ? 0 : -1;
}

static uint32_t duo_reset_hardware(daemon_state *state)
{
    if (state == NULL) return OPENRSP_STATUS_BAD_REQUEST;
    int result = 0;
    if (state->radio && stop_stream(state) != 0) result = -1;
    if (close_radio(state, "duo-reset") != 0) result = -1;
    state->configured = false;
    state->dual_mode = false;
    return result == 0 ? OPENRSP_STATUS_OK : OPENRSP_STATUS_IO_ERROR;
}

/* Apply a full role configuration that arrives after the shared direct-dual
 * endpoint is already running.  Sample rate, IF, and bandwidth are shared by
 * the two lanes and therefore cannot be changed through this per-role path;
 * RF/gain and RSPduo controls can be updated in place. */
static uint32_t duo_apply_running_config(daemon_state *state,
                                         const openrsp_radio_config *config)
{
    if (state == NULL || config == NULL || !state->radio ||
        !state->configured || !state->dual_mode)
        return OPENRSP_STATUS_OK;
    openrsp_radio_config *current = config->tuner == OPENRSP_TUNER_B ?
        &state->duo_config.channel_b : &state->duo_config.channel_a;
    if (config->sample_rate_hz != state->duo_config.sample_rate_hz ||
        config->if_frequency_hz != current->if_frequency_hz ||
        config->bandwidth_hz != current->bandwidth_hz)
        return OPENRSP_STATUS_BAD_REQUEST;
    int result = 0;
    if (config->center_frequency_hz != current->center_frequency_hz ||
        config->gain_reduction_db != current->gain_reduction_db ||
        config->lna_state != current->lna_state)
        result |= mirisdr_update_rspduo_dual(
            state->radio, config->tuner, config->center_frequency_hz,
            config->gain_reduction_db, config->lna_state);
    result |= set_rspduo_controls(state->radio, config,
                                  OPENRSP_CHANGE_RSPDUO_CONTROLS);
    if (result < 0) return OPENRSP_STATUS_IO_ERROR;
    *current = *config;
    return OPENRSP_STATUS_OK;
}

static uint32_t duo_result_status(openrspd_duo_result result)
{
    switch (result) {
    case OPENRSPD_DUO_OK: return OPENRSP_STATUS_OK;
    case OPENRSPD_DUO_START_PENDING: return OPENRSP_STATUS_START_PENDING;
    case OPENRSPD_DUO_STOP_PENDING: return OPENRSP_STATUS_STOP_PENDING;
    case OPENRSPD_DUO_BUSY: return OPENRSP_STATUS_BUSY;
    default: return OPENRSP_STATUS_BAD_REQUEST;
    }
}

static int duo_identity_equal(const openrsp_acquire_request *left,
                              const openrsp_acquire_request *right)
{
    return left != NULL && right != NULL && left->vendor_id == right->vendor_id &&
           left->product_id == right->product_id &&
           strcmp(left->serial, right->serial) == 0;
}

static int serve_request(openrsp_socket_t descriptor, daemon_state *state)
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
    bool start_stream_after_response = false;
    bool start_stream_after_error = false;
    bool duo_start_stream_after_response = false;
    openrspd_duo_event duo_event_after_response;
    openrspd_duo_event_clear(&duo_event_after_response);
    response.sequence = request.sequence;
    if (request.magic != OPENRSP_PROTOCOL_MAGIC || request.version != OPENRSP_PROTOCOL_VERSION) {
        response.status = OPENRSP_STATUS_BAD_REQUEST;
    } else if (request.type == OPENRSP_CMD_PING && request.payload_bytes == 0u) {
        response.status = OPENRSP_STATUS_OK;
    } else if (request.type == OPENRSP_CMD_LIST && request.payload_bytes == 0u) {
        response.status = OPENRSP_STATUS_OK;
        listed_count = snapshot_sdrplay_devices(state, listed_devices,
                                                OPENRSPD_MAX_DEVICES);
        response.changed_flags = listed_count;
    } else if (request.type == OPENRSP_CMD_LOCK_API && request.payload_bytes == 0u) {
        if (state->api_lock_owner >= 0 && state->api_lock_owner != descriptor) {
            response.status = OPENRSP_STATUS_BUSY;
        } else {
            state->api_lock_owner = descriptor;
            response.status = OPENRSP_STATUS_OK;
            fprintf(stderr, "OPENRSPD_API_LOCK fd=%lld\n",
                    (long long)descriptor);
            (void)fflush(stderr);
        }
    } else if (request.type == OPENRSP_CMD_UNLOCK_API && request.payload_bytes == 0u &&
               state->api_lock_owner == descriptor) {
        release_api_lock(state, descriptor, "request");
        response.status = OPENRSP_STATUS_OK;
    } else if (request.type == OPENRSP_CMD_DUO_ACQUIRE &&
               request.payload_bytes == sizeof(openrsp_duo_acquire_request)) {
        const openrsp_duo_acquire_request *acquire =
            (const openrsp_duo_acquire_request *)payload;
        if (state->owner >= 0 || (state->duo_active &&
                                  acquire->role == OPENRSP_DUO_ROLE_MASTER &&
                                  state->duo.master_selected)) {
            response.status = OPENRSP_STATUS_BUSY;
        } else if (acquire->role != OPENRSP_DUO_ROLE_MASTER &&
                   acquire->role != OPENRSP_DUO_ROLE_SLAVE) {
            response.status = OPENRSP_STATUS_BAD_REQUEST;
        } else {
            uint32_t resolved_index = 0u;
            int identity_result = 0;
            if (acquire->identity.vendor_id != 0x1df7u ||
                acquire->identity.product_id != 0x3020u)
                identity_result = OPENRSP_IDENTITY_NOT_FOUND;
            if (identity_result == 0 && acquire->role == OPENRSP_DUO_ROLE_MASTER) {
                openrsp_device_record devices[OPENRSPD_MAX_DEVICES];
                uint32_t count = snapshot_sdrplay_devices(state, devices,
                                                           OPENRSPD_MAX_DEVICES);
                identity_result = openrsp_identity_resolve(
                    &acquire->identity, devices, count, &resolved_index);
            } else if (!state->duo_active || !state->duo.master_selected ||
                       !duo_identity_equal(&state->acquired_identity,
                                           &acquire->identity)) {
                identity_result = OPENRSP_IDENTITY_NOT_FOUND;
            }
            if (identity_result != 0) {
                response.status = OPENRSP_STATUS_BAD_REQUEST;
            } else {
                openrspd_duo_result duo_result = openrspd_duo_acquire(
                    &state->duo, descriptor,
                    acquire->role == OPENRSP_DUO_ROLE_MASTER ?
                        OPENRSPD_DUO_ROLE_MASTER : OPENRSPD_DUO_ROLE_SLAVE,
                    acquire->tuner, acquire->sample_rate_hz,
                    &duo_event_after_response);
                response.status = duo_result_status(duo_result);
                if (duo_result == OPENRSPD_DUO_OK) {
                    state->duo_active = true;
                    state->device_snapshot_valid = false;
                    if (acquire->role == OPENRSP_DUO_ROLE_MASTER) {
                        state->acquired_identity = acquire->identity;
                        state->acquired_identity.device_index = resolved_index;
                    }
                }
            }
        }
    } else if (request.type == OPENRSP_CMD_DUO_CONFIGURE &&
               request.payload_bytes == sizeof(openrsp_radio_config)) {
        openrspd_duo_role role;
        if (!duo_role_descriptor(state, descriptor, &role)) {
            response.status = state->owner >= 0 ? OPENRSP_STATUS_BUSY :
                              OPENRSP_STATUS_UNSUPPORTED;
        } else {
            const openrsp_radio_config *config =
                (const openrsp_radio_config *)payload;
            uint32_t expected_tuner = role == OPENRSPD_DUO_ROLE_MASTER ?
                state->duo.master_tuner : state->duo.slave_tuner;
            if (!openrspd_config_valid(config) || config->tuner != expected_tuner ||
                config->sample_rate_hz != state->duo.sample_rate_hz) {
                response.status = OPENRSP_STATUS_BAD_REQUEST;
            } else {
                response.status = duo_apply_running_config(state, config);
                if (response.status == OPENRSP_STATUS_OK) {
                    if (config->tuner == OPENRSP_TUNER_A) {
                        state->duo_config.channel_a = *config;
                        state->duo_config_a = true;
                    } else {
                        state->duo_config.channel_b = *config;
                        state->duo_config_b = true;
                    }
                }
            }
        }
    } else if (request.type == OPENRSP_CMD_DUO_START &&
               request.payload_bytes == 0u) {
        openrspd_duo_role role;
        if (!duo_role_descriptor(state, descriptor, &role)) {
            response.status = state->owner >= 0 ? OPENRSP_STATUS_BUSY :
                              OPENRSP_STATUS_UNSUPPORTED;
        } else {
            openrspd_duo_result duo_result = openrspd_duo_initialise(
                &state->duo, descriptor, &duo_event_after_response);
            response.status = duo_result_status(duo_result);
            if (duo_result == OPENRSPD_DUO_OK && role == OPENRSPD_DUO_ROLE_MASTER) {
                if (duo_prepare_config(state) != 0) {
                    state->duo.master_initialised = 0u;
                    response.status = OPENRSP_STATUS_BAD_REQUEST;
                    openrspd_duo_event_clear(&duo_event_after_response);
                } else {
                    response.status = configure_dual_radio(state, &state->duo_config);
                    if (response.status == OPENRSP_STATUS_OK) {
                        state->duo_active = true;
                        state->dual_mode = true;
                        duo_start_stream_after_response = true;
                    } else {
                        state->duo.master_initialised = 0u;
                    }
                }
            } else if (duo_result == OPENRSPD_DUO_OK &&
                       role == OPENRSPD_DUO_ROLE_SLAVE) {
                /* The direct-dual endpoint is already running for the master;
                 * the slave only becomes routable after this acknowledgement. */
                if (!state->radio || !state->configured) {
                    state->duo.slave_initialised = 0u;
                    response.status = OPENRSP_STATUS_IO_ERROR;
                    openrspd_duo_event_clear(&duo_event_after_response);
                }
            }
        }
    } else if (request.type == OPENRSP_CMD_DUO_STOP &&
               request.payload_bytes == 0u) {
        openrspd_duo_role role;
        if (!duo_role_descriptor(state, descriptor, &role)) {
            response.status = state->owner >= 0 ? OPENRSP_STATUS_BUSY :
                              OPENRSP_STATUS_UNSUPPORTED;
        } else {
            openrspd_duo_result duo_result = openrspd_duo_uninitialise(
                &state->duo, descriptor, &duo_event_after_response);
            response.status = duo_result_status(duo_result);
            if (duo_result == OPENRSPD_DUO_OK && role == OPENRSPD_DUO_ROLE_MASTER)
                response.status = duo_reset_hardware(state);
        }
    } else if (request.type == OPENRSP_CMD_DUO_RELEASE &&
               request.payload_bytes == 0u) {
        openrspd_duo_role role;
        if (!duo_role_descriptor(state, descriptor, &role)) {
            response.status = state->owner >= 0 ? OPENRSP_STATUS_BUSY :
                              OPENRSP_STATUS_UNSUPPORTED;
        } else {
            openrspd_duo_result duo_result = openrspd_duo_release(
                &state->duo, descriptor, &duo_event_after_response);
            response.status = duo_result_status(duo_result);
            if (duo_result == OPENRSPD_DUO_OK) {
                state->device_snapshot_valid = false;
                if (!state->duo.master_selected && !state->duo.slave_selected) {
                    response.status = duo_reset_hardware(state);
                    state->duo_active = false;
                    state->duo_config_a = false;
                    state->duo_config_b = false;
                }
            }
        }
    } else if (request.type == OPENRSP_CMD_DUO_SWAP_RATE &&
               request.payload_bytes == sizeof(openrsp_duo_rate_request)) {
        openrspd_duo_role role;
        if (!duo_role_descriptor(state, descriptor, &role)) {
            response.status = state->owner >= 0 ? OPENRSP_STATUS_BUSY :
                              OPENRSP_STATUS_UNSUPPORTED;
        } else if (role != OPENRSPD_DUO_ROLE_MASTER) {
            response.status = OPENRSP_STATUS_BAD_REQUEST;
        } else if (!state->duo.master_initialised) {
            response.status = OPENRSP_STATUS_START_PENDING;
        } else {
            const openrsp_duo_rate_request *rate =
                (const openrsp_duo_rate_request *)payload;
            if (rate->sample_rate_hz != 6000000u &&
                rate->sample_rate_hz != 8000000u) {
                response.status = OPENRSP_STATUS_BAD_REQUEST;
            } else if (state->duo.slave_initialised) {
                response.status = OPENRSP_STATUS_STOP_PENDING;
            } else if (rate->sample_rate_hz == state->duo.sample_rate_hz) {
                response.status = OPENRSP_STATUS_OK;
            } else if ((!state->duo_config_a && !state->duo_config_b) ||
                       duo_prepare_config(state) != 0) {
                response.status = OPENRSP_STATUS_BAD_REQUEST;
            } else {
                openrsp_dual_config saved = state->duo_config;
                uint32_t saved_rate = state->duo.sample_rate_hz;
                bool was_streaming = state->stream_thread_started;
                if (was_streaming && stop_stream(state) != 0) {
                    response.status = OPENRSP_STATUS_IO_ERROR;
                } else {
                    state->duo.sample_rate_hz = rate->sample_rate_hz;
                    if (duo_prepare_config(state) != 0) {
                        state->duo.sample_rate_hz = saved_rate;
                        state->duo_config = saved;
                        response.status = OPENRSP_STATUS_BAD_REQUEST;
                    } else {
                        uint32_t reset_status = duo_reset_hardware(state);
                        response.status = reset_status;
                        if (reset_status == OPENRSP_STATUS_OK)
                            response.status = configure_dual_radio(
                                state, &state->duo_config);
                        if (response.status == OPENRSP_STATUS_OK) {
                            state->dual_mode = true;
                            duo_start_stream_after_response = was_streaming;
                        } else {
                            state->duo.sample_rate_hz = saved_rate;
                            state->duo_config = saved;
                            if (reset_status == OPENRSP_STATUS_OK)
                                (void)configure_dual_radio(state, &saved);
                        }
                    }
                }
            }
        }
    } else if (request.type == OPENRSP_CMD_DUO_UPDATE &&
               request.payload_bytes == sizeof(openrsp_update_request)) {
        openrspd_duo_role role;
        if (!duo_role_descriptor(state, descriptor, &role)) {
            response.status = state->owner >= 0 ? OPENRSP_STATUS_BUSY :
                              OPENRSP_STATUS_UNSUPPORTED;
        } else if (!state->radio || !state->configured ||
                   !state->duo.master_initialised) {
            response.status = OPENRSP_STATUS_START_PENDING;
        } else {
            const openrsp_update_request *update =
                (const openrsp_update_request *)payload;
            uint32_t expected_tuner = role == OPENRSPD_DUO_ROLE_MASTER ?
                state->duo.master_tuner : state->duo.slave_tuner;
            if (update->config.tuner != expected_tuner) {
                response.status = OPENRSP_STATUS_BAD_REQUEST;
            } else {
                response.status = apply_update(state, update);
            }
        }
    } else if (request.type == OPENRSP_CMD_ACQUIRE &&
               request.payload_bytes == sizeof(openrsp_acquire_request)) {
        const openrsp_acquire_request *acquire = (const openrsp_acquire_request *)payload;
        if (state->duo_active) {
            response.status = OPENRSP_STATUS_BUSY;
        } else if (state->owner >= 0 && state->owner != descriptor) {
            response.status = OPENRSP_STATUS_BUSY;
        } else if (state->owner == descriptor) {
            response.status = OPENRSP_STATUS_OK;
        } else {
            openrsp_device_record devices[OPENRSPD_MAX_DEVICES];
            uint32_t count = snapshot_sdrplay_devices(state, devices,
                                                      OPENRSPD_MAX_DEVICES);
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
                    (void)close_radio(state, "identity-change");
                    state->configured = false;
                }
                if (running) {
                    state->owner = descriptor;
                    state->acquired_identity = *acquire;
                    state->acquired_identity.device_index = resolved_index;
                    response.status = OPENRSP_STATUS_OK;
                } else {
                    response.status = OPENRSP_STATUS_IO_ERROR;
                }
            }
        }
    } else if (request.type == OPENRSP_CMD_START && request.payload_bytes == 0u &&
               state->owner == descriptor && state->radio &&
               !state->stream_thread_started && !state->mode_swap_paused) {
        state->streaming = true;
        state->stream_error = false;
        state->stream_result = 0;
        state->stream_sequence[0] = 0u;
        state->stream_sequence[1] = 0u;
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
    } else if (request.type == OPENRSP_CMD_CONFIGURE_DUAL &&
               request.payload_bytes == sizeof(openrsp_dual_config) &&
               state->owner == descriptor) {
        const openrsp_dual_config *config = (const openrsp_dual_config *)payload;
        response.status = configure_dual_radio(state, config);
        if (response.status == OPENRSP_STATUS_OK) {
            state->bootstrap_configured = false;
            fprintf(stderr,
                    "OPENRSPD_CONFIGURE_DUAL fd=%lld fs=%u rf_a=%u gr_a=%d lna_a=%u rf_b=%u gr_b=%d lna_b=%u\n",
                    (long long)descriptor, config->sample_rate_hz,
                    config->channel_a.center_frequency_hz,
                    config->channel_a.gain_reduction_db,
                    config->channel_a.lna_state,
                    config->channel_b.center_frequency_hz,
                    config->channel_b.gain_reduction_db,
                    config->channel_b.lna_state);
            (void)fflush(stderr);
        }
    } else if (request.type == OPENRSP_CMD_SWAP_TUNER &&
               request.payload_bytes == sizeof(openrsp_swap_request) &&
               state->owner == descriptor && state->configured &&
               !state->dual_mode) {
        const openrsp_swap_request *swap = (const openrsp_swap_request *)payload;
        if ((swap->tuner != OPENRSP_TUNER_A && swap->tuner != OPENRSP_TUNER_B) ||
            !openrspd_config_valid(&swap->config) ||
            swap->config.tuner != swap->tuner) {
            response.status = OPENRSP_STATUS_BAD_REQUEST;
        } else {
            bool was_streaming = state->stream_thread_started;
            if (was_streaming && stop_stream(state) != 0)
                response.status = OPENRSP_STATUS_IO_ERROR;
            if (response.status == OPENRSP_STATUS_OK && state->radio) {
                if (close_radio(state, "tuner-swap") != 0)
                    response.status = OPENRSP_STATUS_IO_ERROR;
            }
            if (response.status == OPENRSP_STATUS_OK)
                response.status = configure_radio(state, &swap->config, 3u);
            if (response.status == OPENRSP_STATUS_OK) {
                state->config = swap->config;
                state->bootstrap_config = swap->config;
                state->bootstrap_configured = true;
                state->configured = true;
                state->mode_swap_paused = was_streaming;
                state->stream_sequence[0] = 0u;
            }
        }
    } else if (request.type == OPENRSP_CMD_SWAP_MODE &&
               request.payload_bytes == sizeof(openrsp_mode_swap_request) &&
               state->owner == descriptor && state->configured) {
        const openrsp_mode_swap_request *swap =
            (const openrsp_mode_swap_request *)payload;
        bool valid_target = swap->mode == OPENRSP_MODE_SINGLE ?
                            openrspd_config_valid(&swap->single) :
                            swap->mode == OPENRSP_MODE_DUAL ?
                            valid_dual_config(&swap->dual) : false;
        if (!valid_target) {
            response.status = OPENRSP_STATUS_BAD_REQUEST;
        } else {
            bool was_streaming = state->stream_thread_started;
            bool old_dual_mode = state->dual_mode;
            openrsp_radio_config old_config = state->config;
            openrsp_radio_config old_bootstrap = state->bootstrap_config;
            openrsp_dual_config old_dual_config = state->dual_config;
            bool old_bootstrap_configured = state->bootstrap_configured;
            if (was_streaming && stop_stream(state) != 0)
                response.status = OPENRSP_STATUS_IO_ERROR;
            if (response.status == OPENRSP_STATUS_OK && state->radio) {
                if (close_radio(state, "mode-swap") != 0)
                    response.status = OPENRSP_STATUS_IO_ERROR;
            }
            if (response.status == OPENRSP_STATUS_OK) {
                response.status = swap->mode == OPENRSP_MODE_DUAL ?
                    configure_dual_radio(state, &swap->dual) :
                    configure_radio(state, &swap->single, 3u);
            }
            if (response.status == OPENRSP_STATUS_OK) {
                if (swap->mode == OPENRSP_MODE_DUAL) {
                    state->dual_config = swap->dual;
                    state->bootstrap_configured = false;
                } else {
                    state->config = swap->single;
                    state->bootstrap_config = swap->single;
                    state->bootstrap_configured = true;
                }
                state->configured = true;
                state->recovery_config_pending = false;
                state->mode_swap_paused = was_streaming;
                state->stream_sequence[0] = 0u;
                state->stream_sequence[1] = 0u;
                fprintf(stderr,
                        "OPENRSPD_SWAP_MODE fd=%lld mode=%u status=%u fs=%u\n",
                        (long long)descriptor, swap->mode, response.status,
                        swap->mode == OPENRSP_MODE_DUAL ?
                        swap->dual.sample_rate_hz : swap->single.sample_rate_hz);
                (void)fflush(stderr);
            } else if (was_streaming) {
                uint32_t rollback = old_dual_mode ?
                    configure_dual_radio(state, &old_dual_config) :
                    configure_radio(state, &old_config, 3u);
                if (rollback == OPENRSP_STATUS_OK) {
                    state->dual_mode = old_dual_mode;
                    state->config = old_config;
                    state->dual_config = old_dual_config;
                    state->bootstrap_config = old_bootstrap;
                    state->bootstrap_configured = old_bootstrap_configured;
                    state->configured = true;
                    state->mode_swap_paused = false;
                    start_stream_after_response = true;
                    start_stream_after_error = true;
                }
            }
        }
    } else if (request.type == OPENRSP_CMD_RESUME_MODE &&
               request.payload_bytes == 0u && state->owner == descriptor &&
               state->configured && state->mode_swap_paused) {
        state->mode_swap_paused = false;
        start_stream_after_response = true;
    } else if (request.type == OPENRSP_CMD_UPDATE &&
               request.payload_bytes == sizeof(openrsp_update_request) &&
               state->owner == descriptor) {
        const openrsp_update_request *update = (const openrsp_update_request *)payload;
        if (!openrspd_config_valid(&update->config)) {
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
            bool restart_for_sample_rate =
                state->stream_thread_started &&
                (update->changed_flags & OPENRSP_CHANGE_SAMPLE_RATE) != 0u &&
                update->config.sample_rate_hz != state->config.sample_rate_hz;
            response.status = restart_for_sample_rate && stop_stream(state) != 0 ?
                              OPENRSP_STATUS_IO_ERROR :
                              apply_update(state, update);
            if (response.status == OPENRSP_STATUS_OK && restart_for_sample_rate)
                start_stream_after_response = true;
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
    if (response.status == OPENRSP_STATUS_OK &&
        duo_event_after_response.kind != OPENRSPD_DUO_EVENT_NONE) {
        if (send_duo_event(state, &duo_event_after_response) != 0)
            return -1;
    }
    if (duo_start_stream_after_response && response.status == OPENRSP_STATUS_OK) {
        state->logged_usb_iq = false;
        state->logged_socket_iq = false;
        atomic_store(&state->first_iq_seen, false);
        atomic_store(&state->streaming, true);
        atomic_store(&state->stream_error, false);
        state->stream_sequence[0] = 0u;
        state->stream_sequence[1] = 0u;
        if (pthread_create(&state->stream_thread, NULL, stream_main, state) != 0)
            return -1;
        state->stream_thread_started = true;
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
    if (start_stream_after_response &&
        (response.status == OPENRSP_STATUS_OK || start_stream_after_error)) {
        state->logged_usb_iq = false;
        state->logged_socket_iq = false;
        atomic_store(&state->first_iq_seen, false);
        atomic_store(&state->streaming, true);
        atomic_store(&state->stream_error, false);
        state->recovery_config_pending = request.type == OPENRSP_CMD_SWAP_TUNER;
        if (pthread_create(&state->stream_thread, NULL, stream_main, state) != 0)
            return -1;
        state->stream_thread_started = true;
    }
    return 1;
}

static void remove_client(daemon_state *state,
                          openrsp_socket_t clients[OPENRSPD_MAX_CLIENTS],
                          size_t index)
{
    openrsp_socket_t descriptor = clients[index];
    if (descriptor < 0) return;
    release_client(state, descriptor);
    release_api_lock(state, descriptor, "disconnect");
    (void)openrsp_socket_close(descriptor);
    clients[index] = OPENRSP_INVALID_SOCKET;
}

static void close_clients(daemon_state *state,
                          openrsp_socket_t clients[OPENRSPD_MAX_CLIENTS])
{
    for (size_t index = 0u; index < OPENRSPD_MAX_CLIENTS; ++index)
        remove_client(state, clients, index);
}

static void accept_clients(openrsp_socket_t server,
                           openrsp_socket_t clients[OPENRSPD_MAX_CLIENTS])
{
    for (;;) {
        openrsp_socket_t client = (openrsp_socket_t)accept(server, NULL, NULL);
        if (client == OPENRSP_INVALID_SOCKET) {
            int error = openrsp_socket_last_error();
            if (openrsp_socket_interrupted(error)) continue;
            if (!openrsp_socket_would_block(error))
                fprintf(stderr, "accept failed: %d\n", error);
            return;
        }
        /* macOS may propagate O_NONBLOCK from the listening socket.  Control
         * frames fit in one write, but IQ frames can otherwise stop at EAGAIN
         * after a partial payload and leave the receiver waiting forever. */
        if (set_blocking(client) != 0) {
            (void)openrsp_socket_close(client);
            continue;
        }
        /* A client can stop draining IQ after a device-removal callback while
         * leaving its socket open.  Never let that wedge the USB callback and
         * libusb event lock indefinitely. */
#if defined(_WIN32)
        const DWORD send_timeout = 2000u;
#else
        const struct timeval send_timeout = {.tv_sec = 2, .tv_usec = 0};
#endif
        const int stream_buffer_bytes = 4 * 1024 * 1024;
        if (setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
                       (const char *)&send_timeout,
                       sizeof(send_timeout)) != 0 ||
            setsockopt(client, SOL_SOCKET, SO_SNDBUF,
                       (const char *)&stream_buffer_bytes,
                       sizeof(stream_buffer_bytes)) != 0) {
            (void)openrsp_socket_close(client);
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
            fprintf(stderr,
                    "OPENRSPD_REJECT fd=%lld reason=too_many_clients\n",
                    (long long)client);
            (void)openrsp_socket_close(client);
        }
    }
}

static int run_server(void)
{
#if defined(_WIN32)
    if (openrsp_socket_startup() != 0) {
        fprintf(stderr, "Winsock startup failed\n");
        return EXIT_FAILURE;
    }
    const char *port_text = getenv("OPENRSPD_PORT");
    unsigned long port = 50151ul;
    if (port_text && port_text[0]) {
        char *end = NULL;
        unsigned long parsed = strtoul(port_text, &end, 10);
        if (!end || end == port_text || end[0] != '\0' || parsed == 0ul ||
            parsed > 65535ul) {
            fprintf(stderr, "Invalid OPENRSPD_PORT: %s\n", port_text);
            openrsp_socket_cleanup();
            return EXIT_FAILURE;
        }
        port = parsed;
    }
    openrsp_socket_t server = (openrsp_socket_t)socket(AF_INET, SOCK_STREAM, 0);
#else
    const char *path = getenv("OPENRSPD_SOCKET");
    if (!path || path[0] == '\0') path = OPENRSP_SOCKET_PATH;
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        fprintf(stderr, "Socket path is too long: %s\n", path);
        return EXIT_FAILURE;
    }
    openrsp_socket_t server = socket(AF_UNIX, SOCK_STREAM, 0);
#endif
    if (server == OPENRSP_INVALID_SOCKET) {
        fprintf(stderr, "socket failed: %d\n", openrsp_socket_last_error());
#if defined(_WIN32)
        openrsp_socket_cleanup();
#endif
        return EXIT_FAILURE;
    }
#if defined(_WIN32)
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons((u_short)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const BOOL exclusive = TRUE;
    if (setsockopt(server, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   (const char *)&exclusive, sizeof(exclusive)) != 0 ||
        set_nonblocking(server) != 0 ||
        bind(server, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server, 8) != 0) {
        fprintf(stderr, "openrspd TCP setup failed: %d\n",
                openrsp_socket_last_error());
        (void)openrsp_socket_close(server);
        openrsp_socket_cleanup();
        return EXIT_FAILURE;
    }
#else
    struct sockaddr_un address = {0};
    address.sun_family = AF_UNIX;
    (void)strcpy(address.sun_path, path);
    (void)unlink(path);
    if (set_nonblocking(server) != 0 ||
        bind(server, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        chmod(path, 0666) != 0 || listen(server, 8) != 0) {
        perror("openrspd socket setup");
        (void)openrsp_socket_close(server);
        (void)unlink(path);
        return EXIT_FAILURE;
    }
#endif
#if defined(_WIN32)
    (void)signal(SIGINT, stop_server);
    (void)signal(SIGTERM, stop_server);
#else
    struct sigaction stop_action = {0};
    stop_action.sa_handler = stop_server;
    (void)sigemptyset(&stop_action.sa_mask);
    (void)sigaction(SIGINT, &stop_action, NULL);
    (void)sigaction(SIGTERM, &stop_action, NULL);
    (void)signal(SIGPIPE, SIG_IGN);
#endif
#if defined(_WIN32)
    printf("OPENRSPD_READY endpoint=127.0.0.1:%lu protocol=%u\n",
           port, OPENRSP_PROTOCOL_VERSION);
#else
    printf("OPENRSPD_READY socket=%s protocol=%u\n", path, OPENRSP_PROTOCOL_VERSION);
#endif
    (void)fflush(stdout);
#if defined(_WIN32)
    if (service_mode) report_service_status(SERVICE_RUNNING, NO_ERROR, 0u);
#endif

    daemon_state state = {
        .owner = -1,
        .api_lock_owner = -1,
        .write_lock = PTHREAD_MUTEX_INITIALIZER
    };
    openrspd_duo_session_init(&state.duo);
    atomic_init(&state.streaming, false);
    atomic_init(&state.stream_error, false);
    atomic_init(&state.stream_result, 0);
    atomic_init(&state.first_iq_seen, false);
    atomic_init(&state.control_write_pending, false);
    atomic_init(&state.client_write_error, false);
    atomic_init(&state.client_write_error_fd, -1);
    openrsp_socket_t clients[OPENRSPD_MAX_CLIENTS];
    for (size_t index = 0u; index < OPENRSPD_MAX_CLIENTS; ++index)
        clients[index] = OPENRSP_INVALID_SOCKET;
    while (running) {
        if (atomic_exchange(&state.client_write_error, false)) {
            openrsp_socket_t failed_fd =
                atomic_exchange(&state.client_write_error_fd,
                                OPENRSP_INVALID_SOCKET);
            for (size_t index = 0u; index < OPENRSPD_MAX_CLIENTS; ++index) {
                if (clients[index] == failed_fd ||
                    (failed_fd < 0 && clients[index] == state.owner)) {
                    fprintf(stderr,
                            "OPENRSPD_CLIENT_EVICT fd=%lld reason=iq-write-timeout\n",
                            (long long)clients[index]);
                    (void)fflush(stderr);
                    remove_client(&state, clients, index);
                    break;
                }
            }
            continue;
        }
        recover_failed_stream(&state);
        finish_recovery_config(&state);
        openrsp_pollfd fds[OPENRSPD_MAX_CLIENTS + 1u];
        size_t client_index[OPENRSPD_MAX_CLIENTS + 1u];
        openrsp_nfds_t count = 1u;
        fds[0].fd = server;
        fds[0].events = OPENRSP_POLL_READ;
        fds[0].revents = 0;
        client_index[0] = OPENRSPD_MAX_CLIENTS;
        for (size_t index = 0u; index < OPENRSPD_MAX_CLIENTS; ++index) {
            if (clients[index] < 0) continue;
            fds[count].fd = clients[index];
            fds[count].events = OPENRSP_POLL_READ | OPENRSP_POLL_HANGUP |
                                OPENRSP_POLL_ERROR;
            fds[count].revents = 0;
            client_index[count] = index;
            ++count;
        }
        int ready = openrsp_socket_poll(fds, count, 1000);
        if (ready < 0) {
            int error = openrsp_socket_last_error();
            if (openrsp_socket_interrupted(error)) continue;
            fprintf(stderr, "poll failed: %d\n", error);
            break;
        }
        if (ready == 0) continue;
        if ((fds[0].revents & OPENRSP_POLL_READ) != 0)
            accept_clients(server, clients);
        /* Ownership cleanup has priority over new commands. A dead lock owner
         * and a waiting contender can become ready in the same poll cycle;
         * serving by descriptor-slot order would otherwise return a spurious
         * BUSY before processing the owner's hangup. */
        for (openrsp_nfds_t index = 1u; index < count; ++index) {
            size_t slot = client_index[index];
            if ((fds[index].revents &
                 (OPENRSP_POLL_HANGUP | OPENRSP_POLL_ERROR |
                  OPENRSP_POLL_INVALID)) != 0) {
                remove_client(&state, clients, slot);
            }
        }
        for (openrsp_nfds_t index = 1u; index < count; ++index) {
            size_t slot = client_index[index];
            if (clients[slot] < 0) continue;
            if ((fds[index].revents & OPENRSP_POLL_READ) == 0) continue;
            int result = serve_request(clients[slot], &state);
            if (result != 1) remove_client(&state, clients, slot);
        }
    }
    close_clients(&state, clients);
    shutdown_radio(&state);
    (void)openrsp_socket_close(server);
#if defined(_WIN32)
    openrsp_socket_cleanup();
#else
    (void)unlink(path);
#endif
    (void)pthread_mutex_destroy(&state.write_lock);
    return running ? EXIT_FAILURE : EXIT_SUCCESS;
}

#if defined(_WIN32)
static void WINAPI openrsp_service_main(DWORD argc, char **argv)
{
    (void)argc;
    (void)argv;
    service_status_handle =
        RegisterServiceCtrlHandlerA("OpenRSP", service_control);
    if (!service_status_handle) return;
    const char *log_path = getenv("OPENRSPD_LOG");
    if (log_path && log_path[0]) {
        (void)freopen(log_path, "a", stdout);
        (void)freopen(log_path, "a", stderr);
        (void)setvbuf(stdout, NULL, _IOLBF, 0u);
        (void)setvbuf(stderr, NULL, _IOLBF, 0u);
    }
    service_mode = 1;
    report_service_status(SERVICE_START_PENDING, NO_ERROR, 5000u);
    int result = run_server();
    report_service_status(SERVICE_STOPPED,
                          result == EXIT_SUCCESS ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR,
                          0u);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--service") == 0) {
        SERVICE_TABLE_ENTRYA table[] = {
            {"OpenRSP", openrsp_service_main},
            {NULL, NULL}
        };
        if (!StartServiceCtrlDispatcherA(table)) {
            fprintf(stderr, "Service dispatcher failed: %lu\n",
                    (unsigned long)GetLastError());
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--service]\n", argv[0]);
        return EXIT_FAILURE;
    }
    return run_server();
}
#else
int main(void)
{
    return run_server();
}
#endif
