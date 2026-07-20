/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gain_values.h"

#include <assert.h>
#include <math.h>

static void expect_values(double rf_hz, unsigned int state, int gr,
                          float expected_current, float expected_maximum,
                          float expected_minimum)
{
    float current = 0.0f;
    float maximum = 0.0f;
    float minimum = 0.0f;
    assert(openrsp_rspduo_gain_values(rf_hz, state, gr, 20, 0, &current,
                                      &maximum, &minimum) == 0);
    assert(fabsf(current - expected_current) < 0.001f);
    assert(fabsf(maximum - expected_maximum) < 0.001f);
    assert(fabsf(minimum - expected_minimum) < 0.001f);
}

int main(void)
{
    expect_values(0.0, 0u, 20, 68.6600037f, 68.6600037f, -30.6000004f);
    expect_values(1.0, 0u, 20, 68.6600189f, 68.6600189f, -30.5999813f);
    expect_values(500.0, 0u, 20, 68.6677551f, 68.6677551f, -30.5904999f);
    expect_values(999.0, 0u, 20, 68.6754913f, 68.6754913f, -30.5810184f);
    expect_values(10000000.0, 0u, 20, 75.0749969f, 75.0749969f, -26.1549988f);
    expect_values(10000000.0, 6u, 59, -26.1549988f, 75.0749969f, -26.1549988f);
    expect_values(101100000.0, 0u, 20, 101.370598f, 101.370598f, -3.68279648f);
    expect_values(101100000.0, 9u, 59, -3.68279648f, 101.370598f, -3.68279648f);
    expect_values(853862500.0, 0u, 59, 56.7283096f, 95.7283096f, -9.07783127f);
    expect_values(1500000000.0, 8u, 59, -11.5900002f, 87.0722275f, -11.5900002f);

    /* Nearest-kHz rounding selects the next calibration range. */
    expect_values(59999499.0, 0u, 20, 71.5001602f, 71.5001602f, -31.4997635f);
    expect_values(59999500.0, 0u, 20, 101.410004f, 101.410004f, -4.18000031f);
    expect_values(999999499.0, 9u, 59, -10.519989f, 91.4600372f, -10.519989f);
    expect_values(999999500.0, 8u, 59, -14.6399994f, 81.3399963f, -14.6399994f);

    float value = 0.0f;
    float current = 0.0f;
    float maximum = 0.0f;
    float minimum = 0.0f;
    assert(openrsp_rspduo_gain_values(10000000.0, 4u, 59, 20, 1,
                                      &current, &maximum, &minimum) == 0);
    assert(fabsf(current - 7.37571716f) < 0.001f);
    assert(fabsf(maximum - 83.8971405f) < 0.001f);
    assert(fabsf(minimum - 7.37571716f) < 0.001f);
    assert(openrsp_rspduo_gain_values(0.0, 4u, 59, 20, 1,
                                      &current, &maximum, &minimum) == 0);
    assert(fabsf(current - 13.5199966f) < 0.001f);
    assert(openrsp_rspduo_gain_values(29999500.0, 0u, 20, 20, 1,
                                      &current, &maximum, &minimum) == 0);
    assert(fabsf(current - 77.580368f) < 0.001f);
    assert(openrsp_rspduo_gain_values(30000000.0, 4u, 59, 20, 1,
                                      &current, &maximum, &minimum) == 0);
    assert(current == 0.0f && maximum == 0.0f && minimum == 0.0f);
    assert(openrsp_rspduo_gain_values(10000000.0, 5u, 20, 20, 1,
                                      &current, &maximum, &minimum) < 0);
    assert(openrsp_rspduo_gain_values(-1.0, 0u, 20, 20, 0,
                                      &value, &value, &value) < 0);
    assert(openrsp_rspduo_gain_values(101100000.0, 10u, 20,
                                      20, 0, &value, &value, &value) < 0);
    assert(openrsp_rspduo_gain_values(101100000.0, 0u, 60,
                                      20, 0, &value, &value, &value) < 0);
    return 0;
}
