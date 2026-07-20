/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OPENRSP_SYNC_UPDATE_H
#define OPENRSP_SYNC_UPDATE_H

#include <stdint.h>

/* Categories correspond to the API's gain, RF, and sample-rate updates. */
enum openrsp_sync_update_category {
    OPENRSP_SYNC_GAIN = 1u << 0,
    OPENRSP_SYNC_RF = 1u << 1,
    OPENRSP_SYNC_FS = 1u << 2
};

typedef struct {
    uint32_t pending;
    uint32_t due[3];
    uint32_t period[3];
} openrsp_sync_update;

void openrsp_sync_update_init(openrsp_sync_update *state);
/* period zero disables scheduling; otherwise the first update is due at sample. */
void openrsp_sync_update_schedule(openrsp_sync_update *state, uint32_t categories,
                                  uint32_t sample, uint32_t period);
void openrsp_sync_update_reset(openrsp_sync_update *state, uint32_t categories);
/* Advances missed periodic boundaries and returns the next boundary. */
int openrsp_sync_update_next(openrsp_sync_update *state, uint32_t category,
                             uint32_t current_sample, uint32_t *target_sample);
/* Returns categories due at sample, without consuming them. */
uint32_t openrsp_sync_update_due(const openrsp_sync_update *state, uint32_t sample);
void openrsp_sync_update_consume(openrsp_sync_update *state, uint32_t categories,
                                 uint32_t sample);

#endif
