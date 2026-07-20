/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OPENRSP_ADSB_FILTER_H
#define OPENRSP_ADSB_FILTER_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    OPENRSP_ADSB_DECIMATION = 0,
    OPENRSP_ADSB_NO_DECIMATION_LOWPASS = 1,
    OPENRSP_ADSB_NO_DECIMATION_BANDPASS_2MHZ = 2,
    OPENRSP_ADSB_NO_DECIMATION_BANDPASS_3MHZ = 3
} openrsp_adsb_mode;

#define OPENRSP_ADSB_MAX_TAPS 17u

typedef struct {
    openrsp_adsb_mode mode;
    double sample_rate_hz;
    unsigned int taps;
    unsigned int position;
    int32_t coefficients[OPENRSP_ADSB_MAX_TAPS];
    int16_t history_i[OPENRSP_ADSB_MAX_TAPS * 2u];
    int16_t history_q[OPENRSP_ADSB_MAX_TAPS * 2u];
} openrsp_adsb_filter;

int openrsp_adsb_filter_configure(openrsp_adsb_filter *filter,
                                  double sample_rate_hz,
                                  openrsp_adsb_mode mode);
void openrsp_adsb_filter_reset(openrsp_adsb_filter *filter);
size_t openrsp_adsb_filter_process(openrsp_adsb_filter *filter,
                                   int16_t *xi, int16_t *xq, size_t samples);

#endif
