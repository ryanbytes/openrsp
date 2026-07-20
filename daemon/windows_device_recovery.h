/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OPENRSPD_WINDOWS_DEVICE_RECOVERY_H
#define OPENRSPD_WINDOWS_DEVICE_RECOVERY_H

#include <stdint.h>

int openrsp_windows_usb_instance_matches(const char *instance_id,
                                         uint16_t vendor_id,
                                         uint16_t product_id,
                                         const char *serial);
int openrsp_windows_restart_usb_device(uint16_t vendor_id,
                                       uint16_t product_id,
                                       const char *serial);

#endif
