#define _POSIX_C_SOURCE 200809L

#include "openrsp/openrsp.h"

#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int product_is_present(uint16_t product_id)
{
    int count = openrsp_discover(NULL, 0);
    if (count <= 0) {
        return 0;
    }
    openrsp_device_info *devices = calloc((size_t)count, sizeof(*devices));
    if (devices == NULL) {
        return 0;
    }
    int discovered = openrsp_discover(devices, (size_t)count);
    int present = 0;
    for (int index = 0; index < discovered; ++index) {
        if (devices[index].product_id == product_id) {
            present = 1;
            break;
        }
    }
    free(devices);
    return present;
}

int main(int argc, char **argv)
{
    if (argc != 4 || strcmp(argv[1], "--product") != 0 ||
        strcmp(argv[3], "--disruptive-usb-reset") != 0) {
        fprintf(stderr, "Usage: %s --product HEX_PID --disruptive-usb-reset\n", argv[0]);
        return EXIT_FAILURE;
    }
    char *end = NULL;
    unsigned long parsed = strtoul(argv[2], &end, 16);
    if (end == argv[2] || *end != '\0' || parsed > 0xffffu) {
        fputs("Invalid hexadecimal product ID\n", stderr);
        return EXIT_FAILURE;
    }
    int result = openrsp_device_reset((uint16_t)parsed, 0);
    if (result == 0 || result == LIBUSB_ERROR_NO_DEVICE || result == LIBUSB_ERROR_NOT_FOUND) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 250000000L};
        int stable = 0;
        for (int attempt = 0; attempt < 24; ++attempt) {
            if (product_is_present((uint16_t)parsed)) {
                if (++stable >= 3) {
                    printf("RESET_OK device_stable=1 libusb_reset_result=%s\n",
                           result == 0 ? "SUCCESS" :
                           result == LIBUSB_ERROR_NO_DEVICE ? "NO_DEVICE" : "NOT_FOUND");
                    return EXIT_SUCCESS;
                }
            } else {
                stable = 0;
            }
            nanosleep(&delay, NULL);
        }
        fputs("RESET_FAIL device did not become stably discoverable\n", stderr);
        return EXIT_FAILURE;
    }
    if (result < 0) {
        fprintf(stderr, "RESET_FAIL code=%d name=%s\n", result, libusb_error_name(result));
        return EXIT_FAILURE;
    }
    return EXIT_FAILURE;
}
