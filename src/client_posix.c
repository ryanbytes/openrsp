/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "openrsp/client.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef OPENRSP_SOCKET_TIMEOUT_MILLISECONDS
#define OPENRSP_SOCKET_TIMEOUT_MILLISECONDS 5000
#endif

struct openrsp_client {
    int descriptor;
};

static int transfer_exact(int descriptor, void *buffer, size_t bytes, int writing)
{
    unsigned char *cursor = buffer;
    size_t transferred = 0u;
    while (bytes != 0u) {
        ssize_t count;
        if (writing) {
#if defined(MSG_NOSIGNAL)
            count = send(descriptor, cursor, bytes, MSG_NOSIGNAL);
#else
            count = write(descriptor, cursor, bytes);
#endif
        } else {
            count = read(descriptor, cursor, bytes);
        }
        if (count == 0) return -1;
        if (count < 0) {
            if (errno == EINTR) continue;
            if (!writing && transferred == 0u &&
                (errno == EAGAIN || errno == EWOULDBLOCK))
                return OPENRSP_CLIENT_TIMEOUT;
            return -1;
        }
        cursor += (size_t)count;
        bytes -= (size_t)count;
        transferred += (size_t)count;
    }
    return 0;
}

int openrsp_client_connect(const char *socket_path, openrsp_client **out_client)
{
    if (!out_client) return -1;
    const char *path = socket_path && socket_path[0] ? socket_path : OPENRSP_SOCKET_PATH;
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) return -1;
    openrsp_client *client = calloc(1, sizeof(*client));
    if (!client) return -1;
    client->descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client->descriptor < 0) {
        free(client);
        return -1;
    }
    const struct timeval timeout = {
        .tv_sec = OPENRSP_SOCKET_TIMEOUT_MILLISECONDS / 1000,
        .tv_usec = (OPENRSP_SOCKET_TIMEOUT_MILLISECONDS % 1000) * 1000
    };
    const int stream_buffer_bytes = 4 * 1024 * 1024;
    if (setsockopt(client->descriptor, SOL_SOCKET, SO_RCVTIMEO,
                   &timeout, sizeof(timeout)) != 0 ||
        setsockopt(client->descriptor, SOL_SOCKET, SO_SNDTIMEO,
                   &timeout, sizeof(timeout)) != 0 ||
        setsockopt(client->descriptor, SOL_SOCKET, SO_RCVBUF,
                   &stream_buffer_bytes, sizeof(stream_buffer_bytes)) != 0) {
        (void)close(client->descriptor);
        free(client);
        return -1;
    }
#if defined(SO_NOSIGPIPE)
    const int enabled = 1;
    if (setsockopt(client->descriptor, SOL_SOCKET, SO_NOSIGPIPE,
                   &enabled, sizeof(enabled)) != 0) {
        (void)close(client->descriptor);
        free(client);
        return -1;
    }
#endif
    struct sockaddr_un address = {0};
    address.sun_family = AF_UNIX;
    (void)strcpy(address.sun_path, path);
    if (connect(client->descriptor, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        (void)close(client->descriptor);
        free(client);
        return -1;
    }
    *out_client = client;
    return 0;
}

void openrsp_client_close(openrsp_client *client)
{
    if (!client) return;
    /* close() from another thread does not reliably wake a blocking read() on
     * Linux.  shutdown() terminates both directions first so the API reader
     * thread can exit and be joined deterministically. */
    (void)shutdown(client->descriptor, SHUT_RDWR);
    (void)close(client->descriptor);
    free(client);
}

int openrsp_client_send(openrsp_client *client, uint16_t command, uint32_t sequence,
                        const void *payload, uint32_t payload_bytes)
{
    if (!client || (payload_bytes != 0u && !payload)) return -1;
    openrsp_message_header header = {
        .magic = OPENRSP_PROTOCOL_MAGIC, .version = OPENRSP_PROTOCOL_VERSION,
        .type = command, .sequence = sequence, .payload_bytes = payload_bytes
    };
    if (transfer_exact(client->descriptor, &header, sizeof(header), 1) != 0) return -1;
    return payload_bytes == 0u ? 0 :
           transfer_exact(client->descriptor, (void *)payload, payload_bytes, 1);
}

int openrsp_client_receive(openrsp_client *client, openrsp_message_header *header,
                           void *payload, size_t capacity)
{
    if (!client || !header) return -1;
    int header_result = transfer_exact(client->descriptor, header, sizeof(*header), 0);
    if (header_result != OPENRSP_CLIENT_OK) return header_result;
    if (header->magic != OPENRSP_PROTOCOL_MAGIC ||
        header->version != OPENRSP_PROTOCOL_VERSION || header->payload_bytes > capacity)
        return -1;
    if (header->payload_bytes != 0u &&
        transfer_exact(client->descriptor, payload, header->payload_bytes, 0) != 0)
        return OPENRSP_CLIENT_ERROR;
    return 0;
}
