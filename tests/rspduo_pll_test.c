/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "mirisdr.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define PLAN(reg, first, a, final) {reg, first, a, final}

static void assert_gain_band(uint32_t frequency,
                             const mirisdr_rspduo_gain_plan_t *tuner_a,
                             const mirisdr_rspduo_gain_plan_t *tuner_b,
                             size_t count)
{
    uint16_t previous_b = 0u;
    for (size_t state = 0u; state < count; state++) {
        mirisdr_rspduo_gain_plan_t actual = {0};
        assert(mirisdr_rspduo_gain_plan(frequency, 1u, 45, (unsigned int)state,
                                        0u, 0u, 0u, 0u, &actual) == 0);
        assert(actual.reg9 == tuner_a[state].reg9);
        assert(actual.first_gpio_4b == tuner_a[state].first_gpio_4b);
        assert(actual.gpio_4a == tuner_a[state].gpio_4a);
        assert(actual.final_gpio_4b == tuner_a[state].final_gpio_4b);
        assert(mirisdr_rspduo_gain_plan(frequency, 2u, 45, (unsigned int)state,
                                        previous_b, 0u, 0u, 0u, &actual) == 0);
        assert(actual.reg9 == tuner_b[state].reg9);
        assert(actual.first_gpio_4b == tuner_b[state].first_gpio_4b);
        assert(actual.gpio_4a == tuner_b[state].gpio_4a);
        assert(actual.final_gpio_4b == tuner_b[state].final_gpio_4b);
        previous_b = actual.final_gpio_4b;
    }
    mirisdr_rspduo_gain_plan_t invalid = {0};
    assert(mirisdr_rspduo_gain_plan(frequency, 1u, 45, (unsigned int)count,
                                    0u, 0u, 0u, 0u, &invalid) < 0);
}

static void assert_gain_plans(void)
{
    static const mirisdr_rspduo_gain_plan_t low_a[] = {
        PLAN(0xc2d1, 0x12ff, 0x12a2, 0x13ff),
        PLAN(0xc2d1, 0x12ff, 0x12a2, 0x13fb),
        PLAN(0xc2d1, 0x12ff, 0x12a2, 0x13f7),
        PLAN(0xc2d1, 0x12ff, 0x12a2, 0x13ef),
        PLAN(0xd2d1, 0x12ff, 0x12a2, 0x13ef),
        PLAN(0xced1, 0x12ff, 0x12a2, 0x13ef),
        PLAN(0xded1, 0x12ff, 0x12a2, 0x13ef),
    };
    static const mirisdr_rspduo_gain_plan_t low_b[] = {
        PLAN(0xc2d1, 0x13ff, 0x13c4, 0x13ff),
        PLAN(0xc2d1, 0x13ff, 0x13c4, 0x13df),
        PLAN(0xc2d1, 0x13df, 0x13c4, 0x13bf),
        PLAN(0xc2d1, 0x13bf, 0x13c4, 0x137f),
        PLAN(0xd2d1, 0x137f, 0x13c4, 0x137f),
        PLAN(0xced1, 0x137f, 0x13c4, 0x137f),
        PLAN(0xded1, 0x137f, 0x13c4, 0x137f),
    };
    static const mirisdr_rspduo_gain_plan_t mid_a[] = {
        PLAN(0xc2d1, 0x12df, 0x12a4, 0x13ff),
        PLAN(0xc2d1, 0x12df, 0x12a4, 0x13fb),
        PLAN(0xc2d1, 0x12df, 0x12a4, 0x13f7),
        PLAN(0xc2d1, 0x12df, 0x12a4, 0x13ef),
        PLAN(0xc2d1, 0x12ff, 0x12a6, 0x13ff),
        PLAN(0xc2d1, 0x12ff, 0x12a6, 0x13fb),
        PLAN(0xc2d1, 0x12ff, 0x12a6, 0x13f7),
        PLAN(0xc2d1, 0x12ff, 0x12a6, 0x13ef),
        PLAN(0xd2d1, 0x12ff, 0x12a6, 0x13ef),
        PLAN(0xe2d1, 0x12ff, 0x12a6, 0x13ef),
    };
    static const mirisdr_rspduo_gain_plan_t mid_b[] = {
        PLAN(0xc2d1, 0x13fe, 0x13c8, 0x13fe),
        PLAN(0xc2d1, 0x13fe, 0x13c8, 0x13de),
        PLAN(0xc2d1, 0x13de, 0x13c8, 0x13be),
        PLAN(0xc2d1, 0x13be, 0x13c8, 0x137e),
        PLAN(0xc2d1, 0x137f, 0x13cc, 0x13ff),
        PLAN(0xc2d1, 0x13ff, 0x13cc, 0x13df),
        PLAN(0xc2d1, 0x13df, 0x13cc, 0x13bf),
        PLAN(0xc2d1, 0x13bf, 0x13cc, 0x137f),
        PLAN(0xd2d1, 0x137f, 0x13cc, 0x137f),
        PLAN(0xe2d1, 0x137f, 0x13cc, 0x137f),
    };
    static const mirisdr_rspduo_gain_plan_t uhf_a[] = {
        PLAN(0xc2d1, 0x12df, 0x12a4, 0x13ff),
        PLAN(0xe2d1, 0x12df, 0x12a4, 0x13ff),
        PLAN(0xe2d1, 0x12df, 0x12a4, 0x13fb),
        PLAN(0xe2d1, 0x12df, 0x12a4, 0x13f7),
        PLAN(0xc2d1, 0x12ff, 0x12a6, 0x13ff),
        PLAN(0xe2d1, 0x12ff, 0x12a6, 0x13ff),
        PLAN(0xe2d1, 0x12ff, 0x12a6, 0x13fb),
        PLAN(0xe2d1, 0x12ff, 0x12a6, 0x13f7),
        PLAN(0xe2d1, 0x12ff, 0x12a6, 0x13ef),
        PLAN(0xf2d1, 0x12ff, 0x12a6, 0x13ef),
    };
    static const mirisdr_rspduo_gain_plan_t uhf_b[] = {
        PLAN(0xc2d1, 0x13fe, 0x13c8, 0x13fe),
        PLAN(0xe2d1, 0x13fe, 0x13c8, 0x13fe),
        PLAN(0xe2d1, 0x13fe, 0x13c8, 0x13de),
        PLAN(0xe2d1, 0x13de, 0x13c8, 0x13be),
        PLAN(0xc2d1, 0x13bf, 0x13cc, 0x13ff),
        PLAN(0xe2d1, 0x13ff, 0x13cc, 0x13ff),
        PLAN(0xe2d1, 0x13ff, 0x13cc, 0x13df),
        PLAN(0xe2d1, 0x13df, 0x13cc, 0x13bf),
        PLAN(0xe2d1, 0x13bf, 0x13cc, 0x137f),
        PLAN(0xf2d1, 0x137f, 0x13cc, 0x137f),
    };
    static const mirisdr_rspduo_gain_plan_t high_a[] = {
        PLAN(0xc2d1, 0x12df, 0x12b4, 0x13ff),
        PLAN(0xc2d1, 0x12df, 0x12b4, 0x13fb),
        PLAN(0xc2d1, 0x12df, 0x12b4, 0x13f7),
        PLAN(0xc2d1, 0x12ff, 0x12b6, 0x13ff),
        PLAN(0xc2d1, 0x12ff, 0x12b6, 0x13fb),
        PLAN(0xc2d1, 0x12ff, 0x12b6, 0x13f7),
        PLAN(0xc2d1, 0x12ff, 0x12b6, 0x13ef),
        PLAN(0xe2d1, 0x12ff, 0x12b6, 0x13ef),
        PLAN(0xf2d1, 0x12ff, 0x12b6, 0x13ef),
    };
    static const mirisdr_rspduo_gain_plan_t high_b[] = {
        PLAN(0xc2d1, 0x13fe, 0x13e8, 0x13fe),
        PLAN(0xc2d1, 0x13fe, 0x13e8, 0x13de),
        PLAN(0xc2d1, 0x13de, 0x13e8, 0x13be),
        PLAN(0xc2d1, 0x13bf, 0x13ec, 0x13ff),
        PLAN(0xc2d1, 0x13ff, 0x13ec, 0x13df),
        PLAN(0xc2d1, 0x13df, 0x13ec, 0x13bf),
        PLAN(0xc2d1, 0x13bf, 0x13ec, 0x137f),
        PLAN(0xe2d1, 0x137f, 0x13ec, 0x137f),
        PLAN(0xf2d1, 0x137f, 0x13ec, 0x137f),
    };
    assert_gain_band(10000000u, low_a, low_b,
                     sizeof(low_a) / sizeof(low_a[0]));
    assert_gain_band(100000000u, mid_a, mid_b,
                     sizeof(mid_a) / sizeof(mid_a[0]));
    assert_gain_band(853862500u, uhf_a, uhf_b,
                     sizeof(uhf_a) / sizeof(uhf_a[0]));
    assert_gain_band(1500000000u, high_a, high_b,
                     sizeof(high_a) / sizeof(high_a[0]));

    static const struct {
        uint32_t frequency;
        size_t count;
        uint16_t tuner_a_4a;
        uint16_t tuner_b_4a;
        uint32_t last_reg9;
    } routes[] = {
        {1000u, 7u, 0x12a2u, 0x13c4u, 0xded1u},
        {12000000u, 7u, 0x12a1u, 0x13c2u, 0xded1u},
        {30000000u, 7u, 0x12a3u, 0x13c6u, 0xded1u},
        {60000000u, 10u, 0x12a4u, 0x13c8u, 0xe2d1u},
        {120000000u, 10u, 0x12b4u, 0x13e8u, 0xe2d1u},
        {250000000u, 10u, 0x12b0u, 0x13e0u, 0xced1u},
        {300000000u, 10u, 0x12a8u, 0x13d0u, 0xced1u},
        {380000000u, 10u, 0x12b8u, 0x13f0u, 0xced1u},
        {420000000u, 10u, 0x12a4u, 0x13c8u, 0xf2d1u},
        {1000000000u, 9u, 0x12b4u, 0x13e8u, 0xf2d1u},
        {2000000000u, 9u, 0x12b4u, 0x13e8u, 0xf2d1u},
    };
    for (size_t i = 0u; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        mirisdr_rspduo_gain_plan_t route = {0};
        assert(mirisdr_rspduo_gain_plan(routes[i].frequency, 1u, 45, 0u,
                                        0u, 0u, 0u, 0u, &route) == 0);
        assert(route.gpio_4a == routes[i].tuner_a_4a);
        assert(mirisdr_rspduo_gain_plan(routes[i].frequency, 2u, 45, 0u,
                                        0u, 0u, 0u, 0u, &route) == 0);
        assert(route.gpio_4a == routes[i].tuner_b_4a);
        assert(mirisdr_rspduo_gain_plan(
                   routes[i].frequency, 1u, 45,
                   (unsigned int)(routes[i].count - 1u),
                   0u, 0u, 0u, 0u, &route) == 0);
        assert(route.reg9 == routes[i].last_reg9);
        assert(mirisdr_rspduo_gain_plan(
                   routes[i].frequency, 1u, 45,
                   (unsigned int)routes[i].count,
                   0u, 0u, 0u, 0u, &route) < 0);
    }

    mirisdr_rspduo_gain_plan_t controls = {0};
    assert(mirisdr_rspduo_gain_plan(100000000u, 1u, 45, 0u, 0u,
                                    1u, 1u, 1u, &controls) == 0);
    assert(controls.first_gpio_4b == 0x128fu);
    assert(controls.gpio_4a == 0x1284u);
    assert(mirisdr_rspduo_gain_plan(100000000u, 2u, 45, 0u, 0u,
                                    0u, 1u, 0u, &controls) == 0);
    assert(controls.first_gpio_4b == 0x13fcu);
    assert(controls.final_gpio_4b == 0x13fcu);
    assert(mirisdr_rspduo_gain_plan_with_controls(
               10000000u, 1u, 45, 4u, 0u, 0u, 0u, 0u, 1u, 0u,
               &controls) == 0);
    assert(controls.reg9 == 0xded1u);
    assert(controls.gpio_4a == 0x12a2u);
    assert(controls.final_gpio_4b == 0x13ffu);
    assert(mirisdr_rspduo_gain_plan_with_controls(
               10000000u, 1u, 45, 2u, 0u, 0u, 0u, 0u, 1u, 1u,
               &controls) == 0);
    assert(controls.reg9 == 0xcad1u);
    assert(controls.first_gpio_4b == 0x12f7u);
    assert(mirisdr_rspduo_gain_plan_with_controls(
               10000000u, 1u, 45, 5u, 0u, 0u, 0u, 0u, 1u, 0u,
               &controls) < 0);
    assert(mirisdr_rspduo_gain_plan(999u, 1u, 45, 0u, 0u,
                                    0u, 0u, 0u, &controls) < 0);
    assert(mirisdr_rspduo_gain_plan(2000000001u, 1u, 45, 0u, 0u,
                                    0u, 0u, 0u, &controls) < 0);
}

#undef PLAN

int main(void)
{
    uint32_t reg3 = 0u;
    uint32_t reg4 = 0u;
    assert(mirisdr_rspduo_pll_words(2048000u, &reg3, &reg4) == 0);
    assert(reg3 == 0x01081fu);
    assert(reg4 == 0x0624ddu);
    assert(mirisdr_rspduo_252_format_word(2048000u) == 0x000005u);
    assert(mirisdr_rspduo_252_format_word(3072000u) == 0x000094u);
    assert(mirisdr_rspduo_252_format_word(6000000u) == 0x000094u);
    assert(mirisdr_rspduo_format_word(2048000u) == 0x000005u);
    assert(mirisdr_rspduo_format_word(3000000u) == 0x000005u);
    assert(mirisdr_rspduo_format_word(3072000u) == 0x000094u);
    assert(mirisdr_rspduo_format_word(6000000u) == 0x000094u);
    assert(mirisdr_rspduo_format_word(7000000u) == 0x000085u);
    assert(mirisdr_rspduo_format_word(9000000u) == 0x0000a5u);
    assert(mirisdr_rspduo_format_word(10000000u) == 0x000c94u);
    assert(mirisdr_rspduo_format_samples(2048000u) == 336u);
    assert(mirisdr_rspduo_format_samples(3000000u) == 336u);
    assert(mirisdr_rspduo_format_samples(3072000u) == 252u);
    assert(mirisdr_rspduo_format_samples(6000000u) == 252u);
    assert(mirisdr_rspduo_format_samples(7000000u) == 336u);
    assert(mirisdr_rspduo_format_samples(9000000u) == 384u);
    assert(mirisdr_rspduo_format_samples(10000000u) == 504u);
    assert(mirisdr_rspduo_pll_words(10000000u, &reg3, &reg4) == 0);
    assert(reg3 == 0x01ca07u);
    assert(reg4 == 0u);
    assert(mirisdr_rspduo_pll_words(0u, &reg3, &reg4) < 0);
    assert(mirisdr_rspduo_pll_words(2048000u, NULL, &reg4) < 0);
    assert_gain_plans();
    puts("RSPDUO_PLL_OK");
    return 0;
}
