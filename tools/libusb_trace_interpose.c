/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <libusb.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

__attribute__((constructor)) static void trace_loaded(void)
{
    static const char marker[] = "openrsp libusb interposer loaded\n";
    int fd = open("/tmp/openrsp-usb-trace.loaded", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        (void)write(fd, marker, sizeof(marker) - 1u);
        close(fd);
    }
}

static void trace_line(const char *operation, int result, uint8_t request_type, uint8_t request,
                       uint16_t value, uint16_t index, const unsigned char *data, uint16_t length)
{
    char line[1024];
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    int used = snprintf(line, sizeof(line), "%lld.%09ld op=%s result=%d type=%02x request=%02x "
                        "value=%04x index=%04x length=%u data=",
                        (long long)now.tv_sec, now.tv_nsec, operation, result, request_type,
                        request, value, index, length);
    uint16_t captured = length < 64u ? length : 64u;
    for (uint16_t byte = 0; byte < captured && used > 0 && (size_t)used < sizeof(line) - 4u; ++byte) {
        used += snprintf(line + used, sizeof(line) - (size_t)used, "%02x", data == NULL ? 0u : data[byte]);
    }
    if (length > captured && used > 0 && (size_t)used < sizeof(line) - 4u) {
        used += snprintf(line + used, sizeof(line) - (size_t)used, "...");
    }
    if (used > 0 && (size_t)used < sizeof(line) - 1u) line[used++] = '\n';
    int fd = open("/tmp/openrsp-usb-trace.log", O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0) {
        (void)write(fd, line, (size_t)used);
        close(fd);
    }
}

int libusb_control_transfer(libusb_device_handle *device, uint8_t request_type,
                            uint8_t request, uint16_t value, uint16_t index,
                            unsigned char *data, uint16_t length, unsigned int timeout)
{
    typedef int (*function_t)(libusb_device_handle *, uint8_t, uint8_t, uint16_t, uint16_t,
                              unsigned char *, uint16_t, unsigned int);
    function_t original = (function_t)dlsym(RTLD_NEXT, "libusb_control_transfer");
    int result = original(device, request_type, request, value, index, data, length, timeout);
    trace_line("control", result, request_type, request, value, index, data,
               result > 0 && (request_type & LIBUSB_ENDPOINT_IN) != 0 ? (uint16_t)result : length);
    return result;
}

int libusb_claim_interface(libusb_device_handle *device, int interface_number)
{
    typedef int (*function_t)(libusb_device_handle *, int);
    function_t original = (function_t)dlsym(RTLD_NEXT, "libusb_claim_interface");
    int result = original(device, interface_number);
    trace_line("claim", result, 0, 0, 0, (uint16_t)interface_number, NULL, 0);
    return result;
}

int libusb_set_interface_alt_setting(libusb_device_handle *device, int interface_number,
                                     int alternate)
{
    typedef int (*function_t)(libusb_device_handle *, int, int);
    function_t original = (function_t)dlsym(RTLD_NEXT, "libusb_set_interface_alt_setting");
    int result = original(device, interface_number, alternate);
    trace_line("alt", result, 0, 0, (uint16_t)alternate, (uint16_t)interface_number, NULL, 0);
    return result;
}

int libusb_reset_device(libusb_device_handle *device)
{
    typedef int (*function_t)(libusb_device_handle *);
    function_t original = (function_t)dlsym(RTLD_NEXT, "libusb_reset_device");
    int result = original(device);
    trace_line("reset", result, 0, 0, 0, 0, NULL, 0);
    return result;
}

int libusb_submit_transfer(struct libusb_transfer *transfer)
{
    typedef int (*function_t)(struct libusb_transfer *);
    function_t original = (function_t)dlsym(RTLD_NEXT, "libusb_submit_transfer");
    int result = original(transfer);
    trace_line("submit", result, transfer == NULL ? 0u : transfer->type,
               transfer == NULL ? 0u : transfer->endpoint,
               transfer == NULL ? 0u : (uint16_t)transfer->length,
               transfer == NULL ? 0u : (uint16_t)transfer->timeout, NULL, 0);
    return result;
}

int libusb_cancel_transfer(struct libusb_transfer *transfer)
{
    typedef int (*function_t)(struct libusb_transfer *);
    function_t original = (function_t)dlsym(RTLD_NEXT, "libusb_cancel_transfer");
    int result = original(transfer);
    trace_line("cancel", result, transfer == NULL ? 0u : transfer->type,
               transfer == NULL ? 0u : transfer->endpoint,
               transfer == NULL ? 0u : (uint16_t)transfer->length,
               transfer == NULL ? 0u : (uint16_t)transfer->status, NULL, 0);
    return result;
}
