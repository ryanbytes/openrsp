/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OPENRSP_MIRISDR_DEVICE_STATE_H
#define OPENRSP_MIRISDR_DEVICE_STATE_H

#include "mirisdr.h"

static inline int mirisdr_rspduo_descriptor_identity_state(
    uint16_t vendor_id, uint16_t product_id, uint8_t serial_index,
    int serial_read_result)
{
    if (vendor_id != 0x1df7u || product_id != 0x3020u)
        return MIRISDR_RSPDUO_IDENTITY_READY;
    if (serial_index == 0u) return MIRISDR_RSPDUO_IDENTITY_COLD;
    return serial_read_result > 0 ? MIRISDR_RSPDUO_IDENTITY_READY :
                                    MIRISDR_RSPDUO_IDENTITY_UNREADABLE;
}

#endif
