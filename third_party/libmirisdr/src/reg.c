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

int mirisdr_set_rspduo_gain(mirisdr_dev_t *p, int gain_reduction,
                            unsigned int lna_state)
{
    /* Captured from API 3.15.1 on an RSPduo in the 420-1000 MHz band. */
    static const uint16_t reg9_base[] = {
        0xc000, 0xe000, 0xe000, 0xe000, 0xc000,
        0xe000, 0xe000, 0xe000, 0xe000, 0xf000
    };
    static const uint16_t gpio_12[] = {
        0x1224, 0x1224, 0x1224, 0x1224, 0x1226,
        0x1226, 0x1226, 0x1226, 0x1226, 0x1226
    };
    static const uint16_t gpio_13[] = {
        0x13fe, 0x13fe, 0x13fa, 0x13b6, 0x13fe,
        0x13fe, 0x13fa, 0x13f6, 0x13ee, 0x13ee
    };
    static const uint16_t tuner_b_gpio_4a[] = {
        0x13c8, 0x13c8, 0x13c8, 0x13c8, 0x13cc,
        0x13cc, 0x13cc, 0x13cc, 0x13cc, 0x13cc
    };
    static const uint16_t tuner_b_gpio_4b[] = {
        0x13fe, 0x13fe, 0x13de, 0x13be, 0x13ff,
        0x13ff, 0x13df, 0x13bf, 0x137f, 0x137f
    };

    if (!p || p->usb_pid != 0x3020u || p->freq < 420000000u ||
        p->freq >= 1000000000u || gain_reduction < 20 ||
        gain_reduction > 59 || lna_state >= 10u) return -1;

    uint32_t reg9 = (uint32_t)reg9_base[lna_state] |
                    ((uint32_t)gain_reduction << 4) | 1u;
    if (mirisdr_write_reg(p, 0x09, reg9) < 0) return -1;
    if (p->rspduo_tuner == 2u) {
        uint16_t previous = p->rspduo_gpio13 != 0u ? p->rspduo_gpio13 : 0x13fe;
        uint16_t bank_transition = lna_state < 4u ?
                                   (uint16_t)(previous & ~1u) :
                                   (uint16_t)(previous | 1u);
        if (mirisdr_rspduo_gpio(p, 0x4b, bank_transition) < 0 ||
            mirisdr_rspduo_gpio(p, 0x4a, tuner_b_gpio_4a[lna_state]) < 0 ||
            mirisdr_rspduo_gpio(p, 0x4b, tuner_b_gpio_4b[lna_state]) < 0)
            return -1;
        p->rspduo_gpio13 = tuner_b_gpio_4b[lna_state];
    } else if (mirisdr_rspduo_gpio(p, 0x4b, 0x12df) < 0 ||
               mirisdr_rspduo_gpio(p, 0x4a, gpio_12[lna_state]) < 0 ||
               mirisdr_rspduo_gpio(p, 0x4b, gpio_13[lna_state]) < 0) {
        return -1;
    }
    return 0;
}

static int mirisdr_rspduo_frontend_init(mirisdr_dev_t *p)
{
    static const struct {
        uint8_t request;
        uint16_t value;
    } sequence[] = {
        {0x4a, 0x129f}, {0x4a, 0x13ff}, {0x4a, 0x13bf},
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

static int mirisdr_rspduo_route_tuner(mirisdr_dev_t *p)
{
    static const struct {
        uint8_t request;
        uint16_t value;
    } sequence[] = {
        {0x4a, 0x1224}, {0x4a, 0x1389}, {0x4b, 0x12ff}, {0x4b, 0x13ff},
    };

    static const struct {
        uint8_t request;
        uint16_t value;
    } tuner_b_sequence[] = {
        {0x4a, 0x123f}, {0x4a, 0x13c8}, {0x4b, 0x12ff}, {0x4b, 0x13ff},
    };

    if (!p || p->usb_pid != 0x3020u) return 0;
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
    int frontend_stop = p->rspduo_tuner == 2u ?
        (mirisdr_rspduo_gpio(p, 0x4b, 0x12ff) < 0 ||
         mirisdr_rspduo_gpio(p, 0x4b, 0x13bf) < 0) :
        (mirisdr_rspduo_gpio(p, 0x4b, 0x12df) < 0 ||
         mirisdr_rspduo_gpio(p, 0x4a, 0x1389) < 0 ||
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
