/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "iq_correction.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void test_dc_removal(void)
{
    openrsp_iq_correction correction;
    assert(openrsp_iq_correction_configure(&correction, 1, 0) == 0);
    int16_t xi[4096];
    int16_t xq[4096];
    for (size_t i = 0; i < 4096u; ++i) {
        xi[i] = (int16_t)(1200 + (i & 1u ? 80 : -80));
        xq[i] = (int16_t)(-700 + (i & 1u ? 40 : -40));
    }
    assert(openrsp_iq_correction_process(&correction, xi, xq, 4096u) == 4096u);
    assert(abs(xi[4095]) < 120);
    assert(abs(xq[4095]) < 120);
}

static void test_iq_gain_and_reset(void)
{
    openrsp_iq_correction correction;
    assert(openrsp_iq_correction_configure(&correction, 0, 1) == 0);
    int16_t xi[8192];
    int16_t xq[8192];
    static const int16_t in_i[] = {1000, 0, -1000, 0};
    static const int16_t in_q[] = {0, 2000, 0, -2000};
    for (size_t i = 0; i < 8192u; ++i) {
        xi[i] = in_i[i & 3u];
        xq[i] = in_q[i & 3u];
    }
    assert(openrsp_iq_correction_process(&correction, xi, xq, 8192u) == 8192u);
    assert(abs(abs(xi[8188]) - abs(xq[8189])) < 120);
    openrsp_iq_correction_reset(&correction);
    assert(correction.iq_enabled == 1 && correction.power_i == 0.0);
    assert(openrsp_iq_correction_configure(&correction, 2, 0) == -1);
}

static void test_frame_boundaries_do_not_change_output(void)
{
    openrsp_iq_correction whole;
    openrsp_iq_correction split;
    assert(openrsp_iq_correction_configure(&whole, 1, 1) == 0);
    assert(openrsp_iq_correction_configure(&split, 1, 1) == 0);
    assert(openrsp_iq_correction_configure_calibration(
               &whole, 2000000.0, 2u, 1u, 200, 2048) == 0);
    assert(openrsp_iq_correction_configure_calibration(
               &split, 2000000.0, 2u, 1u, 200, 2048) == 0);
    int16_t whole_i[2048];
    int16_t whole_q[2048];
    int16_t split_i[2048];
    int16_t split_q[2048];
    for (size_t index = 0u; index < 2048u; ++index) {
        whole_i[index] = (int16_t)(900 + (int)(index % 31u) * 70 - 1050);
        whole_q[index] = (int16_t)(-400 + (int)(index % 17u) * 110);
    }
    memcpy(split_i, whole_i, sizeof(whole_i));
    memcpy(split_q, whole_q, sizeof(whole_q));
    assert(openrsp_iq_correction_process(&whole, whole_i, whole_q, 2048u) ==
           2048u);
    size_t offset = 0u;
    while (offset < 2048u) {
        size_t chunk = (offset % 173u) + 1u;
        if (chunk > 2048u - offset) chunk = 2048u - offset;
        assert(openrsp_iq_correction_process(
                   &split, split_i + offset, split_q + offset, chunk) == chunk);
        offset += chunk;
    }
    assert(memcmp(whole_i, split_i, sizeof(whole_i)) == 0);
    assert(memcmp(whole_q, split_q, sizeof(whole_q)) == 0);
}

static void test_periodic_dc_modes_freeze_and_refresh(void)
{
    for (unsigned char mode = 1u; mode <= 2u; ++mode) {
        openrsp_iq_correction correction;
        assert(openrsp_iq_correction_configure(&correction, 1, 0) == 0);
        assert(openrsp_iq_correction_configure_calibration(
                   &correction, 1000000.0, mode, 0u, 1, 10) == 0);
        int16_t xi[80];
        int16_t xq[80] = {0};
        for (size_t index = 0u; index < 20u; ++index) xi[index] = 1000;
        (void)openrsp_iq_correction_process(&correction, xi, xq, 20u);
        double frozen = correction.dc_i;
        for (size_t index = 0u; index < 5u; ++index) xi[index] = 2000;
        (void)openrsp_iq_correction_process(&correction, xi, xq, 5u);
        assert(correction.dc_i == frozen);
        for (size_t index = 0u; index < 80u; ++index) xi[index] = 2000;
        (void)openrsp_iq_correction_process(&correction, xi, xq, 80u);
        assert(correction.dc_i > frozen);
    }
}

int main(void)
{
    test_dc_removal();
    test_iq_gain_and_reset();
    test_frame_boundaries_do_not_change_output();
    test_periodic_dc_modes_freeze_and_refresh();
    return 0;
}
