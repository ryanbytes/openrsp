/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "rspduo_analytic.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    const double pi = 3.14159265358979323846;
    const size_t count = 8192u;
    const double cycles_per_sample = 37.0 / 1024.0;
    int16_t samples[count * 2u];
    uint32_t noise = 1u;
    for (size_t index = 0u; index < count; ++index) {
        noise = noise * 1664525u + 1013904223u;
        samples[index * 2u] = (int16_t)(noise >> 17);
        samples[index * 2u + 1u] =
            (int16_t)lround(12000.0 * cos(2.0 * pi * cycles_per_sample * index));
    }

    mirisdr_rspduo_analytic_state state;
    mirisdr_rspduo_analytic_reset(&state);
    const size_t split = 1234u;
    mirisdr_rspduo_analytic_process(&state, samples, split, 1u);
    mirisdr_rspduo_analytic_process(&state, samples + split * 2u,
                                    count - split, 1u);

    double positive_i = 0.0;
    double positive_q = 0.0;
    double negative_i = 0.0;
    double negative_q = 0.0;
    for (size_t index = 128u; index < count; ++index) {
        double angle = 2.0 * pi * cycles_per_sample * (double)index;
        double i = samples[index * 2u];
        double q = samples[index * 2u + 1u];
        double cosine = cos(angle);
        double sine = sin(angle);
        positive_i += i * cosine + q * sine;
        positive_q += q * cosine - i * sine;
        negative_i += i * cosine - q * sine;
        negative_q += q * cosine + i * sine;
    }
    double positive_power = positive_i * positive_i + positive_q * positive_q;
    double negative_power = negative_i * negative_i + negative_q * negative_q;
    assert(positive_power > negative_power * 1000.0);
    puts("RSPDUO_ANALYTIC_IQ_OK");
    return 0;
}
