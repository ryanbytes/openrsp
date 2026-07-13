/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OPENRSP_CLIENT_H
#define OPENRSP_CLIENT_H

#include "openrsp/protocol.h"
#include <stddef.h>
#include <stdint.h>

typedef struct openrsp_client openrsp_client;

#define OPENRSP_CLIENT_OK 0
#define OPENRSP_CLIENT_TIMEOUT 1
#define OPENRSP_CLIENT_ERROR (-1)

int openrsp_client_connect(const char *socket_path, openrsp_client **client);
void openrsp_client_close(openrsp_client *client);
int openrsp_client_send(openrsp_client *client, uint16_t command, uint32_t sequence,
                        const void *payload, uint32_t payload_bytes);
int openrsp_client_receive(openrsp_client *client, openrsp_message_header *header,
                           void *payload, size_t capacity);

#endif
