/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OPENRSPD_WINDOWS_DEVICE_RECOVERY_H
#define OPENRSPD_WINDOWS_DEVICE_RECOVERY_H

#include <stdint.h>

#define OPENRSP_WINDOWS_DEVICE_RESTART_NOT_FOUND (-1)
#define OPENRSP_WINDOWS_DEVICE_RESTART_AMBIGUOUS (-2)
#define OPENRSP_WINDOWS_DEVICE_RESTART_REBOOT_REQUIRED (-3)
#define OPENRSP_WINDOWS_DEVICE_RESTART_SYSTEM_RESTART_REQUIRED (-4)

int openrsp_windows_device_restart_requires_system_restart(int result);

int openrsp_windows_usb_instance_matches(const char *instance_id,
                                         uint16_t vendor_id,
                                         uint16_t product_id,
                                         const char *serial);
int openrsp_windows_restart_usb_device(uint16_t vendor_id,
                                       uint16_t product_id,
                                       const char *serial);

#endif
