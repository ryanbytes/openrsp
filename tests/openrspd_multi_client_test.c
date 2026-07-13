/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "openrsp/client.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int receive_response(openrsp_client *client, uint32_t sequence,
                            openrsp_response *response)
{
    openrsp_message_header header;
    if (openrsp_client_receive(client, &header, response, sizeof(*response)) != 0)
        return -1;
    if (header.type != OPENRSP_MSG_RESPONSE || header.sequence != sequence ||
        header.payload_bytes != sizeof(*response) || response->sequence != sequence)
        return -1;
    return 0;
}

static int ping_client(const char *socket_path, uint32_t sequence)
{
    openrsp_client *client = NULL;
    openrsp_response response;
    if (openrsp_client_connect(socket_path, &client) != 0) return -1;
    int result = openrsp_client_send(client, OPENRSP_CMD_PING, sequence, NULL, 0u);
    if (result == 0) result = receive_response(client, sequence, &response);
    openrsp_client_close(client);
    return result == 0 && response.status == OPENRSP_STATUS_OK ? 0 : -1;
}

static int list_client(const char *socket_path, uint32_t sequence)
{
    openrsp_client *client = NULL;
    openrsp_response response;
    if (openrsp_client_connect(socket_path, &client) != 0) return -1;
    int result = openrsp_client_send(client, OPENRSP_CMD_LIST, sequence, NULL, 0u);
    if (result == 0) result = receive_response(client, sequence, &response);
    for (uint32_t index = 0u; result == 0 && index < response.changed_flags; ++index) {
        openrsp_message_header header;
        openrsp_device_record record;
        if (openrsp_client_receive(client, &header, &record, sizeof(record)) != 0 ||
            header.type != OPENRSP_EVENT_DEVICE ||
            header.payload_bytes != sizeof(record))
            result = -1;
    }
    openrsp_client_close(client);
    return result == 0 && response.status == OPENRSP_STATUS_OK ? 0 : -1;
}

static int command(openrsp_client *client, uint16_t type, uint32_t sequence,
                   uint32_t expected_status)
{
    openrsp_response response;
    if (openrsp_client_send(client, type, sequence, NULL, 0u) != 0 ||
        receive_response(client, sequence, &response) != 0)
        return -1;
    return response.status == expected_status ? 0 : -1;
}

static int wait_for_daemon(const char *socket_path)
{
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 50000000L};
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (ping_client(socket_path, 1000u + (uint32_t)attempt) == 0) return 0;
        nanosleep(&delay, NULL);
    }
    return -1;
}

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    char socket_path[128];
    (void)snprintf(socket_path, sizeof(socket_path), "/tmp/openrspd-test-%ld.sock",
                   (long)getpid());
    (void)unlink(socket_path);
    pid_t child = fork();
    if (child < 0) return 3;
    if (child == 0) {
        (void)setenv("OPENRSPD_SOCKET", socket_path, 1);
        execl(argv[1], argv[1], (char *)NULL);
        _exit(127);
    }
    if (wait_for_daemon(socket_path) != 0) {
        (void)kill(child, SIGTERM);
        (void)waitpid(child, NULL, 0);
        (void)unlink(socket_path);
        return 4;
    }

    openrsp_client *held = NULL;
    openrsp_response response;
    int result = 0;
    if (openrsp_client_connect(socket_path, &held) != 0 ||
        openrsp_client_send(held, OPENRSP_CMD_PING, 42u, NULL, 0u) != 0 ||
        receive_response(held, 42u, &response) != 0 ||
        response.status != OPENRSP_STATUS_OK)
        result = 5;
    if (result == 0 && ping_client(socket_path, 43u) != 0) result = 6;
    if (result == 0 && list_client(socket_path, 44u) != 0) result = 7;

    openrsp_client *contender = NULL;
    if (result == 0 && command(held, OPENRSP_CMD_LOCK_API, 45u,
                               OPENRSP_STATUS_OK) != 0) result = 8;
    if (result == 0 && openrsp_client_connect(socket_path, &contender) != 0) result = 9;
    if (result == 0 && command(contender, OPENRSP_CMD_LOCK_API, 46u,
                               OPENRSP_STATUS_BUSY) != 0) result = 10;
    if (result == 0 && ping_client(socket_path, 47u) != 0) result = 11;
    if (result == 0 && list_client(socket_path, 48u) != 0) result = 12;
    /* Closing without UNLOCK simulates an application crash. */
    openrsp_client_close(held);
    held = NULL;
    if (result == 0 && command(contender, OPENRSP_CMD_LOCK_API, 49u,
                               OPENRSP_STATUS_OK) != 0) result = 13;
    if (result == 0 && command(contender, OPENRSP_CMD_UNLOCK_API, 50u,
                               OPENRSP_STATUS_OK) != 0) result = 14;
    openrsp_client_close(contender);

    (void)kill(child, SIGTERM);
    int status = 0;
    (void)waitpid(child, &status, 0);
    (void)unlink(socket_path);
    if (result != 0) return result;
    if (!WIFEXITED(status) && !WIFSIGNALED(status)) return 15;
    puts("OPENRSPD_MULTI_CLIENT_OK");
    return 0;
}
