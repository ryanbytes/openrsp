/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OPENRSP_PROTOCOL_H
#define OPENRSP_PROTOCOL_H

#include <stdatomic.h>
#include <stdint.h>

#define OPENRSP_PROTOCOL_MAGIC 0x4f525350u
#define OPENRSP_PROTOCOL_VERSION 7u
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
    OPENRSP_CMD_LOCK_API = 9,
    OPENRSP_CMD_UNLOCK_API = 10,
    OPENRSP_CMD_CONFIGURE_DUAL = 11,
    OPENRSP_CMD_SWAP_TUNER = 12,
    OPENRSP_CMD_SWAP_MODE = 13,
    OPENRSP_CMD_RESUME_MODE = 14,
    OPENRSP_MSG_RESPONSE = 0x8000,
    OPENRSP_EVENT_IQ = 0x8001,
    OPENRSP_EVENT_DEVICE = 0x8002,
    OPENRSP_EVENT_IQ_B = 0x8003
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
    uint16_t vendor_id;
    uint16_t product_id;
    char serial[64];
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
    uint32_t tuner;
    uint32_t bias_tee_enabled;
    uint32_t rf_notch_enabled;
    uint32_t dab_notch_enabled;
    uint32_t external_reference_enabled;
    uint32_t am_port_select;
    uint32_t am_notch_enabled;
} openrsp_radio_config;

#define OPENRSP_TUNER_A 1u
#define OPENRSP_TUNER_B 2u
#define OPENRSP_TUNER_BOTH 3u

typedef struct {
    uint32_t sample_rate_hz;
    uint32_t reserved;
    openrsp_radio_config channel_a;
    openrsp_radio_config channel_b;
} openrsp_dual_config;

#define OPENRSP_CHANGE_SAMPLE_RATE (1u << 0)
#define OPENRSP_CHANGE_RF          (1u << 1)
#define OPENRSP_CHANGE_BANDWIDTH   (1u << 2)
#define OPENRSP_CHANGE_IF          (1u << 3)
#define OPENRSP_CHANGE_GAIN        (1u << 4)
#define OPENRSP_CHANGE_AGC         (1u << 5)
#define OPENRSP_CHANGE_BIAS_TEE    (1u << 6)
#define OPENRSP_CHANGE_RF_NOTCH    (1u << 7)
#define OPENRSP_CHANGE_DAB_NOTCH   (1u << 8)
#define OPENRSP_CHANGE_EXT_REF     (1u << 9)
#define OPENRSP_CHANGE_AM_PORT     (1u << 10)
#define OPENRSP_CHANGE_AM_NOTCH    (1u << 11)
#define OPENRSP_CHANGE_RSPDUO_CONTROLS \
    (OPENRSP_CHANGE_BIAS_TEE | OPENRSP_CHANGE_RF_NOTCH | \
     OPENRSP_CHANGE_DAB_NOTCH | OPENRSP_CHANGE_EXT_REF | \
     OPENRSP_CHANGE_AM_PORT | OPENRSP_CHANGE_AM_NOTCH)

typedef struct {
    uint32_t changed_flags;
    uint32_t reserved;
    openrsp_radio_config config;
} openrsp_update_request;

typedef struct {
    uint32_t tuner;
    uint32_t reserved;
    openrsp_radio_config config;
} openrsp_swap_request;

#define OPENRSP_MODE_SINGLE 1u
#define OPENRSP_MODE_DUAL 2u

typedef struct {
    uint32_t mode;
    uint32_t reserved;
    openrsp_radio_config single;
    openrsp_dual_config dual;
} openrsp_mode_swap_request;

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
