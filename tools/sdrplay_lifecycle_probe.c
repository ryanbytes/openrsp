/* SPDX-License-Identifier: GPL-2.0-or-later */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include "sdrplay_api_compat.h"

#include <errno.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#endif
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    atomic_ullong samples;
    atomic_ullong samples_after_update_ack;
    atomic_uint callbacks;
    atomic_uint resets;
    atomic_uint discontinuities;
    atomic_uint rf_changed;
    atomic_uint fs_changed;
    atomic_uint gr_changed;
    atomic_uint device_failures;
    atomic_int update_ack_seen;
    unsigned int expected_sample;
    int sample_seen;
#if defined(_WIN32)
    HANDLE ready_handle;
#else
    int ready_fd;
#endif
    int ready_sent;
} lifecycle_metrics;

static void send_ready(lifecycle_metrics *metrics)
{
    const unsigned char ready = 1u;
#if defined(_WIN32)
    DWORD written = 0u;
    (void)WriteFile(metrics->ready_handle, &ready, sizeof(ready), &written, NULL);
#else
    ssize_t written;
    do {
        written = write(metrics->ready_fd, &ready, sizeof(ready));
    } while (written < 0 && errno == EINTR);
#endif
}

static void stream_callback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params,
                            unsigned int num_samples, unsigned int reset, void *opaque)
{
    (void)xi;
    (void)xq;
    lifecycle_metrics *metrics = opaque;
    if (params->numSamples != num_samples)
        atomic_fetch_add(&metrics->discontinuities, 1u);
    if (metrics->sample_seen && params->firstSampleNum != metrics->expected_sample)
        atomic_fetch_add(&metrics->discontinuities, 1u);
    metrics->sample_seen = 1;
    metrics->expected_sample = params->firstSampleNum + num_samples;
    atomic_fetch_add(&metrics->samples, num_samples);
    atomic_fetch_add(&metrics->callbacks, 1u);
    if (reset != 0u) atomic_fetch_add(&metrics->resets, 1u);
    if (params->rfChanged != 0) atomic_fetch_add(&metrics->rf_changed, 1u);
    if (params->fsChanged != 0) {
        atomic_fetch_add(&metrics->fs_changed, 1u);
        atomic_store(&metrics->update_ack_seen, 1);
    }
    if (atomic_load(&metrics->update_ack_seen) != 0)
        atomic_fetch_add(&metrics->samples_after_update_ack, num_samples);
    if (params->grChanged != 0) atomic_fetch_add(&metrics->gr_changed, 1u);
    if (num_samples != 0u && !metrics->ready_sent &&
#if defined(_WIN32)
        metrics->ready_handle != NULL
#else
        metrics->ready_fd >= 0
#endif
    ) {
        metrics->ready_sent = 1;
        send_ready(metrics);
    }
}

static void event_callback(sdrplay_api_EventT event, sdrplay_api_TunerSelectT tuner,
                           sdrplay_api_EventParamsT *params, void *opaque)
{
    (void)tuner;
    (void)params;
    lifecycle_metrics *metrics = opaque;
    if (event == sdrplay_api_DeviceFailure)
        atomic_fetch_add(&metrics->device_failures, 1u);
}

static int sleep_milliseconds(long milliseconds)
{
#if defined(_WIN32)
    Sleep((DWORD)milliseconds);
    return 0;
#else
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000L
    };
    while (nanosleep(&delay, &delay) < 0) {
        if (errno != EINTR) return -1;
    }
    return 0;
#endif
}

static void reset_metrics(lifecycle_metrics *metrics)
{
    memset(metrics, 0, sizeof(*metrics));
    atomic_init(&metrics->samples, 0u);
    atomic_init(&metrics->samples_after_update_ack, 0u);
    atomic_init(&metrics->callbacks, 0u);
    atomic_init(&metrics->resets, 0u);
    atomic_init(&metrics->discontinuities, 0u);
    atomic_init(&metrics->rf_changed, 0u);
    atomic_init(&metrics->fs_changed, 0u);
    atomic_init(&metrics->gr_changed, 0u);
    atomic_init(&metrics->device_failures, 0u);
    atomic_init(&metrics->update_ack_seen, 0);
#if defined(_WIN32)
    metrics->ready_handle = NULL;
#else
    metrics->ready_fd = -1;
#endif
}

static void configure_defaults(sdrplay_api_DeviceParamsT *params)
{
    params->devParams->fsFreq.fsHz = 2000000.0;
    params->devParams->mode = sdrplay_api_BULK;
    params->rxChannelA->tunerParams.rfFreq.rfHz = 101100000.0;
    params->rxChannelA->tunerParams.bwType = sdrplay_api_BW_1_536;
    params->rxChannelA->tunerParams.ifType = sdrplay_api_IF_Zero;
    params->rxChannelA->tunerParams.gain.gRdB = 40;
    params->rxChannelA->tunerParams.gain.LNAstate = 5;
    params->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE;
}

static int run_cycle(unsigned int cycle)
{
    sdrplay_api_DeviceT devices[SDRPLAY_MAX_DEVICES];
    unsigned int count = 0u;
    sdrplay_api_DeviceParamsT *params = NULL;
    lifecycle_metrics metrics;
    reset_metrics(&metrics);
    sdrplay_api_CallbackFnsT callbacks = {
        .StreamACbFn = stream_callback,
        .EventCbFn = event_callback
    };
    int opened = 0;
    int selected = 0;
    int initialized = 0;
    int failed = 0;
    unsigned long long samples_before_update = 0u;

    sdrplay_api_ErrT status = sdrplay_api_Open();
    if (status != sdrplay_api_Success) {
        fprintf(stderr, "cycle %u Open failed: %s\n", cycle,
                sdrplay_api_GetErrorString(status));
        return -1;
    }
    opened = 1;
    status = sdrplay_api_GetDevices(devices, &count, SDRPLAY_MAX_DEVICES);
    if (status != sdrplay_api_Success || count == 0u) {
        fprintf(stderr, "cycle %u GetDevices failed: status=%s count=%u\n", cycle,
                sdrplay_api_GetErrorString(status), count);
        failed = 1;
        goto cleanup;
    }
    if (devices[0].hwVer == SDRPLAY_RSPduo_ID) {
        devices[0].tuner = sdrplay_api_Tuner_A;
        devices[0].rspDuoMode = sdrplay_api_RspDuoMode_Single_Tuner;
    }
    status = sdrplay_api_SelectDevice(&devices[0]);
    if (status != sdrplay_api_Success) {
        fprintf(stderr, "cycle %u SelectDevice failed: %s\n", cycle,
                sdrplay_api_GetErrorString(status));
        failed = 1;
        goto cleanup;
    }
    selected = 1;
    status = sdrplay_api_GetDeviceParams(devices[0].dev, &params);
    if (status != sdrplay_api_Success || params == NULL) {
        fprintf(stderr, "cycle %u GetDeviceParams failed: %s\n", cycle,
                sdrplay_api_GetErrorString(status));
        failed = 1;
        goto cleanup;
    }
    configure_defaults(params);
    status = sdrplay_api_Init(devices[0].dev, &callbacks, &metrics);
    if (status != sdrplay_api_Success) {
        fprintf(stderr, "cycle %u Init failed: %s\n", cycle,
                sdrplay_api_GetErrorString(status));
        failed = 1;
        goto cleanup;
    }
    initialized = 1;
    if (sleep_milliseconds(750) < 0) {
        perror("nanosleep");
        failed = 1;
        goto cleanup;
    }
    samples_before_update = atomic_load(&metrics.samples);

    params->devParams->fsFreq.fsHz = 2048000.0;
    params->rxChannelA->tunerParams.rfFreq.rfHz =
        (cycle & 1u) == 0u ? 101200000.0 : 101300000.0;
    params->rxChannelA->tunerParams.gain.gRdB = 30 + (int)(cycle % 10u);
    status = sdrplay_api_Update(
        devices[0].dev, sdrplay_api_Tuner_A,
        sdrplay_api_Update_Dev_Fs | sdrplay_api_Update_Tuner_Frf |
            sdrplay_api_Update_Tuner_Gr,
        sdrplay_api_Update_Ext1_None);
    if (status != sdrplay_api_Success) {
        fprintf(stderr, "cycle %u Update failed: %s\n", cycle,
                sdrplay_api_GetErrorString(status));
        failed = 1;
        goto cleanup;
    }
    if (sleep_milliseconds(750) < 0) {
        perror("nanosleep");
        failed = 1;
        goto cleanup;
    }

cleanup:
    if (initialized && sdrplay_api_Uninit(devices[0].dev) != sdrplay_api_Success)
        failed = 1;
    if (selected && sdrplay_api_ReleaseDevice(&devices[0]) != sdrplay_api_Success)
        failed = 1;
    if (opened && sdrplay_api_Close() != sdrplay_api_Success)
        failed = 1;

    const unsigned long long samples = atomic_load(&metrics.samples);
    const unsigned long long samples_after_update =
        samples >= samples_before_update ? samples - samples_before_update : 0u;
    const unsigned long long samples_after_update_ack =
        atomic_load(&metrics.samples_after_update_ack);
    const unsigned int callback_count = atomic_load(&metrics.callbacks);
    const unsigned int resets = atomic_load(&metrics.resets);
    const unsigned int discontinuities = atomic_load(&metrics.discontinuities);
    const unsigned int rf_changed = atomic_load(&metrics.rf_changed);
    const unsigned int fs_changed = atomic_load(&metrics.fs_changed);
    const unsigned int gr_changed = atomic_load(&metrics.gr_changed);
    const unsigned int failures = atomic_load(&metrics.device_failures);
    printf("cycle=%u samples=%llu pre_update=%llu post_update=%llu "
           "post_ack=%llu callbacks=%u "
           "resets=%u discontinuities=%u "
           "rf_changed=%u fs_changed=%u gr_changed=%u device_failures=%u\n",
           cycle, samples, samples_before_update, samples_after_update,
           samples_after_update_ack, callback_count, resets, discontinuities,
           rf_changed, fs_changed, gr_changed, failures);
    if (samples_before_update < 1000000u || samples_after_update < 1000000u ||
        samples_after_update_ack < 1000000u ||
        callback_count == 0u || resets != 1u ||
        discontinuities != 0u || rf_changed != 1u || fs_changed != 1u ||
        gr_changed != 1u || failures != 0u)
        failed = 1;
    return failed ? -1 : 0;
}

static int run_crash_victim(
#if defined(_WIN32)
    HANDLE ready_handle
#else
    int ready_fd
#endif
)
{
    sdrplay_api_DeviceT devices[SDRPLAY_MAX_DEVICES];
    unsigned int count = 0u;
    sdrplay_api_DeviceParamsT *params = NULL;
    lifecycle_metrics metrics;
    reset_metrics(&metrics);
#if defined(_WIN32)
    metrics.ready_handle = ready_handle;
#else
    metrics.ready_fd = ready_fd;
#endif
    sdrplay_api_CallbackFnsT callbacks = {
        .StreamACbFn = stream_callback,
        .EventCbFn = event_callback
    };
    if (sdrplay_api_Open() != sdrplay_api_Success) return -1;
    if (sdrplay_api_GetDevices(devices, &count, SDRPLAY_MAX_DEVICES) !=
            sdrplay_api_Success || count == 0u)
        return -1;
    if (devices[0].hwVer == SDRPLAY_RSPduo_ID) {
        devices[0].tuner = sdrplay_api_Tuner_A;
        devices[0].rspDuoMode = sdrplay_api_RspDuoMode_Single_Tuner;
    }
    if (sdrplay_api_SelectDevice(&devices[0]) != sdrplay_api_Success) return -1;
    if (sdrplay_api_GetDeviceParams(devices[0].dev, &params) !=
            sdrplay_api_Success || params == NULL)
        return -1;
    configure_defaults(params);
    if (sdrplay_api_Init(devices[0].dev, &callbacks, &metrics) !=
        sdrplay_api_Success)
        return -1;
    for (;;) {
#if defined(_WIN32)
        Sleep(INFINITE);
#else
        pause();
#endif
    }
}

static int verify_crash_recovery(unsigned int crash_cycle)
{
#if defined(_WIN32)
    SECURITY_ATTRIBUTES security_attributes = {
        .nLength = sizeof(security_attributes),
        .lpSecurityDescriptor = NULL,
        .bInheritHandle = TRUE
    };
    HANDLE ready_read = NULL;
    HANDLE ready_write = NULL;
    if (!CreatePipe(&ready_read, &ready_write, &security_attributes, 0u)) {
        fprintf(stderr, "CreatePipe failed: %lu\n", (unsigned long)GetLastError());
        return -1;
    }
    if (!SetHandleInformation(ready_read, HANDLE_FLAG_INHERIT, 0u)) {
        fprintf(stderr, "SetHandleInformation failed: %lu\n",
                (unsigned long)GetLastError());
        CloseHandle(ready_read);
        CloseHandle(ready_write);
        return -1;
    }

    char executable_path[MAX_PATH];
    DWORD path_length = GetModuleFileNameA(NULL, executable_path,
                                           (DWORD)sizeof(executable_path));
    if (path_length == 0u || path_length >= (DWORD)sizeof(executable_path)) {
        fprintf(stderr, "GetModuleFileName failed: %lu\n",
                (unsigned long)GetLastError());
        CloseHandle(ready_read);
        CloseHandle(ready_write);
        return -1;
    }
    char command_line[2u * MAX_PATH + 64u];
    int command_length = snprintf(command_line, sizeof(command_line),
                                  "\"%s\" --crash-victim %llu",
                                  executable_path,
                                  (unsigned long long)(uintptr_t)ready_write);
    if (command_length < 0 || (size_t)command_length >= sizeof(command_line)) {
        fputs("crash victim command line is too long\n", stderr);
        CloseHandle(ready_read);
        CloseHandle(ready_write);
        return -1;
    }

    STARTUPINFOA startup_info = {0};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info = {0};
    if (!CreateProcessA(NULL, command_line, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, &startup_info, &process_info)) {
        fprintf(stderr, "CreateProcess failed: %lu\n",
                (unsigned long)GetLastError());
        CloseHandle(ready_read);
        CloseHandle(ready_write);
        return -1;
    }
    CloseHandle(ready_write);

    unsigned char ready = 0u;
    DWORD ready_bytes = 0u;
    ULONGLONG deadline = GetTickCount64() + 10000u;
    int ready_ok = 0;
    while (GetTickCount64() < deadline) {
        DWORD available = 0u;
        if (!PeekNamedPipe(ready_read, NULL, 0u, NULL, &available, NULL)) break;
        if (available != 0u) {
            ready_ok = ReadFile(ready_read, &ready, sizeof(ready), &ready_bytes, NULL) &&
                       ready_bytes == (DWORD)sizeof(ready) && ready == 1u;
            break;
        }
        if (WaitForSingleObject(process_info.hProcess, 0u) == WAIT_OBJECT_0) break;
        Sleep(10u);
    }
    CloseHandle(ready_read);
    if (!ready_ok) {
        fprintf(stderr, "crash victim did not begin streaming within 10 seconds\n");
        (void)TerminateProcess(process_info.hProcess, 1u);
        (void)WaitForSingleObject(process_info.hProcess, 10000u);
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        return -1;
    }
    /* The first IQ notification can arrive just before Init returns. Give the
     * victim time to enter its steady streaming wait before terminating it. */
    if (sleep_milliseconds(500) < 0) {
        (void)TerminateProcess(process_info.hProcess, 1u);
        (void)WaitForSingleObject(process_info.hProcess, 10000u);
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        return -1;
    }
    if (!TerminateProcess(process_info.hProcess, 1u)) {
        fprintf(stderr, "TerminateProcess failed: %lu\n",
                (unsigned long)GetLastError());
        (void)WaitForSingleObject(process_info.hProcess, 10000u);
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        return -1;
    }
    if (WaitForSingleObject(process_info.hProcess, 10000u) != WAIT_OBJECT_0) {
        fputs("crash victim did not terminate within 10 seconds\n", stderr);
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        return -1;
    }
    DWORD exit_code = 0u;
    if (!GetExitCodeProcess(process_info.hProcess, &exit_code) || exit_code != 1u) {
        fprintf(stderr, "crash victim did not terminate with TerminateProcess (exit=%lu)\n",
                (unsigned long)exit_code);
        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        return -1;
    }
    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
#else
    int ready_pipe[2];
    if (pipe(ready_pipe) < 0) {
        perror("pipe");
        return -1;
    }
    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        close(ready_pipe[0]);
        close(ready_pipe[1]);
        return -1;
    }
    if (child == 0) {
        close(ready_pipe[0]);
        int result = run_crash_victim(ready_pipe[1]);
        _exit(result == 0 ? 0 : 2);
    }
    close(ready_pipe[1]);
    struct pollfd ready_poll = {.fd = ready_pipe[0], .events = POLLIN};
    int poll_result;
    do {
        poll_result = poll(&ready_poll, 1u, 10000);
    } while (poll_result < 0 && errno == EINTR);
    unsigned char ready = 0u;
    ssize_t ready_bytes = poll_result > 0 ?
        read(ready_pipe[0], &ready, sizeof(ready)) : -1;
    close(ready_pipe[0]);
    if (ready_bytes != (ssize_t)sizeof(ready) || ready != 1u) {
        fprintf(stderr, "crash victim did not begin streaming within 10 seconds\n");
        (void)kill(child, SIGKILL);
        (void)waitpid(child, NULL, 0);
        return -1;
    }
    /* The first IQ notification can arrive just before Init returns. Give the
     * victim time to enter its steady streaming wait before killing it. */
    if (sleep_milliseconds(500) < 0) {
        perror("nanosleep");
        (void)kill(child, SIGKILL);
        (void)waitpid(child, NULL, 0);
        return -1;
    }
    if (kill(child, SIGKILL) < 0) {
        perror("kill");
        (void)waitpid(child, NULL, 0);
        return -1;
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            perror("waitpid");
            return -1;
        }
    }
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
        fprintf(stderr, "crash victim did not terminate with SIGKILL\n");
        return -1;
    }
#endif
    if (sleep_milliseconds(2000) < 0) {
#if defined(_WIN32)
        fputs("sleep after crash victim termination failed\n", stderr);
#else
        perror("nanosleep");
#endif
        return -1;
    }
    if (run_cycle(crash_cycle) < 0) return -1;
    printf("crash_cycle=%u recovered=1\n", crash_cycle);
    return 0;
}

int main(int argc, char **argv)
{
#if defined(_WIN32)
    if (argc == 3 && strcmp(argv[1], "--crash-victim") == 0) {
        char *end = NULL;
        unsigned long long handle_value = _strtoui64(argv[2], &end, 0);
        if (end == argv[2] || *end != '\0' || handle_value == 0u) {
            fputs("invalid crash victim handle\n", stderr);
            return EXIT_FAILURE;
        }
        return run_crash_victim((HANDLE)(uintptr_t)handle_value) == 0 ?
            EXIT_SUCCESS : EXIT_FAILURE;
    }
#endif
    unsigned long cycles = 10u;
    if ((argc == 2 || argc == 3) && strcmp(argv[1], "--crash-recovery") == 0) {
        unsigned long crash_cycles = 1u;
        if (argc == 3) {
            char *end = NULL;
            crash_cycles = strtoul(argv[2], &end, 10);
            if (end == argv[2] || *end != '\0' || crash_cycles == 0u ||
                crash_cycles > 100u) {
                fputs("crash recovery count must be an integer from 1 through 100\n",
                      stderr);
                return EXIT_FAILURE;
            }
        }
        for (unsigned long cycle = 1u; cycle <= crash_cycles; ++cycle) {
            if (verify_crash_recovery((unsigned int)cycle) < 0)
                return EXIT_FAILURE;
        }
        printf("PASS: daemon recovered from %lu SIGKILLed stream owners without a reconnect\n",
               crash_cycles);
        return EXIT_SUCCESS;
    }
    if (argc == 3 && strcmp(argv[1], "--cycles") == 0) {
        char *end = NULL;
        cycles = strtoul(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || cycles == 0u || cycles > 1000u) {
            fputs("--cycles must be an integer from 1 through 1000\n", stderr);
            return EXIT_FAILURE;
        }
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [--cycles COUNT | --crash-recovery [COUNT]]\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    for (unsigned long cycle = 1u; cycle <= cycles; ++cycle) {
        if (run_cycle((unsigned int)cycle) < 0) return EXIT_FAILURE;
    }
    printf("PASS: %lu complete API lifecycle cycles streamed and cleaned up without a reconnect\n",
           cycles);
    return EXIT_SUCCESS;
}
