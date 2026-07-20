/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OPENRSP_IQ_CORRECTION_H
#define OPENRSP_IQ_CORRECTION_H

#include <stddef.h>
#include <stdint.h>

/* Stateful, allocation-free correction for one complex IQ stream. */
typedef struct {
    int dc_enabled;
    int iq_enabled;
    double dc_i;
    double dc_q;
    double power_i;
    double power_q;
    double cross_iq;
    double leakage;
    double gain;
    uint64_t coefficient_samples;
    unsigned char dc_cal;
    unsigned char speed_up;
    int track_time;
    int refresh_rate_time;
    double sample_rate_hz;
    double dc_alpha;
    uint64_t samples_since_refresh;
    uint64_t calibration_window_samples;
    uint64_t refresh_samples;
} openrsp_iq_correction;

void openrsp_iq_correction_reset(openrsp_iq_correction *correction);
int openrsp_iq_correction_configure(openrsp_iq_correction *correction,
                                    int dc_enable, int iq_enable);
int openrsp_iq_correction_configure_calibration(
    openrsp_iq_correction *correction, double sample_rate_hz,
    unsigned char dc_cal, unsigned char speed_up,
    int track_time, int refresh_rate_time);
size_t openrsp_iq_correction_process(openrsp_iq_correction *correction,
                                     int16_t *xi, int16_t *xq, size_t samples);

#endif
