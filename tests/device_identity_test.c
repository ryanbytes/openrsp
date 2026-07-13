/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "openrsp/identity.h"

#include <assert.h>
#include <string.h>

int main(void)
{
    assert(sizeof(openrsp_acquire_request) == 72u);
    const openrsp_acquire_request selected = {
        .device_index = 13u, .vendor_id = 0x1df7u, .product_id = 0x3020u,
        .serial = "RSP-STABLE"
    };
    openrsp_device_record devices[] = {
        {.device_index = 1u, .vendor_id = 0x1df7u, .product_id = 0x3010u,
         .serial = "RSP-STABLE"},
        {.device_index = 2u, .vendor_id = 0x1df7u, .product_id = 0x3020u,
         .serial = "OTHER"},
        {.device_index = 7u, .vendor_id = 0x1df7u, .product_id = 0x3020u,
         .serial = "RSP-STABLE"}
    };
    uint32_t index = 99u;
    assert(openrsp_identity_matches(&selected, &devices[2]) == 1);
    assert(openrsp_identity_matches(&selected, &devices[0]) == 0);
    assert(openrsp_identity_resolve(&selected, devices,
                                    sizeof(devices) / sizeof(devices[0]),
                                    &index) == 0);
    assert(index == 7u);

    devices[1] = devices[2];
    devices[1].device_index = 5u;
    index = 99u;
    assert(openrsp_identity_resolve(&selected, devices,
                                    sizeof(devices) / sizeof(devices[0]),
                                    &index) == OPENRSP_IDENTITY_AMBIGUOUS);
    assert(index == 99u);

    openrsp_acquire_request no_serial = selected;
    memset(no_serial.serial, 0, sizeof(no_serial.serial));
    devices[1].device_index = 13u;
    memset(devices[1].serial, 0, sizeof(devices[1].serial));
    assert(openrsp_identity_resolve(&no_serial, devices,
                                    sizeof(devices) / sizeof(devices[0]),
                                    &index) == 0);
    assert(index == 13u);

    no_serial.device_index = 42u;
    assert(openrsp_identity_resolve(&no_serial, devices,
                                    sizeof(devices) / sizeof(devices[0]),
                                    &index) == OPENRSP_IDENTITY_NOT_FOUND);

    openrsp_acquire_request malformed = selected;
    memset(malformed.serial, 'x', sizeof(malformed.serial));
    assert(openrsp_identity_matches(&malformed, &devices[2]) == 0);
    memset(devices[2].serial, 'x', sizeof(devices[2].serial));
    assert(openrsp_identity_matches(&selected, &devices[2]) == 0);
    return 0;
}
