#include "openrsp/openrsp.h"

#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *program)
{
    fprintf(stderr, "Usage: %s --product HEX_PID --offline-test-system --i-understand-this-claims-the-usb-interface\n",
            program);
}

int main(int argc, char **argv)
{
    if (argc != 5 || strcmp(argv[1], "--product") != 0 ||
        strcmp(argv[3], "--offline-test-system") != 0 ||
        strcmp(argv[4], "--i-understand-this-claims-the-usb-interface") != 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    char *end = NULL;
    unsigned long parsed = strtoul(argv[2], &end, 16);
    if (end == argv[2] || *end != '\0' || parsed > 0xffffu) {
        fputs("Invalid hexadecimal product ID\n", stderr);
        return EXIT_FAILURE;
    }

    openrsp_session *session = NULL;
    int result = openrsp_session_open((uint16_t)parsed, 0, &session);
    if (result < 0) {
        fprintf(stderr, "OPEN_FAIL code=%d name=%s\n", result, libusb_error_name(result));
        return result == LIBUSB_ERROR_BUSY ? 2 : EXIT_FAILURE;
    }

    puts("OPEN_OK interface=0 writes=0 alternate_setting_changes=0");
    openrsp_session_close(session);
    puts("CLOSE_OK");
    return EXIT_SUCCESS;
}
