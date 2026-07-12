#include "openrsp/openrsp.h"

#include <libusb.h>
#include <stdlib.h>

struct openrsp_session {
    libusb_context *context;
    libusb_device_handle *handle;
    int claimed_interface;
};

int openrsp_session_open(uint16_t product_id, unsigned int match_index, openrsp_session **session)
{
    if (session == NULL) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    *session = NULL;

    openrsp_session *candidate = calloc(1, sizeof(*candidate));
    if (candidate == NULL) {
        return LIBUSB_ERROR_NO_MEM;
    }
    candidate->claimed_interface = -1;

    int result = libusb_init(&candidate->context);
    if (result < 0) {
        free(candidate);
        return result;
    }

    libusb_device **list = NULL;
    ssize_t count = libusb_get_device_list(candidate->context, &list);
    if (count < 0) {
        result = (int)count;
        goto fail;
    }

    unsigned int matched = 0;
    result = LIBUSB_ERROR_NO_DEVICE;
    for (ssize_t index = 0; index < count; ++index) {
        struct libusb_device_descriptor descriptor;
        int descriptor_result = libusb_get_device_descriptor(list[index], &descriptor);
        if (descriptor_result < 0 || descriptor.idVendor != OPENRSP_USB_VENDOR_ID ||
            descriptor.idProduct != product_id) {
            continue;
        }
        if (matched++ != match_index) {
            continue;
        }
        result = libusb_open(list[index], &candidate->handle);
        if (result == 0) {
            result = libusb_claim_interface(candidate->handle, 0);
            if (result == 0) {
                candidate->claimed_interface = 0;
            }
        }
        break;
    }
    libusb_free_device_list(list, 1);
    list = NULL;

    if (result < 0) {
        goto fail;
    }
    *session = candidate;
    return 0;

fail:
    if (list != NULL) {
        libusb_free_device_list(list, 1);
    }
    openrsp_session_close(candidate);
    return result;
}

void openrsp_session_close(openrsp_session *session)
{
    if (session == NULL) {
        return;
    }
    if (session->handle != NULL && session->claimed_interface >= 0) {
        (void)libusb_release_interface(session->handle, session->claimed_interface);
    }
    if (session->handle != NULL) {
        libusb_close(session->handle);
    }
    if (session->context != NULL) {
        libusb_exit(session->context);
    }
    free(session);
}

int openrsp_device_reset(uint16_t product_id, unsigned int match_index)
{
    libusb_context *context = NULL;
    libusb_device **list = NULL;
    libusb_device_handle *handle = NULL;
    int result = libusb_init(&context);
    if (result < 0) {
        return result;
    }

    ssize_t count = libusb_get_device_list(context, &list);
    if (count < 0) {
        result = (int)count;
        goto done;
    }

    unsigned int matched = 0;
    result = LIBUSB_ERROR_NO_DEVICE;
    for (ssize_t index = 0; index < count; ++index) {
        struct libusb_device_descriptor descriptor;
        int descriptor_result = libusb_get_device_descriptor(list[index], &descriptor);
        if (descriptor_result < 0 || descriptor.idVendor != OPENRSP_USB_VENDOR_ID ||
            descriptor.idProduct != product_id) {
            continue;
        }
        if (matched++ != match_index) {
            continue;
        }
        result = libusb_open(list[index], &handle);
        if (result == 0) {
            result = libusb_reset_device(handle);
        }
        break;
    }

done:
    if (handle != NULL) {
        libusb_close(handle);
    }
    if (list != NULL) {
        libusb_free_device_list(list, 1);
    }
    if (context != NULL) {
        libusb_exit(context);
    }
    return result;
}
