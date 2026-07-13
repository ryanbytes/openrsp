/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "openrsp/identity.h"

#include <string.h>

static int terminated(const char serial[64])
{
    return memchr(serial, '\0', 64u) != NULL;
}

int openrsp_identity_matches(const openrsp_acquire_request *identity,
                             const openrsp_device_record *candidate)
{
    if (!identity || !candidate || !terminated(identity->serial) ||
        !terminated(candidate->serial) ||
        identity->vendor_id != candidate->vendor_id ||
        identity->product_id != candidate->product_id)
        return 0;
    if (identity->serial[0] != '\0')
        return strcmp(identity->serial, candidate->serial) == 0;
    return identity->device_index == candidate->device_index;
}

int openrsp_identity_resolve(const openrsp_acquire_request *identity,
                             const openrsp_device_record *devices, size_t count,
                             uint32_t *device_index)
{
    if (!identity || (!devices && count != 0u) || !device_index)
        return OPENRSP_IDENTITY_NOT_FOUND;
    int found = 0;
    uint32_t resolved = 0u;
    for (size_t index = 0u; index < count; ++index) {
        if (!openrsp_identity_matches(identity, &devices[index])) continue;
        if (found) return OPENRSP_IDENTITY_AMBIGUOUS;
        resolved = devices[index].device_index;
        found = 1;
    }
    if (!found) return OPENRSP_IDENTITY_NOT_FOUND;
    *device_index = resolved;
    return 0;
}
