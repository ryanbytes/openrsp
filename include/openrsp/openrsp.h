#ifndef OPENRSP_OPENRSP_H
#define OPENRSP_OPENRSP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
#define OPENRSP_USB_VENDOR_ID 0x1df7u
#define OPENRSP_TEXT_MAX 256u

typedef enum {
    OPENRSP_SUPPORT_UNKNOWN = 0,
    OPENRSP_SUPPORT_DISCOVERY_ONLY,
    OPENRSP_SUPPORT_EXPERIMENTAL_STREAMING
} openrsp_support_level;

typedef struct {
    uint16_t vendor_id;
    uint16_t product_id;
    const char *model;
    openrsp_support_level support;
} openrsp_model;

typedef struct {
    uint8_t bus;
    uint8_t address;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t usb_class;
    uint8_t configuration_count;
    char manufacturer[OPENRSP_TEXT_MAX];
    char product[OPENRSP_TEXT_MAX];
    char serial[OPENRSP_TEXT_MAX];
    const openrsp_model *model;
    int descriptor_error;
} openrsp_device_info;

const openrsp_model *openrsp_model_lookup(uint16_t vendor_id, uint16_t product_id);
const char *openrsp_support_name(openrsp_support_level support);

/* Returns the number of matching SDRplay-vendor devices, or a negative libusb error. */
int openrsp_discover(openrsp_device_info *devices, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
