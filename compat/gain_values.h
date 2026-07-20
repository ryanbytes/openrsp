/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef OPENRSP_GAIN_VALUES_H
#define OPENRSP_GAIN_VALUES_H

int openrsp_rspduo_gain_values(double rf_hz, unsigned int lna_state,
                               int gain_reduction_db,
                               int minimum_gain_reduction_db,
                               int high_impedance_input, float *current,
                               float *maximum, float *minimum);

#endif
