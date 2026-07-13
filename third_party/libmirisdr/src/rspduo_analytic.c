/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "rspduo_analytic.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int16_t clamp_sample(double value)
{
    if (value > 32767.0) return 32767;
    if (value < -32768.0) return -32768;
    return (int16_t)value;
}

void mirisdr_rspduo_analytic_reset(mirisdr_rspduo_analytic_state *state)
{
    const double pi = 3.14159265358979323846;
    const int center = (int)(MIRISDR_RSPDUO_HILBERT_TAPS - 1u) / 2;
    if (state == NULL) return;
    memset(state, 0, sizeof(*state));
    for (unsigned int tap = 0u; tap < MIRISDR_RSPDUO_HILBERT_TAPS; ++tap) {
        int offset = (int)tap - center;
        if (offset == 0 || (abs(offset) & 1) == 0) continue;
        double window = 0.54 - 0.46 * cos(2.0 * pi * (double)tap /
                                         (double)(MIRISDR_RSPDUO_HILBERT_TAPS - 1u));
        state->coefficients[tap] = 2.0 * window / (pi * (double)offset);
    }
}

void mirisdr_rspduo_analytic_process(mirisdr_rspduo_analytic_state *state,
                                     int16_t *interleaved, size_t samples,
                                     unsigned int real_lane)
{
    const unsigned int center = (MIRISDR_RSPDUO_HILBERT_TAPS - 1u) / 2u;
    if (state == NULL || interleaved == NULL || real_lane > 1u) return;
    for (size_t sample = 0u; sample < samples; ++sample) {
        double real = interleaved[sample * 2u + real_lane];
        state->history[state->position] = real;
        state->history[state->position + MIRISDR_RSPDUO_HILBERT_TAPS] = real;
        if (++state->position == MIRISDR_RSPDUO_HILBERT_TAPS)
            state->position = 0u;
        double quadrature = 0.0;
        unsigned int newest = state->position + MIRISDR_RSPDUO_HILBERT_TAPS - 1u;
        /* The odd Hilbert impulse is antisymmetric. Pair its equal/opposite
         * taps and use a duplicated ring buffer: 16 multiplies and no modulo
         * operations per sample preserve the full 63-tap response. */
        for (unsigned int tap = 0u; tap < center; tap += 2u) {
            quadrature += state->coefficients[tap] *
                (state->history[newest - tap] - state->history[state->position + tap]);
        }
        interleaved[sample * 2u] = (int16_t)state->history[newest - center];
        interleaved[sample * 2u + 1u] = clamp_sample(quadrature);
    }
}
