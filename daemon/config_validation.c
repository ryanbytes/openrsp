/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "config_validation.h"

int openrspd_config_valid(const openrsp_radio_config *config)
{
    return config && config->sample_rate_hz != 0u &&
           config->gain_reduction_db >= 0 && config->gain_reduction_db <= 59 &&
           config->lna_state <= 9u && config->agc_mode >= 0 && config->agc_mode <= 4 &&
           config->agc_setpoint_dbfs >= -72 && config->agc_setpoint_dbfs <= 0 &&
           config->bias_tee_enabled <= 1u && config->rf_notch_enabled <= 1u &&
           config->dab_notch_enabled <= 1u &&
           config->external_reference_enabled <= 1u &&
           config->am_port_select <= 1u && config->am_notch_enabled <= 1u &&
           !(config->tuner == OPENRSP_TUNER_A && config->bias_tee_enabled != 0u) &&
           !(config->tuner == OPENRSP_TUNER_B &&
             (config->am_port_select != 0u || config->am_notch_enabled != 0u)) &&
           (config->tuner == OPENRSP_TUNER_A || config->tuner == OPENRSP_TUNER_B);
}
