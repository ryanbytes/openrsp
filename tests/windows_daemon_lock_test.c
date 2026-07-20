/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "openrsp/client.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <windows.h>

static uint32_t command(openrsp_client *client, uint16_t type, uint32_t sequence)
{
    assert(openrsp_client_send(client, type, sequence, NULL, 0u) ==
           OPENRSP_CLIENT_OK);
    openrsp_message_header header;
    openrsp_response response;
    assert(openrsp_client_receive(client, &header, &response, sizeof(response)) ==
           OPENRSP_CLIENT_OK);
    assert(header.type == OPENRSP_MSG_RESPONSE);
    assert(header.sequence == sequence);
    assert(header.payload_bytes == sizeof(response));
    assert(response.sequence == sequence);
    return response.status;
}

int main(void)
{
    openrsp_client *owner = NULL;
    openrsp_client *contender = NULL;
    assert(openrsp_client_connect(NULL, &owner) == OPENRSP_CLIENT_OK);
    assert(openrsp_client_connect(NULL, &contender) == OPENRSP_CLIENT_OK);

    assert(command(owner, OPENRSP_CMD_PING, 1u) == OPENRSP_STATUS_OK);
    assert(command(contender, OPENRSP_CMD_PING, 2u) == OPENRSP_STATUS_OK);
    assert(command(owner, OPENRSP_CMD_LOCK_API, 3u) == OPENRSP_STATUS_OK);
    assert(command(contender, OPENRSP_CMD_LOCK_API, 4u) == OPENRSP_STATUS_BUSY);

    /* Closing without UNLOCK models an application crash.  The daemon must
     * release the cross-client lease before the surviving client retries. */
    openrsp_client_close(owner);
    uint32_t lock_status = OPENRSP_STATUS_BUSY;
    for (uint32_t attempt = 0u; attempt < 100u &&
         lock_status == OPENRSP_STATUS_BUSY; ++attempt) {
        lock_status = command(contender, OPENRSP_CMD_LOCK_API, 5u + attempt);
        if (lock_status == OPENRSP_STATUS_BUSY) Sleep(10u);
    }
    assert(lock_status == OPENRSP_STATUS_OK);
    assert(command(contender, OPENRSP_CMD_UNLOCK_API, 105u) == OPENRSP_STATUS_OK);
    openrsp_client_close(contender);
    puts("WINDOWS_DAEMON_LOCK_OK");
    return 0;
}
