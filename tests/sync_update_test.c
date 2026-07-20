/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sync_update.h"
#include <assert.h>

int main(void)
{
    openrsp_sync_update state;
    openrsp_sync_update_init(&state);
    openrsp_sync_update_schedule(&state, OPENRSP_SYNC_GAIN | OPENRSP_SYNC_RF, 100u, 10u);
    assert(openrsp_sync_update_due(&state, 99u) == 0u);
    assert(openrsp_sync_update_due(&state, 100u) == (OPENRSP_SYNC_GAIN | OPENRSP_SYNC_RF));
    openrsp_sync_update_consume(&state, OPENRSP_SYNC_GAIN, 100u);
    assert(openrsp_sync_update_due(&state, 100u) == OPENRSP_SYNC_RF);
    assert(openrsp_sync_update_due(&state, 110u) == (OPENRSP_SYNC_GAIN | OPENRSP_SYNC_RF));
    openrsp_sync_update_reset(&state, OPENRSP_SYNC_RF);
    assert(openrsp_sync_update_due(&state, 110u) == OPENRSP_SYNC_GAIN);
    openrsp_sync_update_schedule(&state, OPENRSP_SYNC_FS, UINT32_MAX - 2u, 5u);
    assert((openrsp_sync_update_due(&state, 1u) & OPENRSP_SYNC_FS) != 0u);
    openrsp_sync_update_schedule(&state, OPENRSP_SYNC_GAIN, 0u, 0u);
    assert((openrsp_sync_update_due(&state, 1000u) & OPENRSP_SYNC_GAIN) != 0u);
    openrsp_sync_update_consume(&state, OPENRSP_SYNC_GAIN, 1000u);
    assert((openrsp_sync_update_due(&state, 1000u) & OPENRSP_SYNC_GAIN) == 0u);
    openrsp_sync_update_schedule(&state, OPENRSP_SYNC_RF, 100u, 10u);
    uint32_t target = 0u;
    assert(openrsp_sync_update_next(&state, OPENRSP_SYNC_RF, 127u, &target) == 0);
    assert(target == 130u);
    return 0;
}
