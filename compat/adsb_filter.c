/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "adsb_filter.h"

#include <math.h>
#include <string.h>

static int valid_mode(openrsp_adsb_mode mode)
{
    return mode >= OPENRSP_ADSB_DECIMATION && mode <= OPENRSP_ADSB_NO_DECIMATION_BANDPASS_3MHZ;
}

#define OPENRSP_ADSB_COEFFICIENT_SCALE 1073741824LL

static int16_t clamp_sample(int64_t scaled_value)
{
    int64_t value = scaled_value >= 0 ? scaled_value >> 30 :
                    -((-scaled_value) >> 30);
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return (int16_t)value;
}

int openrsp_adsb_filter_configure(openrsp_adsb_filter *filter,
                                  double sample_rate_hz,
                                  openrsp_adsb_mode mode)
{
    if (filter == NULL || !isfinite(sample_rate_hz) || sample_rate_hz <= 0.0 ||
        !valid_mode(mode)) return -1;
    memset(filter, 0, sizeof(*filter));
    filter->mode = mode;
    filter->sample_rate_hz = sample_rate_hz;
    if (mode == OPENRSP_ADSB_DECIMATION) {
        filter->taps = 1u;
        filter->coefficients[0] = (int32_t)OPENRSP_ADSB_COEFFICIENT_SCALE;
        return 0;
    }
    double bandwidth_hz = mode == OPENRSP_ADSB_NO_DECIMATION_BANDPASS_3MHZ ?
                          3000000.0 : 2000000.0;
    double nyquist_hz = sample_rate_hz * 0.5;
    /* A compact odd-length FIR keeps no-decimation processing viable at the
     * API's multi-megasample rates.  Duplicating the circular history below
     * makes each convolution contiguous and removes modulo from the hot loop. */
    filter->taps = OPENRSP_ADSB_MAX_TAPS;
    double cutoff_hz = bandwidth_hz * 0.5;
    double transition_hz = bandwidth_hz * 0.1;
    if (cutoff_hz + transition_hz >= nyquist_hz) return -1;
    double normalized_cutoff = (cutoff_hz + transition_hz) / sample_rate_hz;
    const double pi = 3.14159265358979323846;
    double center = ((double)filter->taps - 1.0) * 0.5;
    double sum = 0.0;
    for (unsigned int index = 0u; index < filter->taps; ++index) {
        double offset = (double)index - center;
        double sinc = offset == 0.0 ? 2.0 * normalized_cutoff :
                      sin(2.0 * pi * normalized_cutoff * offset) /
                      (pi * offset);
        double window = 0.42 - 0.5 * cos(2.0 * pi * (double)index /
                                         (double)(filter->taps - 1u)) +
                        0.08 * cos(4.0 * pi * (double)index /
                                   (double)(filter->taps - 1u));
        sum += sinc * window;
    }
    if (fabs(sum) < 1e-12) return -1;
    for (unsigned int index = 0u; index < filter->taps; ++index) {
        double offset = (double)index - center;
        double sinc = offset == 0.0 ? 2.0 * normalized_cutoff :
                      sin(2.0 * pi * normalized_cutoff * offset) /
                      (pi * offset);
        double window = 0.42 - 0.5 * cos(2.0 * pi * (double)index /
                                         (double)(filter->taps - 1u)) +
                        0.08 * cos(4.0 * pi * (double)index /
                                   (double)(filter->taps - 1u));
        filter->coefficients[index] = (int32_t)llround(
            sinc * window / sum * (double)OPENRSP_ADSB_COEFFICIENT_SCALE);
    }
    return 0;
}

void openrsp_adsb_filter_reset(openrsp_adsb_filter *filter)
{
    if (filter == NULL) return;
    memset(filter->history_i, 0, sizeof(filter->history_i));
    memset(filter->history_q, 0, sizeof(filter->history_q));
    filter->position = 0u;
}

size_t openrsp_adsb_filter_process(openrsp_adsb_filter *filter,
                                   int16_t *xi, int16_t *xq, size_t samples)
{
    if (filter == NULL || xi == NULL || xq == NULL) return 0u;
    if (filter->mode == OPENRSP_ADSB_DECIMATION) return samples;
    for (size_t index = 0u; index < samples; ++index) {
        filter->history_i[filter->position] = xi[index];
        filter->history_q[filter->position] = xq[index];
        filter->history_i[filter->position + filter->taps] = xi[index];
        filter->history_q[filter->position + filter->taps] = xq[index];
        filter->position = (filter->position + 1u) % filter->taps;
        int64_t sum_i = 0;
        int64_t sum_q = 0;
        for (unsigned int tap = 0u; tap < filter->taps; ++tap) {
            unsigned int history = filter->position + tap;
            sum_i += (int64_t)filter->coefficients[tap] *
                     filter->history_i[history];
            sum_q += (int64_t)filter->coefficients[tap] *
                     filter->history_q[history];
        }
        xi[index] = clamp_sample(sum_i);
        xq[index] = clamp_sample(sum_q);
    }
    return samples;
}
