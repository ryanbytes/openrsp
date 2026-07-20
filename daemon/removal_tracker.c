/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "removal_tracker.h"

#include <stddef.h>

void openrspd_removal_tracker_present(openrspd_removal_tracker *tracker)
{
    if (tracker == NULL) return;
    tracker->absent_since_ms = 0u;
    tracker->absence_active = 0;
    tracker->removal_notified = 0;
}

int openrspd_removal_tracker_absent(openrspd_removal_tracker *tracker,
                                    uint64_t now_ms, uint64_t grace_ms)
{
    if (tracker == NULL) return 0;
    if (!tracker->absence_active) {
        tracker->absent_since_ms = now_ms;
        tracker->absence_active = 1;
        if (grace_ms == 0u && !tracker->removal_notified) {
            tracker->removal_notified = 1;
            return 1;
        }
        return 0;
    }
    if (now_ms < tracker->absent_since_ms) {
        /* Defensive fallback for callers that cannot provide a monotonic
         * clock: restart the grace window instead of unsigned underflow. */
        tracker->absent_since_ms = now_ms;
        return 0;
    }
    if (!tracker->removal_notified &&
        now_ms - tracker->absent_since_ms >= grace_ms) {
        tracker->removal_notified = 1;
        return 1;
    }
    return 0;
}
