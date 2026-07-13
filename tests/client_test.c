/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "openrsp/client.h"
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    const char *path = getenv("OPENRSPD_SOCKET");
    openrsp_client *client = NULL;
    if (openrsp_client_connect(path, &client) != 0) return 1;
    if (openrsp_client_send(client, OPENRSP_CMD_PING, 42u, NULL, 0u) != 0) return 2;
    openrsp_message_header header;
    openrsp_response response;
    if (openrsp_client_receive(client, &header, &response, sizeof(response)) != 0) return 3;
    openrsp_client_close(client);
    if (header.type != OPENRSP_MSG_RESPONSE || header.sequence != 42u ||
        header.payload_bytes != sizeof(response) || response.status != OPENRSP_STATUS_OK ||
        response.sequence != 42u) return 4;
    puts("OPENRSP_CLIENT_PING_OK");
    return 0;
}
