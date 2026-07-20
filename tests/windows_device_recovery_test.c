/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "windows_device_recovery.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    const char *instance = "USB\\VID_1DF7&PID_3020\\SERIAL-TEST";
    assert(openrsp_windows_usb_instance_matches(
        instance, 0x1df7u, 0x3020u, "SERIAL-TEST"));
    assert(openrsp_windows_usb_instance_matches(
        "usb\\vid_1df7&pid_3020\\serial-test",
        0x1df7u, 0x3020u, "SERIAL-TEST"));
    assert(openrsp_windows_usb_instance_matches(
        instance, 0x1df7u, 0x3020u, NULL));
    assert(!openrsp_windows_usb_instance_matches(
        instance, 0x1df7u, 0x3020u, "OTHER"));
    assert(!openrsp_windows_usb_instance_matches(
        instance, 0x1df7u, 0x3011u, "SERIAL-TEST"));
    assert(!openrsp_windows_usb_instance_matches(
        "USB\\VID_1DF7&PID_3020&MI_00\\SERIAL-TEST",
        0x1df7u, 0x3020u, "SERIAL-TEST"));
    puts("WINDOWS_DEVICE_RECOVERY_MATCH_OK");
    return 0;
}
