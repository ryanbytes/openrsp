/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "adsb_filter.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#define TEST_PI 3.14159265358979323846

static void test_passthrough(void)
{
    openrsp_adsb_filter filter;
    int16_t xi[] = {1, -2, 300, -400};
    int16_t xq[] = {-5, 6, -700, 800};
    assert(openrsp_adsb_filter_configure(&filter, 2000000.0,
                                         OPENRSP_ADSB_DECIMATION) == 0);
    assert(openrsp_adsb_filter_process(&filter, xi, xq, 4u) == 4u);
    assert(xi[0] == 1 && xi[3] == -400 && xq[1] == 6 && xq[2] == -700);
}

static void test_lowpass_and_state(void)
{
    openrsp_adsb_filter filter;
    assert(openrsp_adsb_filter_configure(
               &filter, 8000000.0, OPENRSP_ADSB_NO_DECIMATION_LOWPASS) == 0);
    int16_t xi[8192];
    int16_t xq[8192];
    for (size_t i = 0; i < 8192u; ++i) {
        xi[i] = (int16_t)(12000.0 * sin(2.0 * TEST_PI * 250000.0 * (double)i /
                                        8000000.0));
        xq[i] = xi[i];
    }
    assert(openrsp_adsb_filter_process(&filter, xi, xq, 4096u) == 4096u);
    assert(openrsp_adsb_filter_process(&filter, xi + 4096, xq + 4096, 4096u) == 4096u);
    assert(fabs((double)xi[8191]) > 2000.0);
    openrsp_adsb_filter_reset(&filter);
    assert(filter.position == 0u && filter.history_i[0] == 0);
}

static void test_bandpass_rejects_out_of_band(void)
{
    openrsp_adsb_filter filter;
    int16_t xi[8192];
    int16_t xq[8192];
    for (int mode = OPENRSP_ADSB_NO_DECIMATION_BANDPASS_2MHZ;
         mode <= OPENRSP_ADSB_NO_DECIMATION_BANDPASS_3MHZ; ++mode) {
        assert(openrsp_adsb_filter_configure(
                   &filter, 8000000.0, (openrsp_adsb_mode)mode) == 0);
        double pass_hz = mode == OPENRSP_ADSB_NO_DECIMATION_BANDPASS_2MHZ ?
                         500000.0 : 1000000.0;
        for (size_t i = 0; i < 8192u; ++i) {
            xi[i] = (int16_t)(12000.0 * sin(2.0 * TEST_PI * pass_hz *
                                            (double)i / 8000000.0));
            xq[i] = 0;
        }
        (void)openrsp_adsb_filter_process(&filter, xi, xq, 8192u);
        long long pass_energy = 0;
        for (size_t i = 7168u; i < 8192u; ++i)
            pass_energy += (long long)xi[i] * xi[i];
        assert(pass_energy / 1024 > 10000000);
    }

    assert(openrsp_adsb_filter_configure(
               &filter, 8000000.0, OPENRSP_ADSB_NO_DECIMATION_BANDPASS_2MHZ) == 0);
    for (size_t i = 0; i < 8192u; ++i) {
        xi[i] = (int16_t)(12000.0 * sin(2.0 * TEST_PI * 2500000.0 * (double)i /
                                        8000000.0));
        xq[i] = 0;
    }
    (void)openrsp_adsb_filter_process(&filter, xi, xq, 8192u);
    assert(abs(xi[8191]) < 1000);
    assert(openrsp_adsb_filter_configure(&filter, 3000000.0,
                                         OPENRSP_ADSB_NO_DECIMATION_BANDPASS_3MHZ) == -1);
}

int main(void)
{
    test_passthrough();
    test_lowpass_and_state();
    test_bandpass_rejects_out_of_band();
    return 0;
}
