/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "low_if_dsp.h"

#include <math.h>
#include <string.h>

static int16_t clamp_sample(double value)
{
    if (value > 32767.0) return 32767;
    if (value < -32768.0) return -32768;
    return (int16_t)lround(value);
}

int openrsp_low_if_configure(openrsp_low_if_dsp *dsp, double sample_rate_hz,
                             int if_frequency_hz)
{
    const double pi = 3.14159265358979323846;
    if (dsp == NULL) return -1;
    unsigned int factor = 1u;
    if (sample_rate_hz == 6000000.0 && if_frequency_hz == 1620000)
        factor = 3u;
    else if (sample_rate_hz == 8000000.0 && if_frequency_hz == 2048000)
        factor = 4u;

    memset(dsp, 0, sizeof(*dsp));
    dsp->factor = factor;
    dsp->oscillator_cosine = 1.0;
    if (factor == 1u) return 0;
    double oscillator_step = 2.0 * pi * (double)if_frequency_hz / sample_rate_hz;
    dsp->oscillator_cosine_step = cos(oscillator_step);
    dsp->oscillator_sine_step = sin(oscillator_step);
    dsp->taps = 16u * factor + 1u;
    const double cutoff = 0.45 / (double)factor;
    const double center = ((double)dsp->taps - 1.0) / 2.0;
    double sum = 0.0;
    for (unsigned int index = 0u; index < dsp->taps; ++index) {
        double offset = (double)index - center;
        double sinc = offset == 0.0 ? 2.0 * cutoff :
                      sin(2.0 * pi * cutoff * offset) / (pi * offset);
        double window = 0.54 - 0.46 * cos(2.0 * pi * (double)index /
                                         (double)(dsp->taps - 1u));
        dsp->coefficients[index] = sinc * window;
        sum += dsp->coefficients[index];
    }
    for (unsigned int index = 0u; index < dsp->taps; ++index)
        dsp->coefficients[index] /= sum;
    return 0;
}

size_t openrsp_low_if_process(openrsp_low_if_dsp *dsp, int16_t *xi, int16_t *xq,
                              size_t samples)
{
    if (dsp == NULL || xi == NULL || xq == NULL) return 0u;
    if (dsp->factor <= 1u) return samples;
    size_t output = 0u;
    for (size_t input = 0u; input < samples; ++input) {
        double cosine = dsp->oscillator_cosine;
        double sine = dsp->oscillator_sine;
        double mixed_i = (double)xi[input] * cosine + (double)xq[input] * sine;
        double mixed_q = (double)xq[input] * cosine - (double)xi[input] * sine;
        dsp->oscillator_cosine = cosine * dsp->oscillator_cosine_step -
                                 sine * dsp->oscillator_sine_step;
        dsp->oscillator_sine = sine * dsp->oscillator_cosine_step +
                               cosine * dsp->oscillator_sine_step;
        if ((++dsp->oscillator_samples & 4095u) == 0u) {
            double magnitude = hypot(dsp->oscillator_cosine, dsp->oscillator_sine);
            dsp->oscillator_cosine /= magnitude;
            dsp->oscillator_sine /= magnitude;
        }

        dsp->history_i[dsp->position] = mixed_i;
        dsp->history_q[dsp->position] = mixed_q;
        dsp->position = (dsp->position + 1u) % dsp->taps;
        if (dsp->phase == 0u) {
            double sum_i = 0.0;
            double sum_q = 0.0;
            for (unsigned int tap = 0u; tap < dsp->taps; ++tap) {
                unsigned int history =
                    (dsp->position + dsp->taps - 1u - tap) % dsp->taps;
                sum_i += dsp->coefficients[tap] * dsp->history_i[history];
                sum_q += dsp->coefficients[tap] * dsp->history_q[history];
            }
            xi[output] = clamp_sample(sum_i);
            xq[output] = clamp_sample(sum_q);
            ++output;
        }
        dsp->phase = (dsp->phase + 1u) % dsp->factor;
    }
    return output;
}
