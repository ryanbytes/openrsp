/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OPENRSP_LOW_IF_DSP_H
#define OPENRSP_LOW_IF_DSP_H

#include <stddef.h>
#include <stdint.h>

#define OPENRSP_LOW_IF_MAX_TAPS 65u

typedef struct {
    unsigned int factor;
    unsigned int taps;
    unsigned int position;
    unsigned int phase;
    double oscillator_cosine;
    double oscillator_sine;
    double oscillator_cosine_step;
    double oscillator_sine_step;
    unsigned int oscillator_samples;
    double coefficients[OPENRSP_LOW_IF_MAX_TAPS];
    double history_i[OPENRSP_LOW_IF_MAX_TAPS];
    double history_q[OPENRSP_LOW_IF_MAX_TAPS];
} openrsp_low_if_dsp;

int openrsp_low_if_configure(openrsp_low_if_dsp *dsp, double sample_rate_hz,
                             int if_frequency_hz);
size_t openrsp_low_if_process(openrsp_low_if_dsp *dsp, int16_t *xi, int16_t *xq,
                              size_t samples);

#endif
