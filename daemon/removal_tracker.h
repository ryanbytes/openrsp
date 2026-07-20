/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OPENRSPD_REMOVAL_TRACKER_H
#define OPENRSPD_REMOVAL_TRACKER_H

#include <stdint.h>

typedef struct {
    uint64_t absent_since_ms;
    int absence_active;
    int removal_notified;
} openrspd_removal_tracker;

void openrspd_removal_tracker_present(openrspd_removal_tracker *tracker);
int openrspd_removal_tracker_absent(openrspd_removal_tracker *tracker,
                                    uint64_t now_ms, uint64_t grace_ms);

#endif
