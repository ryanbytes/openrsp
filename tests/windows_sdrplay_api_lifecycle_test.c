/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "openrsp/protocol.h"
#include "sdrplay_api.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

typedef struct {
    SOCKET listener;
    atomic_uint list_requests;
    atomic_uint acquire_requests;
    atomic_uint configure_requests;
    atomic_uint start_requests;
    atomic_uint update_requests;
    atomic_uint stop_requests;
    atomic_uint release_requests;
    int result;
} mock_daemon;

static int transfer_exact(SOCKET socket, void *buffer, size_t bytes, int writing)
{
    unsigned char *cursor = buffer;
    while (bytes != 0u) {
        int count = writing ? send(socket, (const char *)cursor, (int)bytes, 0) :
                              recv(socket, (char *)cursor, (int)bytes, 0);
        if (count <= 0) return -1;
        cursor += (size_t)count;
        bytes -= (size_t)count;
    }
    return 0;
}

static int send_frame(SOCKET socket, uint16_t type, uint32_t sequence,
                      const void *payload, uint32_t bytes)
{
    const openrsp_message_header header = {
        .magic = OPENRSP_PROTOCOL_MAGIC,
        .version = OPENRSP_PROTOCOL_VERSION,
        .type = type,
        .sequence = sequence,
        .payload_bytes = bytes
    };
    return transfer_exact(socket, (void *)&header, sizeof(header), 1) == 0 &&
           (bytes == 0u || transfer_exact(socket, (void *)payload, bytes, 1) == 0) ? 0 : -1;
}

static int send_response(SOCKET socket, uint32_t sequence)
{
    const openrsp_response response = {
        .status = OPENRSP_STATUS_OK,
        .sequence = sequence
    };
    return send_frame(socket, OPENRSP_MSG_RESPONSE, sequence, &response, sizeof(response));
}

static int send_device(SOCKET socket, uint32_t sequence)
{
    const openrsp_device_record device = {
        .device_index = 1u,
        .vendor_id = 0x1df7u,
        .product_id = 0x2500u,
        .serial = "WINDOWS-LIFECYCLE",
        .model = "SDRplay RSP1"
    };
    const openrsp_response response = {
        .status = OPENRSP_STATUS_OK,
        .sequence = sequence,
        .changed_flags = 1u
    };
    return send_frame(socket, OPENRSP_MSG_RESPONSE, sequence, &response, sizeof(response)) == 0 &&
           send_frame(socket, OPENRSP_EVENT_DEVICE, sequence, &device, sizeof(device)) == 0 ? 0 : -1;
}

static int send_iq(SOCKET socket)
{
    int16_t iq[2048];
    for (size_t index = 0u; index < sizeof(iq) / sizeof(iq[0]); ++index)
        iq[index] = (int16_t)(index - 1024);
    return send_frame(socket, OPENRSP_EVENT_IQ, 1u, iq, sizeof(iq));
}

static int serve_client(mock_daemon *daemon, SOCKET client)
{
    for (;;) {
        openrsp_message_header request;
        unsigned char payload[4096];
        if (transfer_exact(client, &request, sizeof(request), 0) != 0) return 0;
        if (request.magic != OPENRSP_PROTOCOL_MAGIC ||
            request.version != OPENRSP_PROTOCOL_VERSION ||
            request.payload_bytes > sizeof(payload) ||
            (request.payload_bytes != 0u &&
             transfer_exact(client, payload, request.payload_bytes, 0) != 0))
            return -1;
        if (request.type == OPENRSP_CMD_LIST) {
            atomic_fetch_add(&daemon->list_requests, 1u);
            if (send_device(client, request.sequence) != 0) return -1;
        } else if (request.type == OPENRSP_CMD_ACQUIRE) {
            atomic_fetch_add(&daemon->acquire_requests, 1u);
            if (request.payload_bytes != sizeof(openrsp_acquire_request) ||
                send_response(client, request.sequence) != 0) return -1;
        } else if (request.type == OPENRSP_CMD_CONFIGURE) {
            atomic_fetch_add(&daemon->configure_requests, 1u);
            if (request.payload_bytes != sizeof(openrsp_radio_config) ||
                send_response(client, request.sequence) != 0) return -1;
        } else if (request.type == OPENRSP_CMD_START) {
            atomic_fetch_add(&daemon->start_requests, 1u);
            if (send_response(client, request.sequence) != 0 || send_iq(client) != 0) return -1;
        } else if (request.type == OPENRSP_CMD_UPDATE) {
            atomic_fetch_add(&daemon->update_requests, 1u);
            if (request.payload_bytes != sizeof(openrsp_update_request) ||
                send_response(client, request.sequence) != 0) return -1;
        } else if (request.type == OPENRSP_CMD_STOP) {
            atomic_fetch_add(&daemon->stop_requests, 1u);
            if (send_response(client, request.sequence) != 0) return -1;
        } else if (request.type == OPENRSP_CMD_RELEASE) {
            atomic_fetch_add(&daemon->release_requests, 1u);
            return send_response(client, request.sequence);
        } else {
            return -1;
        }
    }
}

static void *mock_daemon_main(void *opaque)
{
    mock_daemon *daemon = opaque;
    while (atomic_load(&daemon->release_requests) == 0u) {
        SOCKET client = accept(daemon->listener, NULL, NULL);
        if (client == INVALID_SOCKET) {
            daemon->result = 1;
            return NULL;
        }
        if (serve_client(daemon, client) != 0) daemon->result = 2;
        (void)closesocket(client);
        if (daemon->result != 0) return NULL;
    }
    return NULL;
}

static atomic_uint callback_count;
static atomic_uint callback_samples;

static void stream_callback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params,
                            unsigned int samples, unsigned int reset, void *context)
{
    (void)reset;
    (void)context;
    assert(xi != NULL && xq != NULL && params != NULL);
    assert(samples == params->numSamples && samples != 0u);
    assert(xi[0] == -1024 && xq[0] == -1023);
    atomic_store(&callback_samples, samples);
    atomic_fetch_add(&callback_count, 1u);
}

static void event_callback(sdrplay_api_EventT event, sdrplay_api_TunerSelectT tuner,
                           sdrplay_api_EventParamsT *params, void *context)
{
    (void)event;
    (void)tuner;
    (void)params;
    (void)context;
}

int main(void)
{
    WSADATA winsock;
    assert(WSAStartup(MAKEWORD(2, 2), &winsock) == 0);
    const char *port_text = getenv("OPENRSPD_PORT");
    assert(port_text != NULL && port_text[0] != '\0');
    const unsigned long port = strtoul(port_text, NULL, 10);
    assert(port > 0ul && port <= 65535ul);

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(listener != INVALID_SOCKET);
    const struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons((u_short)port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
    };
    assert(bind(listener, (const struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(listener, 4) == 0);
    mock_daemon daemon = {.listener = listener};
    pthread_t daemon_thread;
    assert(pthread_create(&daemon_thread, NULL, mock_daemon_main, &daemon) == 0);

    assert(sdrplay_api_Open() == sdrplay_api_Success);
    sdrplay_api_DeviceT devices[SDRPLAY_MAX_DEVICES];
    unsigned int count = 0u;
    assert(sdrplay_api_GetDevices(devices, &count, SDRPLAY_MAX_DEVICES) == sdrplay_api_Success);
    assert(count == 1u && devices[0].hwVer == SDRPLAY_RSP1_ID && devices[0].valid == 1u);
    assert(strcmp(devices[0].SerNo, "WINDOWS-LIFECYCLE") == 0);
    assert(sdrplay_api_SelectDevice(&devices[0]) == sdrplay_api_Success);
    sdrplay_api_DeviceParamsT *params = NULL;
    assert(sdrplay_api_GetDeviceParams(devices[0].dev, &params) == sdrplay_api_Success);
    assert(params != NULL && params->rxChannelA != NULL);
    params->rxChannelA->ctrlParams.dcOffset.DCenable = 0u;
    params->rxChannelA->ctrlParams.dcOffset.IQenable = 0u;
    sdrplay_api_CallbackFnsT callbacks = {
        .StreamACbFn = stream_callback,
        .EventCbFn = event_callback
    };
    assert(sdrplay_api_Init(devices[0].dev, &callbacks, NULL) ==
           sdrplay_api_Success);
    for (unsigned int attempt = 0u; attempt < 100u && atomic_load(&callback_count) == 0u;
         ++attempt)
        Sleep(10u);
    assert(atomic_load(&callback_count) == 1u && atomic_load(&callback_samples) != 0u);
    assert(sdrplay_api_Uninit(devices[0].dev) == sdrplay_api_Success);
    assert(sdrplay_api_ReleaseDevice(&devices[0]) == sdrplay_api_Success);
    assert(sdrplay_api_Close() == sdrplay_api_Success);
    assert(pthread_join(daemon_thread, NULL) == 0);
    (void)closesocket(listener);
    assert(daemon.result == 0);
    assert(atomic_load(&daemon.list_requests) == 2u);
    assert(atomic_load(&daemon.acquire_requests) == 1u);
    assert(atomic_load(&daemon.configure_requests) == 1u);
    assert(atomic_load(&daemon.start_requests) == 1u);
    assert(atomic_load(&daemon.update_requests) >= 1u);
    assert(atomic_load(&daemon.stop_requests) == 1u);
    assert(atomic_load(&daemon.release_requests) == 1u);
    WSACleanup();
    puts("WINDOWS_SDRPLAY_API_LIFECYCLE_OK");
    return 0;
}
