#include "openrsp/openrsp.h"

#include <libusb.h>
#include <stdio.h>
#include <stdlib.h>

static void json_string(const char *value)
{
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; ++p) {
        switch (*p) {
        case '"': fputs("\\\"", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        case '\b': fputs("\\b", stdout); break;
        case '\f': fputs("\\f", stdout); break;
        case '\n': fputs("\\n", stdout); break;
        case '\r': fputs("\\r", stdout); break;
        case '\t': fputs("\\t", stdout); break;
        default:
            if (*p < 0x20u) printf("\\u%04x", *p);
            else putchar(*p);
        }
    }
    putchar('"');
}
int main(void)
{
    int count = openrsp_discover(NULL, 0);
    if (count < 0) {
        fprintf(stderr, "USB discovery failed: %s\n", libusb_error_name(count));
        return EXIT_FAILURE;
    }

    openrsp_device_info *devices = count == 0 ? NULL : calloc((size_t)count, sizeof(*devices));
    if (count > 0 && devices == NULL) {
        fputs("Allocation failed\n", stderr);
        return EXIT_FAILURE;
    }
    int second_count = openrsp_discover(devices, (size_t)count);
    if (second_count < 0) {
        fprintf(stderr, "USB discovery failed: %s\n", libusb_error_name(second_count));
        free(devices);
        return EXIT_FAILURE;
    }

    puts("{");
    printf("  \"schema\": 1,\n  \"device_count\": %d,\n  \"devices\": [\n", second_count);
    for (int index = 0; index < second_count; ++index) {
        const openrsp_device_info *device = &devices[index];
        const char *model = device->model == NULL ? "unidentified SDRplay-vendor device" : device->model->model;
        openrsp_support_level support = device->model == NULL ? OPENRSP_SUPPORT_UNKNOWN : device->model->support;
        fputs("    {\"bus\": ", stdout); printf("%u, \"address\": %u, ", device->bus, device->address);
        printf("\"vid\": \"%04x\", \"pid\": \"%04x\", \"model\": ", device->vendor_id, device->product_id);
        json_string(model); fputs(", \"support\": ", stdout); json_string(openrsp_support_name(support));
        fputs(", \"manufacturer\": ", stdout); json_string(device->manufacturer);
        fputs(", \"product\": ", stdout); json_string(device->product);
        fputs(", \"serial\": ", stdout); json_string(device->serial);
        printf(", \"usb_class\": %u, \"configurations\": %u, \"descriptor_error\": %d, "
               "\"configuration_error\": %d, \"interface_count\": %u, \"endpoints\": [",
               device->usb_class, device->configuration_count, device->descriptor_error,
               device->configuration_error, device->interface_count);
        for (uint8_t endpoint_index = 0; endpoint_index < device->endpoint_count; ++endpoint_index) {
            const openrsp_endpoint_info *endpoint = &device->endpoints[endpoint_index];
            printf("%s{\"address\": \"%02x\", \"attributes\": \"%02x\", \"max_packet_size\": %u, "
                   "\"interval\": %u, \"interface\": %u, \"alternate\": %u}",
                   endpoint_index == 0 ? "" : ", ", endpoint->address, endpoint->attributes,
                   endpoint->max_packet_size, endpoint->interval, endpoint->interface_number,
                   endpoint->alternate_setting);
        }
        putchar(']');
        putchar('}');
        puts(index + 1 == second_count ? "" : ",");
    }
    puts("  ]\n}");
    free(devices);
    return EXIT_SUCCESS;
}
