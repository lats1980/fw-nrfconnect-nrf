/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <stdio.h>
#include <logging/log.h>

#include "slm_ui.h"
#include <modem/lte_lc.h>

LOG_MODULE_REGISTER(ui, CONFIG_SLM_LOG_LEVEL);

static enum ui_led_pattern current_led_state;
static struct k_delayed_work leds_update_work;

/**@brief Update LEDs state. */
static void leds_update(struct k_work *work)
{
	int err = 0;
	static bool led_on;

	led_on = !led_on;
	if (led_on) {
		err = dk_set_leds_state(DK_NO_LEDS_MSK, DK_ALL_LEDS_MSK);
		if (err) {
			LOG_ERR("Could not set leds state, err code: %d", err);
		}
	} else {
		err = dk_set_leds_state(DK_ALL_LEDS_MSK, DK_NO_LEDS_MSK);
		if (err) {
			LOG_ERR("Could not set leds state, err code: %d", err);
		}
	}

	if (work) {
		if (led_on) {
			k_delayed_work_submit(&leds_update_work,
					      K_MSEC(UI_LED_ON_PERIOD_NORMAL));
		} else {
			k_delayed_work_submit(&leds_update_work,
					      K_MSEC(UI_LED_OFF_PERIOD_NORMAL));
		}
	}
}

static void lte_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if (evt->nw_reg_status == LTE_LC_NW_REG_UICC_FAIL) {
			LOG_INF("Network registration fail: UICC");
		} else if ((evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME) ||
			(evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING)) {
			LOG_INF("Network registration status: %s",
			evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ?
			"Connected - home network" : "Connected - roaming");
			ui_led_set_pattern(UI_LTE_CONNECTED);
		}
		break;
	case LTE_LC_EVT_PSM_UPDATE:
		LOG_INF("PSM parameter update: TAU: %d, Active time: %d",
			evt->psm_cfg.tau, evt->psm_cfg.active_time);
		break;
	case LTE_LC_EVT_EDRX_UPDATE: {
		char log_buf[60];
		ssize_t len;

		len = snprintf(log_buf, sizeof(log_buf),
					"eDRX parameter update: eDRX: %0.2f, PTW: %0.2f",
					evt->edrx_cfg.edrx, evt->edrx_cfg.ptw);
		if ((len > 0) && (len < sizeof(log_buf))) {
			LOG_INF("%s", log_strdup(log_buf));
		}
		break;
	}
	case LTE_LC_EVT_RRC_UPDATE:
		LOG_INF("RRC mode: %s",
			evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED ?
			"Connected" : "Idle");
		break;
	case LTE_LC_EVT_CELL_UPDATE:
		LOG_INF("LTE cell changed: Cell ID: %d, Tracking area: %d",
			evt->cell.id, evt->cell.tac);
		break;
	default:
		break;
	}
}

void ui_led_set_pattern(enum ui_led_pattern state)
{
	current_led_state = state;
}

enum ui_led_pattern ui_led_get_pattern(void)
{
	return current_led_state;
}

int slm_ui_init(void)
{
	int err = 0;

	err = dk_leds_init();
	if (err) {
		LOG_ERR("Could not initialize leds, err code: %d", err);
		return err;
	}

	lte_lc_register_handler(lte_handler);
	err = lte_lc_init();
	if (err) {
		LOG_ERR("Could not initialize link control, err code: %d", err);
		return err;
	}

	err = dk_set_leds_state(DK_ALL_LEDS_MSK, DK_NO_LEDS_MSK);
	if (err) {
		LOG_ERR("Could not set leds state, err code: %d", err);
		return err;
	}

	k_delayed_work_init(&leds_update_work, leds_update);
	k_delayed_work_submit(&leds_update_work, K_NO_WAIT);

	return err;
}
