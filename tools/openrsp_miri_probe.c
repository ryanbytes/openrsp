/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <mirisdr.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    int open_device = argc == 2 && strcmp(argv[1], "--open") == 0;
    if (argc > 2 || (argc == 2 && !open_device)) {
        fprintf(stderr, "usage: %s [--open]\n", argv[0]);
        return 2;
    }
    uint32_t count = mirisdr_get_device_count();
    printf("direct_backend_device_count=%u\n", count);
    for (uint32_t index = 0; index < count; ++index) {
        char manufacturer[256] = {0};
        char product[256] = {0};
        char serial[256] = {0};
        int result = mirisdr_get_device_usb_strings(index, manufacturer, product, serial);
        printf("device=%u name=%s manufacturer=%s product=%s descriptor_result=%d\n",
               index, mirisdr_get_device_name(index), manufacturer, product, result);
    }
    if (count > 0 && open_device) {
        mirisdr_dev_t *device = NULL;
        int result = mirisdr_open(&device, 0);
        printf("direct_backend_open=%d\n", result);
        if (result < 0) return 1;
        mirisdr_close(device);
    }
    return count == 0 ? 1 : 0;
}
