/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "openrsp/client.h"

#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double monotonic_seconds(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
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
        listen(server, 3) != 0) return 2;
    for (unsigned int connection = 0u; connection < 3u; ++connection) {
        int client = accept(server, NULL, NULL);
        if (client < 0) return 3;
        if (connection < 2u) {
            unsigned char buffer[16];
            while (read(client, buffer, sizeof(buffer)) > 0) {}
        } else {
            const openrsp_message_header header = {
                .magic = OPENRSP_PROTOCOL_MAGIC,
                .version = OPENRSP_PROTOCOL_VERSION,
                .type = OPENRSP_MSG_RESPONSE,
                .sequence = 7u,
                .payload_bytes = sizeof(openrsp_response)
            };
            const openrsp_response response = {
                .status = OPENRSP_STATUS_OK,
                .sequence = 7u
            };
            if (write(client, &header, sizeof(header)) != (ssize_t)sizeof(header) ||
                write(client, &response, sizeof(response) / 2u) !=
                    (ssize_t)(sizeof(response) / 2u))
                return 4;
        }
        (void)close(client);
    }
    (void)close(server);
    (void)unlink(socket_path);
    return 0;
}

static openrsp_client *connect_with_retry(const char *socket_path)
{
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 10000000L};
    for (unsigned int attempt = 0u; attempt < 200u; ++attempt) {
        openrsp_client *client = NULL;
        if (openrsp_client_connect(socket_path, &client) == 0) return client;
        (void)nanosleep(&delay, NULL);
    }
    return NULL;
}

static void *blocked_reader(void *opaque)
{
    openrsp_client *client = opaque;
    openrsp_message_header header;
    openrsp_response response;
    assert(openrsp_client_receive(client, &header, &response, sizeof(response)) ==
           OPENRSP_CLIENT_ERROR);
    return NULL;
}

int main(void)
{
    char socket_path[128];
    (void)snprintf(socket_path, sizeof(socket_path), "/tmp/openrsp-timeout-%ld.sock",
                   (long)getpid());
    pid_t server = fork();
    assert(server >= 0);
    if (server == 0) _exit(run_server(socket_path));

    openrsp_client *client = connect_with_retry(socket_path);
    assert(client != NULL);
    openrsp_message_header header;
    openrsp_response response;
    double started = monotonic_seconds();
    assert(openrsp_client_receive(client, &header, &response, sizeof(response)) ==
           OPENRSP_CLIENT_TIMEOUT);
    double elapsed = monotonic_seconds() - started;
    assert(elapsed >= 0.05 && elapsed < 1.0);
    openrsp_client_close(client);

    client = connect_with_retry(socket_path);
    assert(client != NULL);
    pthread_t reader;
    assert(pthread_create(&reader, NULL, blocked_reader, client) == 0);
    const struct timespec blocked = {.tv_sec = 0, .tv_nsec = 50000000L};
    (void)nanosleep(&blocked, NULL);
    started = monotonic_seconds();
    openrsp_client_close(client);
    assert(pthread_join(reader, NULL) == 0);
    assert(monotonic_seconds() - started < 1.0);

    client = connect_with_retry(socket_path);
    assert(client != NULL);
    assert(openrsp_client_receive(client, &header, &response, sizeof(response)) ==
           OPENRSP_CLIENT_ERROR);
    openrsp_client_close(client);

    int status = 0;
    assert(waitpid(server, &status, 0) == server);
    (void)unlink(socket_path);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    puts("OPENRSP_CLIENT_TIMEOUT_OK");
    return 0;
}
