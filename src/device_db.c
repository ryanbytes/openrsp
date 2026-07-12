#include "openrsp/openrsp.h"

/*
 * IDs below are independently published by libmirisdr-5. A name is not a
 * claim of functional support. Unknown 0x1df7 devices remain discoverable.
 */
static const openrsp_model models[] = {
    {OPENRSP_USB_VENDOR_ID, 0x2500u, "Mirics MSi2500 / original RSP1-class", OPENRSP_SUPPORT_DISCOVERY_ONLY},
    {OPENRSP_USB_VENDOR_ID, 0x3000u, "SDRplay RSP1A", OPENRSP_SUPPORT_DISCOVERY_ONLY},
    {OPENRSP_USB_VENDOR_ID, 0x3010u, "SDRplay RSP2", OPENRSP_SUPPORT_DISCOVERY_ONLY},
    {OPENRSP_USB_VENDOR_ID, 0x3020u, "SDRplay RSPduo", OPENRSP_SUPPORT_DISCOVERY_ONLY},
};

const openrsp_model *openrsp_model_lookup(uint16_t vendor_id, uint16_t product_id)
{
    size_t count = sizeof(models) / sizeof(models[0]);
    for (size_t index = 0; index < count; ++index) {
        if (models[index].vendor_id == vendor_id && models[index].product_id == product_id) {
            return &models[index];
        }
    }
    return NULL;
}

const char *openrsp_support_name(openrsp_support_level support)
{
    switch (support) {
    case OPENRSP_SUPPORT_DISCOVERY_ONLY:
        return "discovery-only";
    case OPENRSP_SUPPORT_EXPERIMENTAL_STREAMING:
        return "experimental-streaming";
    case OPENRSP_SUPPORT_UNKNOWN:
    default:
        return "unknown";
    }
}
