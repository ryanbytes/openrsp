#include "openrsp/openrsp.h"

#include <libusb.h>
#include <string.h>

static void read_string(libusb_device_handle *handle, uint8_t index, char output[OPENRSP_TEXT_MAX])
{
    output[0] = '\0';
    if (handle == NULL || index == 0) {
        return;
    }
    int result = libusb_get_string_descriptor_ascii(handle, index, (unsigned char *)output,
                                                     OPENRSP_TEXT_MAX - 1u);
    if (result > 0) {
        output[result] = '\0';
    } else {
        output[0] = '\0';
    }
}
int openrsp_discover(openrsp_device_info *devices, size_t capacity)
{
    libusb_context *context = NULL;
    libusb_device **list = NULL;
    int result = libusb_init(&context);
    if (result < 0) {
        return result;
    }

    ssize_t list_count = libusb_get_device_list(context, &list);
    if (list_count < 0) {
        libusb_exit(context);
        return (int)list_count;
    }

    size_t matches = 0;
    for (ssize_t index = 0; index < list_count; ++index) {
        struct libusb_device_descriptor descriptor;
        result = libusb_get_device_descriptor(list[index], &descriptor);
        if (result < 0 || descriptor.idVendor != OPENRSP_USB_VENDOR_ID) {
            continue;
        }

        if (devices != NULL && matches < capacity) {
            openrsp_device_info *info = &devices[matches];
            memset(info, 0, sizeof(*info));
            info->bus = libusb_get_bus_number(list[index]);
            info->address = libusb_get_device_address(list[index]);
            info->vendor_id = descriptor.idVendor;
            info->product_id = descriptor.idProduct;
            info->usb_class = descriptor.bDeviceClass;
            info->configuration_count = descriptor.bNumConfigurations;
            info->model = openrsp_model_lookup(descriptor.idVendor, descriptor.idProduct);

            libusb_device_handle *handle = NULL;
            result = libusb_open(list[index], &handle);
            info->descriptor_error = result < 0 ? result : 0;
            if (result == 0) {
                read_string(handle, descriptor.iManufacturer, info->manufacturer);
                read_string(handle, descriptor.iProduct, info->product);
                read_string(handle, descriptor.iSerialNumber, info->serial);
                libusb_close(handle);
            }
        }
        ++matches;
    }

    libusb_free_device_list(list, 1);
    libusb_exit(context);
    return (int)matches;
}
