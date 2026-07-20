/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "iq_correction.h"

#include <math.h>
#include <string.h>

/* A slow estimator avoids audible modulation while converging in a bounded
 * number of IQ frames.  It also keeps all state in the caller-owned object. */
#define OPENRSP_IQ_ESTIMATOR_ALPHA (1.0 / 256.0)

static void reset_estimates(openrsp_iq_correction *correction)
{
    correction->dc_i = 0.0;
    correction->dc_q = 0.0;
    correction->power_i = 0.0;
    correction->power_q = 0.0;
    correction->cross_iq = 0.0;
    correction->leakage = 0.0;
    correction->gain = 1.0;
    correction->coefficient_samples = 0u;
    correction->samples_since_refresh = 0u;
}

void openrsp_iq_correction_reset(openrsp_iq_correction *correction)
{
    if (correction == NULL) return;
    int dc_enabled = correction->dc_enabled;
    int iq_enabled = correction->iq_enabled;
    reset_estimates(correction);
    correction->dc_enabled = dc_enabled;
    correction->iq_enabled = iq_enabled;
}

int openrsp_iq_correction_configure(openrsp_iq_correction *correction,
                                    int dc_enable, int iq_enable)
{
    if (correction == NULL || (dc_enable != 0 && dc_enable != 1) ||
        (iq_enable != 0 && iq_enable != 1)) return -1;
    memset(correction, 0, sizeof(*correction));
    correction->dc_enabled = dc_enable;
    correction->iq_enabled = iq_enable;
    correction->dc_cal = 3u;
    correction->track_time = 1;
    correction->refresh_rate_time = 2048;
    correction->dc_alpha = OPENRSP_IQ_ESTIMATOR_ALPHA;
    return 0;
}

int openrsp_iq_correction_configure_calibration(
    openrsp_iq_correction *correction, double sample_rate_hz,
    unsigned char dc_cal, unsigned char speed_up,
    int track_time, int refresh_rate_time)
{
    if (correction == NULL || !isfinite(sample_rate_hz) || sample_rate_hz <= 0.0 ||
        speed_up > 1u || track_time <= 0 ||
        refresh_rate_time <= 0)
        return -1;
    correction->dc_cal = dc_cal;
    correction->speed_up = speed_up;
    correction->track_time = track_time;
    correction->refresh_rate_time = refresh_rate_time;
    correction->sample_rate_hz = sample_rate_hz;
    double tracking_us = (double)(dc_cal == 0u ? 1u : dc_cal) * 3.0 *
                         (double)track_time;
    double tracking_samples = sample_rate_hz * tracking_us / 1000000.0;
    if (tracking_samples < 16.0) tracking_samples = 16.0;
    correction->calibration_window_samples = (uint64_t)tracking_samples;
    correction->dc_alpha = (speed_up != 0u ? 4.0 : 1.0) / tracking_samples;
    if (correction->dc_alpha > 0.25) correction->dc_alpha = 0.25;
    double refresh_us = (double)(dc_cal == 0u ? 1u : dc_cal) * 3.0 *
                        (double)refresh_rate_time;
    correction->refresh_samples = (uint64_t)(sample_rate_hz * refresh_us / 1000000.0);
    if (correction->refresh_samples < 1u) correction->refresh_samples = 1u;
    reset_estimates(correction);
    return 0;
}

static int16_t clamp_sample(double value)
{
    if (value > 32767.0) return 32767;
    if (value < -32768.0) return -32768;
    return (int16_t)lround(value);
}

size_t openrsp_iq_correction_process(openrsp_iq_correction *correction,
                                     int16_t *xi, int16_t *xq, size_t samples)
{
    if (correction == NULL || xi == NULL || xq == NULL) return 0u;
    for (size_t index = 0u; index < samples; ++index) {
        double input_i = (double)xi[index];
        double input_q = (double)xq[index];
        if (correction->dc_enabled) {
            if (correction->dc_cal != 0u && correction->refresh_samples != 0u &&
                ++correction->samples_since_refresh >= correction->refresh_samples) {
                correction->samples_since_refresh = 0u;
            }
            double alpha = correction->dc_cal == 0u ? 0.0 : correction->dc_alpha;
            if (correction->dc_cal != 0u &&
                correction->samples_since_refresh > correction->calibration_window_samples)
                alpha = 0.0;
            correction->dc_i += alpha *
                                (input_i - correction->dc_i);
            correction->dc_q += alpha *
                                (input_q - correction->dc_q);
        } else {
            correction->dc_i = 0.0;
            correction->dc_q = 0.0;
        }
        double output_i = input_i - correction->dc_i;
        double output_q = input_q - correction->dc_q;
        if (correction->iq_enabled) {
            correction->power_i += OPENRSP_IQ_ESTIMATOR_ALPHA *
                                   (output_i * output_i - correction->power_i);
            correction->power_q += OPENRSP_IQ_ESTIMATOR_ALPHA *
                                   (output_q * output_q - correction->power_q);
            correction->cross_iq += OPENRSP_IQ_ESTIMATOR_ALPHA *
                                    (output_i * output_q - correction->cross_iq);
            ++correction->coefficient_samples;
            if ((correction->coefficient_samples & 255u) == 0u &&
                correction->power_i > 1.0 &&
                correction->power_q > 1.0) {
                /* Remove quadrature leakage first, then equalize Q RMS to I.
                 * Recompute coefficients once per 256 samples instead of
                 * performing a square root for every sample. */
                correction->leakage =
                    correction->cross_iq / correction->power_i;
                correction->gain =
                    sqrt(correction->power_i / correction->power_q);
                if (correction->gain < 0.25) correction->gain = 0.25;
                if (correction->gain > 4.0) correction->gain = 4.0;
            }
            output_q = (output_q - correction->leakage * output_i) *
                       correction->gain;
        } else {
            correction->power_i = 0.0;
            correction->power_q = 0.0;
            correction->cross_iq = 0.0;
            correction->leakage = 0.0;
            correction->gain = 1.0;
            correction->coefficient_samples = 0u;
        }
        xi[index] = clamp_sample(output_i);
        xq[index] = clamp_sample(output_q);
    }
    return samples;
}
