/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OPENRSP_RSPDUO_ANALYTIC_H
#define OPENRSP_RSPDUO_ANALYTIC_H

#include <stddef.h>
#include <stdint.h>

#define MIRISDR_RSPDUO_HILBERT_TAPS 63u

typedef struct {
    double history[MIRISDR_RSPDUO_HILBERT_TAPS * 2u];
    double coefficients[MIRISDR_RSPDUO_HILBERT_TAPS];
    unsigned int position;
} mirisdr_rspduo_analytic_state;

void mirisdr_rspduo_analytic_reset(mirisdr_rspduo_analytic_state *state);
void mirisdr_rspduo_analytic_process(mirisdr_rspduo_analytic_state *state,
                                     int16_t *interleaved, size_t samples,
                                     unsigned int real_lane);

#endif
