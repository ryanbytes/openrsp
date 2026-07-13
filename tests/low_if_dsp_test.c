/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "low_if_dsp.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

static void fill_tone(int16_t *xi, int16_t *xq, size_t samples, size_t offset,
                      double normalized)
{
    const double two_pi = 6.28318530717958647692;
    for (size_t index = 0u; index < samples; ++index) {
        double phase = two_pi * normalized * (double)(index + offset);
        xi[index] = (int16_t)lround(12000.0 * cos(phase));
        xq[index] = (int16_t)lround(12000.0 * sin(phase));
    }
}

int main(void)
{
    int16_t xi[6000];
    int16_t xq[6000];
    int16_t xi_second[2999];
    int16_t xq_second[2999];
    openrsp_low_if_dsp dsp;
    assert(openrsp_low_if_configure(&dsp, 6000000.0, 1620000) == 0);
    fill_tone(xi, xq, 3001u, 0u, 1620000.0 / 6000000.0);
    fill_tone(xi_second, xq_second, 2999u, 3001u,
              1620000.0 / 6000000.0);
    size_t output = openrsp_low_if_process(&dsp, xi, xq, 3001u);
    size_t second_output = openrsp_low_if_process(&dsp, xi_second, xq_second, 2999u);
    for (size_t index = 0u; index < second_output; ++index) {
        xi[output + index] = xi_second[index];
        xq[output + index] = xq_second[index];
    }
    output += second_output;
    assert(output == 2000u);
    long long mean_i = 0;
    long long mean_q = 0;
    for (size_t index = 100u; index < output; ++index) {
        mean_i += xi[index];
        mean_q += xq[index];
    }
    mean_i /= (long long)(output - 100u);
    mean_q /= (long long)(output - 100u);
    assert(llabs(mean_i - 12000) < 100);
    assert(llabs(mean_q) < 100);

    assert(openrsp_low_if_configure(&dsp, 6000000.0, 1620000) == 0);
    for (size_t index = 0u; index < 6000u; ++index) {
        xi[index] = 12000;
        xq[index] = 0;
    }
    output = openrsp_low_if_process(&dsp, xi, xq, 6000u);
    int stopband_peak = 0;
    for (size_t index = 100u; index < output; ++index) {
        int magnitude_i = abs(xi[index]);
        int magnitude_q = abs(xq[index]);
        if (magnitude_i > stopband_peak) stopband_peak = magnitude_i;
        if (magnitude_q > stopband_peak) stopband_peak = magnitude_q;
    }
    assert(stopband_peak < 100);

    assert(openrsp_low_if_configure(&dsp, 8000000.0, 2048000) == 0);
    fill_tone(xi, xq, 6000u, 0u, 2048000.0 / 8000000.0);
    assert(openrsp_low_if_process(&dsp, xi, xq, 6000u) == 1500u);
    assert(openrsp_low_if_configure(&dsp, 2000000.0, 0) == 0);
    assert(openrsp_low_if_process(&dsp, xi, xq, 6000u) == 6000u);
    assert(openrsp_low_if_configure(&dsp, 6000000.0, 450000) == 0);
    return EXIT_SUCCESS;
}
