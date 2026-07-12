/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <mirisdr.h>

#include <stdint.h>
#include <stdio.h>

int main(void)
{
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
    return count == 0 ? 1 : 0;
}
