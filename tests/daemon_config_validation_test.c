/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config_validation.h"

#include <assert.h>
#include <stdio.h>

static openrsp_radio_config valid_config(uint32_t sample_rate_hz,
                                         int32_t setpoint_dbfs)
{
    openrsp_radio_config config = {
        .sample_rate_hz = sample_rate_hz,
        .center_frequency_hz = 853712500u,
        .bandwidth_hz = 1536000u,
        .gain_reduction_db = 10,
        .lna_state = 0u,
        .agc_mode = 1,
        .agc_setpoint_dbfs = setpoint_dbfs,
        .tuner = OPENRSP_TUNER_A
    };
    return config;
}

int main(void)
{
    openrsp_radio_config config = valid_config(2000000u, -72);
    assert(openrspd_config_valid(&config));
    config.center_frequency_hz = 0u;
    assert(openrspd_config_valid(&config));
    config.center_frequency_hz = 853712500u;
    config.agc_setpoint_dbfs = 0;
    assert(openrspd_config_valid(&config));
    config.agc_setpoint_dbfs = -73;
    assert(!openrspd_config_valid(&config));

    config = valid_config(8064000u, -72);
    assert(openrspd_config_valid(&config));
    config = valid_config(10000000u, -72);
    assert(openrspd_config_valid(&config));

    puts("DAEMON_CONFIG_VALIDATION_OK");
    return 0;
}
