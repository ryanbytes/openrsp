/*
 * Copyright (C) 2013 by Miroslav Slugen <thunder.m@email.cz
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

static mirisdr_device_t mirisdr_devices[] = {
    { 0x1df7, 0x2500, "Mirics MSi2500 default (e.g. VTX3D card)", "Mirics", "MSi2500"},
    { 0x1df7, 0x3000, "SDRplay RSP1A", "SDRPlay", "RSP1A"},
    { 0x1df7, 0x3010, "SDRplay RSP2", "SDRPlay", "RSP2"},
    /* OpenRSP: added 2026-07-12 for direct RSPduo transport development. */
    { 0x1df7, 0x3020, "SDRplay RSPduo (experimental tuner A)", "SDRPlay", "RSPduo"},
    { 0x2040, 0xd300, "Hauppauge WinTV 133559 LF", "Hauppauge", "WinTV 133559 LF"},
    { 0x07ca, 0x8591, "AverMedia A859 Pure DVBT", "AverTV", "A859 Pure DVBT"},
    { 0x04bb, 0x0537, "IO-DATA GV-TV100 stick", "IO-DATA", "GV-TV100"},
    { 0x0511, 0x0037, "Logitec LDT-1S310U/J", "Logitec", "LDT-1S310U/J"}
};

static mirisdr_device_t *mirisdr_device_get (uint16_t vid, uint16_t pid) {
    size_t i;

    for (i = 0; i < sizeof(mirisdr_devices) / sizeof(mirisdr_device_t); i++) {
        if ((mirisdr_devices[i].vid == vid) && (mirisdr_devices[i].pid == pid)) return &mirisdr_devices[i];
    }

    return NULL;
}

static int mirisdr_rspduo_serial_is_readable(
    libusb_device_handle *handle,
    const struct libusb_device_descriptor *descriptor)
{
    unsigned char serial[256];
    if (!handle || !descriptor || descriptor->idVendor != 0x1df7u ||
        descriptor->idProduct != 0x3020u || descriptor->iSerialNumber == 0u)
        return 0;
    return libusb_get_string_descriptor_ascii(
               handle, descriptor->iSerialNumber, serial,
               (int)sizeof(serial) - 1) > 0;
}

/* počet dostupných zařízení */
uint32_t mirisdr_get_device_count (void) {
    ssize_t i, i_max;
    uint32_t ret = 0;
    libusb_context *ctx;
    libusb_device **list;
    struct libusb_device_descriptor dd;

#ifdef __ANDROID__
    /* LibUSB does not support device discovery on android */
    libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY, NULL);
#endif

    libusb_init(&ctx);

    i_max = libusb_get_device_list(ctx, &list);

    for (i = 0; i < i_max; i++) {
        libusb_get_device_descriptor(list[i], &dd);

        if (mirisdr_device_get(dd.idVendor, dd.idProduct)) ret++;
    }

    libusb_free_device_list(list, 1);

    libusb_exit(ctx);

    return ret;
}

/* název zařízení */
const char *mirisdr_get_device_name (uint32_t index) {
    ssize_t i, i_max;
    size_t j = 0;
    libusb_context *ctx;
    libusb_device **list;
    struct libusb_device_descriptor dd;
    mirisdr_device_t *device = NULL;

#ifdef __ANDROID__
    /* LibUSB does not support device discovery on android */
    libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY, NULL);
#endif

    libusb_init(&ctx);
    i_max = libusb_get_device_list(ctx, &list);

    for (i = 0; i < i_max; i++) {
        libusb_get_device_descriptor(list[i], &dd);

        if ((device = mirisdr_device_get(dd.idVendor, dd.idProduct)) &&
            (j++ == index)) {
            libusb_free_device_list(list, 1);
            libusb_exit(ctx);
            return device->name;
        }
    }

    libusb_free_device_list(list, 1);
    libusb_exit(ctx);

    return "";
}

int mirisdr_get_device_info (uint32_t index, uint16_t *vendor_id,
                             uint16_t *product_id, char *manufact,
                             char *product, char *serial) {
    ssize_t i, i_max;
    size_t j = 0;
    libusb_context *ctx;
    libusb_device **list;
    struct libusb_device_descriptor dd;
    mirisdr_device_t *device = NULL;

    if (!manufact || !product || !serial) return -1;
    if (vendor_id) *vendor_id = 0u;
    if (product_id) *product_id = 0u;
    memset(manufact, 0, 256);
    memset(product, 0, 256);
    memset(serial, 0, 256);

#ifdef __ANDROID__
    /* LibUSB does not support device discovery on android */
    libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY, NULL);
#endif

    if (libusb_init(&ctx) != 0) return -1;
    i_max = libusb_get_device_list(ctx, &list);
    if (i_max < 0) {
        libusb_exit(ctx);
        return -1;
    }

    for (i = 0; i < i_max; i++) {
        libusb_get_device_descriptor(list[i], &dd);

        if ((device = mirisdr_device_get(dd.idVendor, dd.idProduct)) &&
            (j++ == index)) {
            if (vendor_id) *vendor_id = dd.idVendor;
            if (product_id) *product_id = dd.idProduct;
            strcpy(manufact, device->manufacturer);
            strcpy(product, device->product);

            /* Genuine RSP receivers expose a stable USB serial descriptor.
             * Preserve it for application configuration identity.  Older
             * Mirics-compatible devices without one retain the physical-port
             * fallback below. */
            if (dd.iSerialNumber != 0u) {
                libusb_device_handle *handle = NULL;
                if (libusb_open(list[i], &handle) == 0) {
                    int length = libusb_get_string_descriptor_ascii(
                        handle, dd.iSerialNumber, (unsigned char *)serial, 255);
                    libusb_close(handle);
                    if (length > 0) {
                        serial[length] = '\0';
                        libusb_free_device_list(list, 1);
                        libusb_exit(ctx);
                        return 0;
                    }
                }
            }

            /* No readable serial: use the physical USB location as a stable
             * identity only while the receiver remains on the same port. */

            char *serial_cursor = serial;
            size_t serial_remaining = 256u;
            int written = snprintf(serial_cursor, serial_remaining, "%u:",
                                   libusb_get_bus_number(list[i]));
            if (written < 0 || (size_t)written >= serial_remaining) goto serial_failed;
            serial_cursor += (size_t)written;
            serial_remaining -= (size_t)written;

#if LIBUSBX_API_VERSION >= 0x01000102 
            uint8_t usb_path[16];
            int path_len = libusb_get_port_numbers(list[i], usb_path, sizeof(usb_path));
            if (path_len == LIBUSB_ERROR_OVERFLOW) { // array too small
                path_len = sizeof(usb_path);
            }

            for (int u = 0; u < path_len; u++) {
                written = snprintf(serial_cursor, serial_remaining, "%u.", usb_path[u]);
                if (written < 0 || (size_t)written >= serial_remaining) goto serial_failed;
                serial_cursor += (size_t)written;
                serial_remaining -= (size_t)written;
            }
            
#endif
            *(serial_cursor - 1) = '\0'; // remove last dot or :

            libusb_free_device_list(list, 1);
            libusb_exit(ctx);
            return 0;

serial_failed:
            serial[0] = '\0';
            libusb_free_device_list(list, 1);
            libusb_exit(ctx);
            return -1;
        }
    }

    libusb_free_device_list(list, 1);
    libusb_exit(ctx);

    return -1;
}

int mirisdr_device_requires_firmware (uint32_t index) {
    ssize_t i, count;
    size_t matched = 0u;
    libusb_context *ctx = NULL;
    libusb_device **list = NULL;
    struct libusb_device_descriptor descriptor;
    int result = -1;

    if (libusb_init(&ctx) != 0) return -1;
    count = libusb_get_device_list(ctx, &list);
    if (count < 0) goto done;
    for (i = 0; i < count; ++i) {
        if (libusb_get_device_descriptor(list[i], &descriptor) != 0 ||
            mirisdr_device_get(descriptor.idVendor, descriptor.idProduct) == NULL)
            continue;
        if (matched++ != index) continue;
        if (descriptor.idVendor == 0x1df7u &&
            descriptor.idProduct == 0x3020u) {
            libusb_device_handle *handle = NULL;
            if (libusb_open(list[i], &handle) == 0) {
                result = mirisdr_rspduo_serial_is_readable(
                             handle, &descriptor) ? 0 : 1;
                libusb_close(handle);
            } else {
                result = 1;
            }
        } else {
            result = 0;
        }
        break;
    }

done:
    if (list) libusb_free_device_list(list, 1);
    if (ctx) libusb_exit(ctx);
    return result;
}

/* vlastní implementace */
int mirisdr_get_device_usb_strings (uint32_t index, char *manufact,
                                    char *product, char *serial) {
    return mirisdr_get_device_info(index, NULL, NULL, manufact, product, serial);
}
