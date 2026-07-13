/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "openrsp/client.h"
#include "sdrplay_api.h"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int receive_response(openrsp_client *client, uint32_t sequence,
                            uint32_t expected_status)
{
    openrsp_message_header header;
    openrsp_response response;
    if (openrsp_client_receive(client, &header, &response, sizeof(response)) != 0 ||
        header.type != OPENRSP_MSG_RESPONSE || header.sequence != sequence ||
        header.payload_bytes != sizeof(response) || response.sequence != sequence)
        return -1;
    return response.status == expected_status ? 0 : -1;
}

static int command(openrsp_client *client, uint16_t type, uint32_t sequence,
                   uint32_t expected_status)
{
    return openrsp_client_send(client, type, sequence, NULL, 0u) == 0 ?
           receive_response(client, sequence, expected_status) : -1;
}

static int wait_for_daemon(const char *socket_path)
{
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 20000000L};
    for (uint32_t attempt = 0u; attempt < 250u; ++attempt) {
        openrsp_client *client = NULL;
        if (openrsp_client_connect(socket_path, &client) == 0) {
            int result = command(client, OPENRSP_CMD_PING, attempt + 1u,
                                 OPENRSP_STATUS_OK);
            openrsp_client_close(client);
            if (result == 0) return 0;
        }
        (void)nanosleep(&delay, NULL);
    }
    return -1;
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    char socket_path[104];
    (void)snprintf(socket_path, sizeof(socket_path),
                   "/tmp/openrsp-api-lock-%ld.sock", (long)getpid());
    (void)unlink(socket_path);
    pid_t daemon = fork();
    if (daemon < 0) return 3;
    if (daemon == 0) {
        (void)setenv("OPENRSPD_SOCKET", socket_path, 1);
        execl(argv[1], argv[1], (char *)NULL);
        _exit(127);
    }
    if (wait_for_daemon(socket_path) != 0) {
        (void)kill(daemon, SIGTERM);
        (void)waitpid(daemon, NULL, 0);
        return 4;
    }
    (void)setenv("OPENRSPD_SOCKET", socket_path, 1);

    openrsp_client *other_process = NULL;
    assert(openrsp_client_connect(socket_path, &other_process) == 0);
    assert(command(other_process, OPENRSP_CMD_LOCK_API, 100u,
                   OPENRSP_STATUS_OK) == 0);

    assert(sdrplay_api_Open() == sdrplay_api_Success);
    assert(sdrplay_api_LockDeviceApi() == sdrplay_api_Fail);

    /* Simulate a process crash: the daemon must release a socket-owned lease. */
    openrsp_client_close(other_process);
    other_process = NULL;
    assert(sdrplay_api_LockDeviceApi() == sdrplay_api_Success);
    assert(sdrplay_api_UnlockDeviceApi() == sdrplay_api_Success);
    assert(sdrplay_api_Close() == sdrplay_api_Success);

    (void)kill(daemon, SIGTERM);
    int status = 0;
    (void)waitpid(daemon, &status, 0);
    (void)unlink(socket_path);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    puts("SDRPLAY_API_CROSS_PROCESS_LOCK_OK");
    return 0;
}
