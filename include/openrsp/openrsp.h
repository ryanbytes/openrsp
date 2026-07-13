#ifndef OPENRSP_OPENRSP_H
#define OPENRSP_OPENRSP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
#define OPENRSP_USB_VENDOR_ID 0x1df7u
#define OPENRSP_TEXT_MAX 256u
#define OPENRSP_MAX_INTERFACES 16u
#define OPENRSP_MAX_ENDPOINTS 32u

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
    uint8_t address;
    uint8_t attributes;
    uint16_t max_packet_size;
    uint8_t interval;
    uint8_t interface_number;
    uint8_t alternate_setting;
} openrsp_endpoint_info;

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
    char physical_path[OPENRSP_TEXT_MAX];
    const openrsp_model *model;
    int descriptor_error;
    int configuration_error;
    uint8_t interface_count;
    uint8_t endpoint_count;
    openrsp_endpoint_info endpoints[OPENRSP_MAX_ENDPOINTS];
} openrsp_device_info;

typedef struct openrsp_session openrsp_session;

const openrsp_model *openrsp_model_lookup(uint16_t vendor_id, uint16_t product_id);
const char *openrsp_support_name(openrsp_support_level support);

/* Returns the number of matching SDRplay-vendor devices, or a negative libusb error. */
int openrsp_discover(openrsp_device_info *devices, size_t capacity);

/* Opens the indexed VID 0x1df7/PID match and claims interface 0 without detaching anything. */
int openrsp_session_open(uint16_t product_id, unsigned int match_index, openrsp_session **session);
void openrsp_session_close(openrsp_session *session);

/* Claims interface 0 and performs a disruptive USB port-level reset. */
int openrsp_device_reset(uint16_t product_id, unsigned int match_index);

#ifdef __cplusplus
}
#endif

#endif
