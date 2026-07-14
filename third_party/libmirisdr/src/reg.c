/*
 * Copyright (C) 2013 by Miroslav Slugen <thunder.m@email.cz
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


int mirisdr_write_reg (mirisdr_dev_t *p, uint8_t reg, uint32_t val) {
    uint16_t value = (val & 0xff) << 8 | reg;
    uint16_t index = (val >> 8) & 0xffff;

    if (!p) goto failed;
    if (!p->dh) goto failed;

#if MIRISDR_DEBUG >= 2
    fprintf( stderr, "write reg: 0x%02x, val 0x%08x\n", reg, val);
#endif

    uint8_t request_type = p->usb_pid == 0x3020u ? 0x40u : 0x42u;
    if (getenv("OPENRSP_TRACE_USB") != NULL) {
        fprintf(stderr, "OPENRSP_USB control type=%02x request=41 value=%04x index=%04x\n",
                request_type, value, index);
    }
    return libusb_control_transfer(p->dh, request_type, 0x41, value, index, NULL, 0,
                                   CTRL_TIMEOUT);

failed:
    return -1;
}

static int mirisdr_rspduo_gpio(mirisdr_dev_t *p, uint8_t request, uint16_t value)
{
    if (!p || !p->dh) return -1;
    if (getenv("OPENRSP_TRACE_USB") != NULL) {
        fprintf(stderr, "OPENRSP_USB control type=40 request=%02x value=%04x index=0040\n",
                request, value);
    }
    return libusb_control_transfer(p->dh, 0x40, request, value, 0x0040, NULL, 0,
                                   CTRL_TIMEOUT);
}

static uint16_t rspduo_active_low(uint16_t value, uint16_t mask,
                                  unsigned int enabled)
{
    return enabled != 0u ? (uint16_t)(value & (uint16_t)~mask) :
                           (uint16_t)(value | mask);
}

int mirisdr_set_rspduo_controls(mirisdr_dev_t *p, unsigned int tuner,
                                unsigned int bias_tee, unsigned int rf_notch,
                                unsigned int dab_notch,
                                unsigned int external_reference,
                                unsigned int am_port, unsigned int am_notch,
                                unsigned int changed_flags)
{
    if (!p || p->usb_pid != 0x3020u || (tuner != 1u && tuner != 2u) ||
        bias_tee > 1u || rf_notch > 1u || dab_notch > 1u ||
        external_reference > 1u || am_port < 1u || am_port > 2u ||
        am_notch > 1u ||
        (tuner == 1u && bias_tee != 0u)) return -1;
    if (tuner != 1u &&
        (changed_flags & (MIRISDR_RSPDUO_CHANGE_AM_PORT |
                          MIRISDR_RSPDUO_CHANGE_AM_NOTCH)) != 0u) return -1;

    unsigned int index = tuner - 1u;
    p->rspduo_bias_tee[index] = bias_tee;
    p->rspduo_rf_notch[index] = rf_notch;
    p->rspduo_dab_notch[index] = dab_notch;
    p->rspduo_external_reference = external_reference;
    p->rspduo_am_port = am_port;
    p->rspduo_am_notch = am_notch;

    /* API 3.15 reprograms the tuner when the tuner-1 AM input changes.  The
     * clean-room trace differs in register 0 bit 11 (0x04f610 for AM1 versus
     * 0x04fe10 for AM2 at 10 MHz); mirisdr_set_soft derives the rest from the
     * active frequency instead of replaying a frequency-specific capture. */
    if ((changed_flags & MIRISDR_RSPDUO_CHANGE_AM_PORT) != 0u &&
        mirisdr_set_soft(p) < 0) return -1;
    /* On tuner A, API 3.15.1 emits this frequency-independent GPIO pulse
     * sequence when the AM notch is enabled at 10 MHz.  Disabling it returns
     * success without an observable USB control transfer. */
    if ((changed_flags & MIRISDR_RSPDUO_CHANGE_AM_NOTCH) != 0u &&
        am_notch != 0u) {
        if (mirisdr_rspduo_gpio(p, 0x4b, 0x12ff) < 0 ||
            mirisdr_rspduo_gpio(p, 0x4b, 0x12ff) < 0 ||
            mirisdr_rspduo_gpio(p, 0x4b, 0x12ff) < 0 ||
            mirisdr_rspduo_gpio(p, 0x4a, 0x00df) < 0 ||
            mirisdr_rspduo_gpio(p, 0x4a, 0x01ff) < 0 ||
            mirisdr_rspduo_gpio(p, 0x4b, 0x00ff) < 0 ||
            mirisdr_rspduo_gpio(p, 0x4b, 0x01ff) < 0)
            return -1;
    }

    /* Clean-room API 3.15.1 traces show active-low GPIO controls.  Coalesce
     * controls sharing a GPIO bank so a combined API update cannot restore a
     * bit changed earlier in the same request. */
    if ((changed_flags & MIRISDR_RSPDUO_CHANGE_EXT_REF) != 0u) {
        uint16_t value = tuner == 1u ? 0x12a4u : 0x123fu;
        value = rspduo_active_low(value, 0x0020u, external_reference);
        if (mirisdr_rspduo_gpio(p, 0x4a, value) < 0) return -1;
    }
    if ((changed_flags & (MIRISDR_RSPDUO_CHANGE_BIAS_TEE |
                          MIRISDR_RSPDUO_CHANGE_RF_NOTCH |
                          (tuner == 1u ? MIRISDR_RSPDUO_CHANGE_DAB_NOTCH : 0u))) != 0u) {
        uint16_t value = tuner == 1u ? 0x12dfu : 0x12ffu;
        value = rspduo_active_low(value, tuner == 1u ? 0x0010u : 0x0080u,
                                  rf_notch);
        if (tuner == 1u)
            value = rspduo_active_low(value, 0x0040u, dab_notch);
        else
            value = rspduo_active_low(value, 0x0002u, bias_tee);
        if (mirisdr_rspduo_gpio(p, 0x4b, value) < 0) return -1;
    }
    if (tuner == 2u &&
        (changed_flags & MIRISDR_RSPDUO_CHANGE_DAB_NOTCH) != 0u) {
        unsigned int lna = p->rspduo_lna_state[1];
        static const uint16_t gpio_13[] = {
            0x13fe, 0x13fe, 0x13de, 0x13be, 0x13ff,
            0x13ff, 0x13df, 0x13bf, 0x137f, 0x137f
        };
        if (lna >= sizeof(gpio_13) / sizeof(gpio_13[0])) return -1;
        uint16_t value = rspduo_active_low(gpio_13[lna], 0x0002u, dab_notch);
        if (mirisdr_rspduo_gpio(p, 0x4b, value) < 0) return -1;
        p->rspduo_gpio13 = value;
    }
    return 0;
}

struct rspduo_gain_row {
    uint16_t reg9_base;
    uint16_t tuner_a_first_4b;
    uint16_t tuner_a_4a;
    uint16_t tuner_a_final_4b;
    uint16_t tuner_b_4a;
    uint16_t tuner_b_final_4b;
};

struct rspduo_gain_band {
    const struct rspduo_gain_row *rows;
    size_t count;
    unsigned int tuner_b_bank_threshold;
    uint16_t tuner_b_default_4b;
    int tuner_b_toggles_bank_bit;
};

#define GAIN_ROW(reg9, a1, a4a, a2, b4a, b2) \
    {reg9, a1, a4a, a2, b4a, b2}

#define LOW_GAIN_ROWS(a4a, b4a) \
    GAIN_ROW(0xc000, 0x12ff, a4a, 0x13ff, b4a, 0x13ff), \
    GAIN_ROW(0xc000, 0x12ff, a4a, 0x13fb, b4a, 0x13df), \
    GAIN_ROW(0xc000, 0x12ff, a4a, 0x13f7, b4a, 0x13bf), \
    GAIN_ROW(0xc000, 0x12ff, a4a, 0x13ef, b4a, 0x137f), \
    GAIN_ROW(0xd000, 0x12ff, a4a, 0x13ef, b4a, 0x137f), \
    GAIN_ROW(0xcc00, 0x12ff, a4a, 0x13ef, b4a, 0x137f), \
    GAIN_ROW(0xdc00, 0x12ff, a4a, 0x13ef, b4a, 0x137f)

#define MID_GAIN_ROWS(a_lo, a_hi, b_lo, b_hi, last_reg9) \
    GAIN_ROW(0xc000, 0x12df, a_lo, 0x13ff, b_lo, 0x13fe), \
    GAIN_ROW(0xc000, 0x12df, a_lo, 0x13fb, b_lo, 0x13de), \
    GAIN_ROW(0xc000, 0x12df, a_lo, 0x13f7, b_lo, 0x13be), \
    GAIN_ROW(0xc000, 0x12df, a_lo, 0x13ef, b_lo, 0x137e), \
    GAIN_ROW(0xc000, 0x12ff, a_hi, 0x13ff, b_hi, 0x13ff), \
    GAIN_ROW(0xc000, 0x12ff, a_hi, 0x13fb, b_hi, 0x13df), \
    GAIN_ROW(0xc000, 0x12ff, a_hi, 0x13f7, b_hi, 0x13bf), \
    GAIN_ROW(0xc000, 0x12ff, a_hi, 0x13ef, b_hi, 0x137f), \
    GAIN_ROW(0xd000, 0x12ff, a_hi, 0x13ef, b_hi, 0x137f), \
    GAIN_ROW(last_reg9, 0x12ff, a_hi, 0x13ef, b_hi, 0x137f)

/* API 3.15.1 switches front-end selectors at ten exact RF ranges.  These are
 * reconstructed control words from independent USB traces. */
static const struct rspduo_gain_row rspduo_gain_below_12mhz[] = {
    LOW_GAIN_ROWS(0x12a2, 0x13c4)
};
static const struct rspduo_gain_row rspduo_gain_below_30mhz[] = {
    LOW_GAIN_ROWS(0x12a1, 0x13c2)
};
static const struct rspduo_gain_row rspduo_gain_below_60mhz[] = {
    LOW_GAIN_ROWS(0x12a3, 0x13c6)
};
static const struct rspduo_gain_row rspduo_gain_below_120mhz[] = {
    MID_GAIN_ROWS(0x12a4, 0x12a6, 0x13c8, 0x13cc, 0xe000)
};
static const struct rspduo_gain_row rspduo_gain_below_250mhz[] = {
    MID_GAIN_ROWS(0x12b4, 0x12b6, 0x13e8, 0x13ec, 0xe000)
};
static const struct rspduo_gain_row rspduo_gain_below_300mhz[] = {
    MID_GAIN_ROWS(0x12b0, 0x12b2, 0x13e0, 0x13e4, 0xcc00)
};
static const struct rspduo_gain_row rspduo_gain_below_380mhz[] = {
    MID_GAIN_ROWS(0x12a8, 0x12aa, 0x13d0, 0x13d4, 0xcc00)
};
static const struct rspduo_gain_row rspduo_gain_below_420mhz[] = {
    MID_GAIN_ROWS(0x12b8, 0x12ba, 0x13f0, 0x13f4, 0xcc00)
};

#undef LOW_GAIN_ROWS
#undef MID_GAIN_ROWS

static const struct rspduo_gain_row rspduo_gain_below_1ghz[] = {
    GAIN_ROW(0xc000, 0x12df, 0x12a4, 0x13ff, 0x13c8, 0x13fe),
    GAIN_ROW(0xe000, 0x12df, 0x12a4, 0x13ff, 0x13c8, 0x13fe),
    GAIN_ROW(0xe000, 0x12df, 0x12a4, 0x13fb, 0x13c8, 0x13de),
    GAIN_ROW(0xe000, 0x12df, 0x12a4, 0x13f7, 0x13c8, 0x13be),
    GAIN_ROW(0xc000, 0x12ff, 0x12a6, 0x13ff, 0x13cc, 0x13ff),
    GAIN_ROW(0xe000, 0x12ff, 0x12a6, 0x13ff, 0x13cc, 0x13ff),
    GAIN_ROW(0xe000, 0x12ff, 0x12a6, 0x13fb, 0x13cc, 0x13df),
    GAIN_ROW(0xe000, 0x12ff, 0x12a6, 0x13f7, 0x13cc, 0x13bf),
    GAIN_ROW(0xe000, 0x12ff, 0x12a6, 0x13ef, 0x13cc, 0x137f),
    GAIN_ROW(0xf000, 0x12ff, 0x12a6, 0x13ef, 0x13cc, 0x137f),
};

static const struct rspduo_gain_row rspduo_gain_below_2ghz[] = {
    GAIN_ROW(0xc000, 0x12df, 0x12b4, 0x13ff, 0x13e8, 0x13fe),
    GAIN_ROW(0xc000, 0x12df, 0x12b4, 0x13fb, 0x13e8, 0x13de),
    GAIN_ROW(0xc000, 0x12df, 0x12b4, 0x13f7, 0x13e8, 0x13be),
    GAIN_ROW(0xc000, 0x12ff, 0x12b6, 0x13ff, 0x13ec, 0x13ff),
    GAIN_ROW(0xc000, 0x12ff, 0x12b6, 0x13fb, 0x13ec, 0x13df),
    GAIN_ROW(0xc000, 0x12ff, 0x12b6, 0x13f7, 0x13ec, 0x13bf),
    GAIN_ROW(0xc000, 0x12ff, 0x12b6, 0x13ef, 0x13ec, 0x137f),
    GAIN_ROW(0xe000, 0x12ff, 0x12b6, 0x13ef, 0x13ec, 0x137f),
    GAIN_ROW(0xf000, 0x12ff, 0x12b6, 0x13ef, 0x13ec, 0x137f),
};

#undef GAIN_ROW

static int rspduo_gain_band(uint32_t frequency, struct rspduo_gain_band *band)
{
    if (!band || frequency < 1000u || frequency > 2000000000u) return -1;
#define SET_GAIN_BAND(rows_, threshold_, default_, toggle_) \
    *band = (struct rspduo_gain_band){ \
        rows_, sizeof(rows_) / sizeof((rows_)[0]), \
        threshold_, default_, toggle_ \
    }
    if (frequency < 12000000u) {
        SET_GAIN_BAND(rspduo_gain_below_12mhz, 0u, 0x13ffu, 0);
    } else if (frequency < 30000000u) {
        SET_GAIN_BAND(rspduo_gain_below_30mhz, 0u, 0x13ffu, 0);
    } else if (frequency < 60000000u) {
        SET_GAIN_BAND(rspduo_gain_below_60mhz, 0u, 0x13ffu, 0);
    } else if (frequency < 120000000u) {
        SET_GAIN_BAND(rspduo_gain_below_120mhz, 4u, 0x13feu, 1);
    } else if (frequency < 250000000u) {
        SET_GAIN_BAND(rspduo_gain_below_250mhz, 4u, 0x13feu, 1);
    } else if (frequency < 300000000u) {
        SET_GAIN_BAND(rspduo_gain_below_300mhz, 4u, 0x13feu, 1);
    } else if (frequency < 380000000u) {
        SET_GAIN_BAND(rspduo_gain_below_380mhz, 4u, 0x13feu, 1);
    } else if (frequency < 420000000u) {
        SET_GAIN_BAND(rspduo_gain_below_420mhz, 4u, 0x13feu, 1);
    } else if (frequency < 1000000000u) {
        SET_GAIN_BAND(rspduo_gain_below_1ghz, 4u, 0x13feu, 1);
    } else {
        SET_GAIN_BAND(rspduo_gain_below_2ghz, 3u, 0x13feu, 1);
    }
#undef SET_GAIN_BAND
    return 0;
}

int mirisdr_rspduo_gain_plan_with_controls(
    uint32_t frequency, unsigned int tuner, int gain_reduction,
    unsigned int lna_state, uint16_t previous_gpio_4b,
    unsigned int rf_notch, unsigned int dab_notch,
    unsigned int external_reference, unsigned int am_port,
    unsigned int am_notch, mirisdr_rspduo_gain_plan_t *plan)
{
    struct rspduo_gain_band band = {0};
    if (!plan || (tuner != 1u && tuner != 2u) || gain_reduction < 20 ||
        gain_reduction > 59 || rf_notch > 1u || dab_notch > 1u ||
        external_reference > 1u || am_port < 1u || am_port > 2u ||
        am_notch > 1u || rspduo_gain_band(frequency, &band) < 0 ||
        (tuner == 1u && am_port == 1u && frequency < 60000000u &&
         lna_state >= 5u) ||
        lna_state >= band.count) return -1;
    const struct rspduo_gain_row *row = &band.rows[lna_state];
    uint16_t reg9_base = row->reg9_base;
    if (tuner == 1u && am_port == 1u && frequency < 60000000u) {
        static const uint16_t am_port1_reg9[] = {
            0xc000u, 0xc400u, 0xc800u, 0xcc00u, 0xdc00u
        };
        reg9_base = am_port1_reg9[lna_state];
    }
    plan->reg9 = (uint32_t)reg9_base | ((uint32_t)gain_reduction << 4) | 1u;
    if (tuner == 1u) {
        plan->first_gpio_4b = rspduo_active_low(row->tuner_a_first_4b,
                                                0x0010u, rf_notch);
        plan->first_gpio_4b = rspduo_active_low(plan->first_gpio_4b,
                                                0x0040u, dab_notch);
        if (frequency < 60000000u)
            plan->first_gpio_4b = rspduo_active_low(plan->first_gpio_4b,
                                                    0x0008u, am_notch);
        plan->gpio_4a = rspduo_active_low(row->tuner_a_4a, 0x0020u,
                                         external_reference);
        plan->final_gpio_4b = am_port == 1u && frequency < 60000000u ?
                              0x13ffu : row->tuner_a_final_4b;
    } else {
        uint16_t previous = previous_gpio_4b != 0u ? previous_gpio_4b :
                                                    band.tuner_b_default_4b;
        if (band.tuner_b_toggles_bank_bit != 0)
            previous = lna_state < band.tuner_b_bank_threshold ?
                       (uint16_t)(previous & ~1u) :
                       (uint16_t)(previous | 1u);
        previous = rspduo_active_low(previous, 0x0002u, dab_notch);
        plan->first_gpio_4b = previous;
        plan->gpio_4a = row->tuner_b_4a;
        plan->final_gpio_4b = rspduo_active_low(row->tuner_b_final_4b,
                                                0x0002u, dab_notch);
    }
    return 0;
}

int mirisdr_rspduo_gain_plan(uint32_t frequency, unsigned int tuner,
                             int gain_reduction, unsigned int lna_state,
                             uint16_t previous_gpio_4b,
                             unsigned int rf_notch,
                             unsigned int dab_notch,
                             unsigned int external_reference,
                             mirisdr_rspduo_gain_plan_t *plan)
{
    return mirisdr_rspduo_gain_plan_with_controls(
        frequency, tuner, gain_reduction, lna_state, previous_gpio_4b,
        rf_notch, dab_notch, external_reference, 2u, 0u, plan);
}

int mirisdr_set_rspduo_gain(mirisdr_dev_t *p, int gain_reduction,
                            unsigned int lna_state)
{
    if (!p || p->usb_pid != 0x3020u) return -1;
    unsigned int tuner = p->rspduo_tuner == 2u ? 2u : 1u;
    mirisdr_rspduo_gain_plan_t plan = {0};
    if (mirisdr_rspduo_gain_plan_with_controls(
            p->freq, tuner, gain_reduction, lna_state, p->rspduo_gpio13,
            p->rspduo_rf_notch[tuner - 1u],
            p->rspduo_dab_notch[tuner - 1u],
            p->rspduo_external_reference, p->rspduo_am_port,
            p->rspduo_am_notch, &plan) < 0)
        return -1;
    if (mirisdr_write_reg(p, 0x09, plan.reg9) < 0 ||
        mirisdr_rspduo_gpio(p, 0x4b, plan.first_gpio_4b) < 0 ||
        mirisdr_rspduo_gpio(p, 0x4a, plan.gpio_4a) < 0 ||
        mirisdr_rspduo_gpio(p, 0x4b, plan.final_gpio_4b) < 0)
        return -1;
    if (tuner == 2u) p->rspduo_gpio13 = plan.final_gpio_4b;
    unsigned int state_index = p->rspduo_tuner == 2u ? 1u : 0u;
    p->rspduo_gain_reduction[state_index] = gain_reduction;
    p->rspduo_lna_state[state_index] = lna_state;
    return 0;
}

static int mirisdr_rspduo_frontend_init(mirisdr_dev_t *p)
{
    static const struct {
        uint8_t request;
        uint16_t value;
    } sequence[] = {
        {0x4a, 0x129f}, {0x4a, 0x13ff}, {0x4a, 0x137f},
        {0x4a, 0x0000}, {0x4a, 0x0100}, {0x4b, 0x12ff},
        {0x4b, 0x13ff}, {0x4b, 0x0000}, {0x4b, 0x0100},
    };

    if (libusb_control_transfer(p->dh, 0x40, 0x41, 0x8008, 0x00ea, NULL, 0,
                                CTRL_TIMEOUT) < 0) return -1;
    for (size_t i = 0; i < sizeof(sequence) / sizeof(sequence[0]); ++i) {
        uint16_t value = sequence[i].value;
        if (p->rspduo_tuner == 2u && i == 2u) value = 0x13ff;
        if (mirisdr_rspduo_gpio(p, sequence[i].request, value) < 0) return -1;
    }
    return 0;
}

static int mirisdr_rspduo_frontend_ready(mirisdr_dev_t *p)
{
    if (!p || !p->dh || p->usb_pid != 0x3020u) return -1;
    /* The device returns 0x0a for both API 3.15 reads on tuner A. */
    for (unsigned int i = 0u; i < 2u; ++i) {
        uint8_t ready = 0u;
        if (libusb_control_transfer(p->dh, 0xc0, 0x48, 0x0000, 0x0000,
                                    &ready, sizeof(ready), CTRL_TIMEOUT) !=
            (int)sizeof(ready))
            return -1;
    }
    return 0;
}

static int mirisdr_rspduo_route_tuner(mirisdr_dev_t *p)
{
    static const struct {
        uint8_t request;
        uint16_t value;
    } sequence[] = {
        {0x4a, 0x12a4}, {0x4a, 0x137f}, {0x4b, 0x12ff}, {0x4b, 0x13ff},
    };

    static const struct {
        uint8_t request;
        uint16_t value;
    } tuner_b_sequence[] = {
        {0x4a, 0x123f}, {0x4a, 0x13c8}, {0x4b, 0x12ff}, {0x4b, 0x13ff},
    };

    if (!p || p->usb_pid != 0x3020u) return 0;
    if (p->rspduo_dual && p->rspduo_tuner == 2u)
        return mirisdr_rspduo_gpio(p, 0x4a, 0x1348);
    if (p->rspduo_dual) {
        static const struct {
            uint8_t request;
            uint16_t value;
        } dual_a_sequence[] = {
            {0x4a, 0x12a4}, {0x4a, 0x1388},
            {0x4b, 0x12ff}, {0x4b, 0x13ff},
        };
        for (size_t i = 0u; i < sizeof(dual_a_sequence) / sizeof(dual_a_sequence[0]); ++i)
            if (mirisdr_rspduo_gpio(p, dual_a_sequence[i].request,
                                    dual_a_sequence[i].value) < 0) return -1;
        return 0;
    }
    if (p->rspduo_tuner == 2u) {
        for (size_t i = 0; i < sizeof(tuner_b_sequence) / sizeof(tuner_b_sequence[0]); ++i)
            if (mirisdr_rspduo_gpio(p, tuner_b_sequence[i].request,
                                    tuner_b_sequence[i].value) < 0) return -1;
        return 0;
    }
    for (size_t i = 0; i < sizeof(sequence) / sizeof(sequence[0]); ++i) {
        if (mirisdr_rspduo_gpio(p, sequence[i].request, sequence[i].value) < 0) return -1;
    }
    return 0;
}

static int mirisdr_rspduo_finish_tuner(mirisdr_dev_t *p)
{
    if (!p || p->usb_pid != 0x3020u) return 0;
    if (p->rspduo_tuner == 2u) {
        if (mirisdr_set_gain(p) < 0 ||
            mirisdr_rspduo_gpio(p, 0x4b, 0x13fe) < 0 ||
            mirisdr_rspduo_gpio(p, 0x4a, 0x13c8) < 0 ||
            mirisdr_rspduo_gpio(p, 0x4b, 0x13be) < 0) return -1;
        p->rspduo_gpio13 = 0x13be;
        return 0;
    }
    if (mirisdr_rspduo_gpio(p, 0x4a, 0x1349) < 0 || mirisdr_set_gain(p) < 0 ||
        mirisdr_rspduo_gpio(p, 0x4b, 0x12df) < 0 ||
        mirisdr_rspduo_gpio(p, 0x4a, 0x1224) < 0 ||
        mirisdr_rspduo_gpio(p, 0x4b, 0x13ff) < 0 ||
        mirisdr_rspduo_gpio(p, 0x4a, 0x13c9) < 0 || mirisdr_set_gain(p) < 0 ||
        mirisdr_rspduo_gpio(p, 0x4b, 0x13fe) < 0 ||
        mirisdr_rspduo_gpio(p, 0x4a, 0x13c9) < 0 ||
        mirisdr_rspduo_gpio(p, 0x4b, 0x13fe) < 0) return -1;
    return 0;
}

static int mirisdr_rspduo_shutdown(mirisdr_dev_t *p)
{
    if (!p || p->usb_pid != 0x3020u) return -1;
    uint32_t shutdown_status = 0u;
    int frontend_stop = p->rspduo_dual ?
        (mirisdr_rspduo_gpio(p, 0x4b, 0x12df) < 0 ||
         mirisdr_rspduo_gpio(p, 0x4a, 0x138c) < 0 ||
         mirisdr_rspduo_gpio(p, 0x4b, 0x12ff) < 0) :
        p->rspduo_tuner == 2u ?
        (mirisdr_rspduo_gpio(p, 0x4b, 0x12ff) < 0 ||
         mirisdr_rspduo_gpio(p, 0x4b, 0x13bf) < 0) :
        (mirisdr_rspduo_gpio(p, 0x4b, 0x12df) < 0 ||
         mirisdr_rspduo_gpio(p, 0x4b, 0x12ff) < 0);
    if (frontend_stop ||
        mirisdr_write_reg(p, 0x09, 0x073000) < 0 ||
        mirisdr_write_reg(p, 0x09, 0x014001) < 0 ||
        mirisdr_write_reg(p, 0x09, 0x201982) < 0 ||
        mirisdr_write_reg(p, 0x09, 0x000003) < 0 ||
        mirisdr_write_reg(p, 0x09, 0x000004) < 0 ||
        mirisdr_write_reg(p, 0x09, 0x289c05) < 0 ||
        mirisdr_write_reg(p, 0x09, 0x200016) < 0 ||
        mirisdr_rspduo_gpio(p, 0x4a, 0x00df) < 0 ||
        mirisdr_rspduo_gpio(p, 0x4a, 0x01ff) < 0 ||
        mirisdr_rspduo_gpio(p, 0x4b, 0x00ff) < 0 ||
        mirisdr_rspduo_gpio(p, 0x4b, 0x01ff) < 0 ||
        mirisdr_write_reg(p, 0x09, 0x00001c) < 0 ||
        mirisdr_write_reg(p, 0x09, 0x000000) < 0 ||
        libusb_control_transfer(p->dh, 0xc0, 0x42, 0x0000, 0x0010,
                                (unsigned char *)&shutdown_status,
                                sizeof(shutdown_status), CTRL_TIMEOUT) !=
            (int)sizeof(shutdown_status) ||
        mirisdr_write_reg(p, 0x09, 0x00000d) < 0 ||
        mirisdr_write_reg(p, 0x09, 0x00000e) < 0 ||
        mirisdr_write_reg(p, 0x09, 0x063000) < 0 ||
        mirisdr_write_reg(p, 0x09, 0x00800e) < 0 ||
        mirisdr_write_reg(p, 0x03, 0x091300) < 0 ||
        mirisdr_write_reg(p, 0x05, 0x000004) < 0) return -1;
    return 0;
}

#define CMD_RESET              0x40
#define CMD_WREG               0x41
#define CMD_RREG               0x42
#define CMD_START_STREAMING    0x43
#define CMD_DOWNLOAD           0x44
#define CMD_STOP_STREAMING     0x45
//WValue = Addr?
#define CMD_REEPROM            0x46
//WValue = Addr?
#define CMD_WEEPROM            0x47
#define CMD_READ_UNKNOWN       0x48
//wValue = gpio << 8 | val
#define CMD_WGPIO              0x49
#define CMD_EXT_WGPIO_BASE     0x4b
/*
RSP1
GPIO(0x13) & 0x01 = DSB_NOTCH
GPIO(0x13) & 0x04 = BROADCAST_NOTCH




*/
