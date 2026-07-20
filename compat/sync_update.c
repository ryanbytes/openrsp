/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sync_update.h"

#include <stddef.h>

static unsigned int index_for(uint32_t category)
{
    return category == OPENRSP_SYNC_GAIN ? 0u :
           category == OPENRSP_SYNC_RF ? 1u : 2u;
}

void openrsp_sync_update_init(openrsp_sync_update *state)
{
    if (state == NULL) return;
    *state = (openrsp_sync_update){0};
}

void openrsp_sync_update_schedule(openrsp_sync_update *state, uint32_t categories,
                                  uint32_t sample, uint32_t period)
{
    if (state == NULL) return;
    const uint32_t known = OPENRSP_SYNC_GAIN | OPENRSP_SYNC_RF | OPENRSP_SYNC_FS;
    categories &= known;
    for (uint32_t bit = OPENRSP_SYNC_GAIN; bit <= OPENRSP_SYNC_FS; bit <<= 1u) {
        if ((categories & bit) == 0u) continue;
        unsigned int index = index_for(bit);
        state->period[index] = period;
        state->due[index] = sample;
        state->pending |= bit;
    }
}

int openrsp_sync_update_next(openrsp_sync_update *state, uint32_t category,
                             uint32_t current_sample, uint32_t *target_sample)
{
    if (state == NULL || target_sample == NULL ||
        (category != OPENRSP_SYNC_GAIN && category != OPENRSP_SYNC_RF &&
         category != OPENRSP_SYNC_FS) || (state->pending & category) == 0u)
        return -1;
    unsigned int index = index_for(category);
    uint32_t target = state->due[index];
    uint32_t period = state->period[index];
    if (period != 0u && (int32_t)(current_sample - target) >= 0) {
        uint32_t elapsed = current_sample - target;
        target += (elapsed / period + 1u) * period;
        state->due[index] = target;
    }
    *target_sample = target;
    return 0;
}

void openrsp_sync_update_reset(openrsp_sync_update *state, uint32_t categories)
{
    if (state == NULL) return;
    categories &= (OPENRSP_SYNC_GAIN | OPENRSP_SYNC_RF | OPENRSP_SYNC_FS);
    state->pending &= ~categories;
    /* Reset flags cancel the active schedule; callers can explicitly schedule
     * a new period after the reset. */
    for (uint32_t bit = OPENRSP_SYNC_GAIN; bit <= OPENRSP_SYNC_FS; bit <<= 1u)
        if ((categories & bit) != 0u) state->period[index_for(bit)] = 0u;
}

uint32_t openrsp_sync_update_due(const openrsp_sync_update *state, uint32_t sample)
{
    if (state == NULL) return 0u;
    uint32_t result = 0u;
    for (uint32_t bit = OPENRSP_SYNC_GAIN; bit <= OPENRSP_SYNC_FS; bit <<= 1u) {
        unsigned int index = index_for(bit);
        if ((state->pending & bit) != 0u &&
            (int32_t)(sample - state->due[index]) >= 0)
            result |= bit;
    }
    return result;
}

void openrsp_sync_update_consume(openrsp_sync_update *state, uint32_t categories,
                                 uint32_t sample)
{
    if (state == NULL) return;
    categories &= openrsp_sync_update_due(state, sample);
    for (uint32_t bit = OPENRSP_SYNC_GAIN; bit <= OPENRSP_SYNC_FS; bit <<= 1u) {
        if ((categories & bit) == 0u) continue;
        unsigned int index = index_for(bit);
        state->pending &= ~bit;
        if (state->period[index] != 0u) {
            state->due[index] += state->period[index];
            state->pending |= bit;
        } else {
            state->pending &= ~bit;
        }
    }
}
