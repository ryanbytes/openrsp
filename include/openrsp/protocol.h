/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OPENRSP_PROTOCOL_H
#define OPENRSP_PROTOCOL_H

#include <stdatomic.h>
#include <stdint.h>

#define OPENRSP_PROTOCOL_MAGIC 0x4f525350u
#define OPENRSP_PROTOCOL_VERSION 1u
#define OPENRSP_SOCKET_PATH "/var/run/openrspd.sock"
#define OPENRSP_MAX_IQ_SAMPLES 65536u

typedef enum {
    OPENRSP_CMD_PING = 1,
    OPENRSP_CMD_LIST = 2,
    OPENRSP_CMD_ACQUIRE = 3,
    OPENRSP_CMD_RELEASE = 4,
    OPENRSP_CMD_CONFIGURE = 5,
    OPENRSP_CMD_START = 6,
    OPENRSP_CMD_STOP = 7,
    OPENRSP_CMD_UPDATE = 8,
    OPENRSP_MSG_RESPONSE = 0x8000,
    OPENRSP_EVENT_IQ = 0x8001,
    OPENRSP_EVENT_DEVICE = 0x8002
} openrsp_command_type;

typedef enum {
    OPENRSP_STATUS_OK = 0,
    OPENRSP_STATUS_BAD_REQUEST = 1,
    OPENRSP_STATUS_UNSUPPORTED = 2,
    OPENRSP_STATUS_BUSY = 3,
    OPENRSP_STATUS_IO_ERROR = 4
} openrsp_status;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t sequence;
    uint32_t payload_bytes;
} openrsp_message_header;

typedef struct {
    uint32_t device_index;
    uint32_t reserved;
} openrsp_acquire_request;

typedef struct {
    uint32_t device_index;
    uint16_t vendor_id;
    uint16_t product_id;
    char serial[64];
    char model[64];
} openrsp_device_record;

typedef struct {
    uint32_t sample_rate_hz;
    uint32_t center_frequency_hz;
    uint32_t bandwidth_hz;
    int32_t if_frequency_hz;
    int32_t gain_reduction_db;
    uint32_t lna_state;
    int32_t agc_mode;
    int32_t agc_setpoint_dbfs;
} openrsp_radio_config;

#define OPENRSP_CHANGE_SAMPLE_RATE (1u << 0)
#define OPENRSP_CHANGE_RF          (1u << 1)
#define OPENRSP_CHANGE_BANDWIDTH   (1u << 2)
#define OPENRSP_CHANGE_IF          (1u << 3)
#define OPENRSP_CHANGE_GAIN        (1u << 4)
#define OPENRSP_CHANGE_AGC         (1u << 5)

typedef struct {
    uint32_t changed_flags;
    uint32_t reserved;
    openrsp_radio_config config;
} openrsp_update_request;

typedef struct {
    uint32_t status;
    uint32_t sequence;
    uint32_t changed_flags;
    uint32_t reserved;
} openrsp_response;

#define OPENRSP_RESPONSE_RECOVERY_QUEUED (1u << 31)

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t slot_count;
    uint32_t samples_per_slot;
    atomic_uint producer_sequence;
    atomic_uint consumer_sequence;
    atomic_uint dropped_slots;
    atomic_uint stream_state;
} openrsp_ring_header;

#endif
