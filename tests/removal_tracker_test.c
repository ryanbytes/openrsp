/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "removal_tracker.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    openrspd_removal_tracker tracker = {0};
    assert(!openrspd_removal_tracker_absent(&tracker, 1000u, 5000u));
    assert(!openrspd_removal_tracker_absent(&tracker, 5999u, 5000u));
    assert(openrspd_removal_tracker_absent(&tracker, 6000u, 5000u));
    assert(!openrspd_removal_tracker_absent(&tracker, 9000u, 5000u));
    openrspd_removal_tracker_present(&tracker);
    assert(!openrspd_removal_tracker_absent(&tracker, 10000u, 5000u));
    assert(openrspd_removal_tracker_absent(&tracker, 15000u, 5000u));
    openrspd_removal_tracker_present(&tracker);
    assert(!openrspd_removal_tracker_absent(&tracker, 20000u, 5000u));
    assert(!openrspd_removal_tracker_absent(&tracker, 19000u, 5000u));
    assert(!openrspd_removal_tracker_absent(&tracker, 23999u, 5000u));
    assert(openrspd_removal_tracker_absent(&tracker, 24000u, 5000u));
    openrspd_removal_tracker_present(&tracker);
    assert(openrspd_removal_tracker_absent(&tracker, 30000u, 0u));
    puts("REMOVAL_TRACKER_OK");
    return 0;
}
