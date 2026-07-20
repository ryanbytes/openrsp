/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "windows_device_recovery.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <setupapi.h>
#endif

static int ascii_equal_n(const char *left, const char *right, size_t length)
{
    for (size_t index = 0u; index < length; ++index) {
        unsigned char left_char = (unsigned char)left[index];
        unsigned char right_char = (unsigned char)right[index];
        if (tolower(left_char) != tolower(right_char)) return 0;
    }
    return 1;
}

static int ascii_equal(const char *left, const char *right)
{
    if (!left || !right) return 0;
    size_t left_length = strlen(left);
    return left_length == strlen(right) &&
           ascii_equal_n(left, right, left_length);
}

int openrsp_windows_usb_instance_matches(const char *instance_id,
                                         uint16_t vendor_id,
                                         uint16_t product_id,
                                         const char *serial)
{
    if (!instance_id) return 0;
    char prefix[64];
    int written = snprintf(prefix, sizeof(prefix), "USB\\VID_%04X&PID_%04X\\",
                           vendor_id, product_id);
    if (written < 0 || (size_t)written >= sizeof(prefix)) return 0;
    size_t prefix_length = (size_t)written;
    if (strlen(instance_id) <= prefix_length ||
        !ascii_equal_n(instance_id, prefix, prefix_length))
        return 0;
    return !serial || serial[0] == '\0' ||
           ascii_equal(instance_id + prefix_length, serial);
}

int openrsp_windows_restart_usb_device(uint16_t vendor_id,
                                       uint16_t product_id,
                                       const char *serial)
{
#if defined(_WIN32)
    HDEVINFO devices = SetupDiGetClassDevsA(
        NULL, "USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (devices == INVALID_HANDLE_VALUE) return (int)GetLastError();
    SP_DEVINFO_DATA match = {.cbSize = sizeof(SP_DEVINFO_DATA)};
    unsigned int matches = 0u;
    for (DWORD index = 0u;; ++index) {
        SP_DEVINFO_DATA candidate = {.cbSize = sizeof(SP_DEVINFO_DATA)};
        if (!SetupDiEnumDeviceInfo(devices, index, &candidate)) {
            if (GetLastError() != ERROR_NO_MORE_ITEMS) {
                int error = (int)GetLastError();
                SetupDiDestroyDeviceInfoList(devices);
                return error;
            }
            break;
        }
        char instance_id[512];
        if (!SetupDiGetDeviceInstanceIdA(
                devices, &candidate, instance_id, (DWORD)sizeof(instance_id),
                NULL))
            continue;
        if (!openrsp_windows_usb_instance_matches(
                instance_id, vendor_id, product_id, serial))
            continue;
        match = candidate;
        ++matches;
    }
    if (matches != 1u) {
        SetupDiDestroyDeviceInfoList(devices);
        return matches == 0u ? -1 : -2;
    }
    SP_PROPCHANGE_PARAMS change;
    memset(&change, 0, sizeof(change));
    change.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    change.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    change.StateChange = DICS_PROPCHANGE;
    change.Scope = DICS_FLAG_GLOBAL;
    int result = 0;
    if (!SetupDiSetClassInstallParamsA(
            devices, &match, &change.ClassInstallHeader, sizeof(change)) ||
        !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, devices, &match))
        result = (int)GetLastError();
    SetupDiDestroyDeviceInfoList(devices);
    return result;
#else
    (void)vendor_id;
    (void)product_id;
    (void)serial;
    return -1;
#endif
}
