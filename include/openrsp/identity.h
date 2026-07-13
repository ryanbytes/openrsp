/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OPENRSP_IDENTITY_H
#define OPENRSP_IDENTITY_H

#include "openrsp/protocol.h"
#include <stddef.h>
#include <stdint.h>

#define OPENRSP_IDENTITY_NOT_FOUND (-1)
#define OPENRSP_IDENTITY_AMBIGUOUS (-2)

int openrsp_identity_matches(const openrsp_acquire_request *identity,
                             const openrsp_device_record *candidate);
int openrsp_identity_resolve(const openrsp_acquire_request *identity,
                             const openrsp_device_record *devices, size_t count,
                             uint32_t *device_index);

#endif
