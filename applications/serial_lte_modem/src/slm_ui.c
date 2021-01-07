/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <stdio.h>
#include <logging/log.h>

#include "slm_ui.h"
#include <drivers/gpio.h>
#include <modem/lte_lc.h>
#include <modem/modem_info.h>
#if defined(CONFIG_SLM_DIAG)
#include "slm_diag.h"
#endif
const struct device *ui_gpio_dev;

LOG_MODULE_REGISTER(ui, CONFIG_SLM_LOG_LEVEL);

static struct led leds[LED_ID_COUNT];

static void work_handler(struct k_work *work)
{
	struct led *led = CONTAINER_OF(work, struct led, work);
	const struct led_effect_step *effect_step =
		&leds[led->id].effect->steps[leds[led->id].effect_step];
	//int substeps_left = effect_step->substep_count - leds[led->id].effect_substep;

	//LOG_DBG("LED %d state %d", led_map[led->id], effect_step->led_on);
	dk_set_led(led_map[led->id], effect_step->led_on);

	leds[led->id].effect_substep++;
	if (leds[led->id].effect_substep == effect_step->substep_count) {
		leds[led->id].effect_substep = 0;
		leds[led->id].effect_step++;

		if (leds[led->id].effect_step == leds[led->id].effect->step_count) {
			if (leds[led->id].effect->loop_count == 0) {
				leds[led->id].effect_step = 0;
			} else {
				leds[led->id].effect_loop++;
				if (leds[led->id].effect_loop < leds[led->id].effect->loop_count) {
					leds[led->id].effect_step = 0;
				}
			}
		} else {
			__ASSERT_NO_MSG(leds[led->id].effect->steps[leds[led->id].effect_step].substep_count > 0);
		}
	}

	if (leds[led->id].effect_step < leds[led->id].effect->step_count) {
		int32_t next_delay =
			leds[led->id].effect->steps[leds[led->id].effect_step].substep_time;

		k_delayed_work_submit(&leds[led->id].work, K_MSEC(next_delay));
	}
}

static void led_update(struct led *led)
{
	k_delayed_work_cancel(&led->work);

	led->effect_step = 0;
	led->effect_substep = 0;
	led->effect_loop = 0;

	if (!led->effect) {
		LOG_DBG("No effect set");
		return;
	}

	__ASSERT_NO_MSG(led->effect->steps);

	if (led->effect->step_count > 0) {
		int32_t next_delay =
			led->effect->steps[led->effect_step].substep_time;

		k_delayed_work_submit(&led->work, K_MSEC(next_delay));
	} else {
		LOG_DBG("LED effect with no effect");
	}
}


static void lte_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		if (evt->nw_reg_status == LTE_LC_NW_REG_UICC_FAIL) {
			LOG_INF("Network registration fail: UICC");
#if defined(CONFIG_SLM_DIAG)
			slm_diag_set_event(SLM_DIAG_UICC_FAIL);
#endif
		} else if (evt->nw_reg_status == LTE_LC_NW_REG_SEARCHING) {
			LOG_INF("Network registration status: Connecting");
			ui_led_set_state(LED_ID_LTE, UI_LTE_CONNECTING);
		} else if ((evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME) ||
			(evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING)) {
			LOG_INF("Network registration status: %s",
			evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ?
			"Connected - home network" : "Connected - roaming");
			ui_led_set_state(LED_ID_LTE, UI_LTE_CONNECTED);
#if defined(CONFIG_SLM_DIAG)
			slm_diag_clear_event(SLM_DIAG_UICC_FAIL);
#endif
		} else if ((evt->nw_reg_status == LTE_LC_NW_REG_NOT_REGISTERED) ||
			(evt->nw_reg_status == LTE_LC_NW_REG_UNKNOWN)) {
			ui_led_set_state(LED_ID_LTE, UI_LTE_DISCONNECTED);
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

/**@brief Callback handler for LTE RSRP data. */
static void modem_rsrp_handler(char rsrp_value)
{
	static int32_t modem_rsrp;
	/* Only send a value from a valid range (0 - 97). */
	if (rsrp_value > 97) {
		ui_led_set_state(LED_ID_SIGNAL, UI_SIGNAL_OFF);
		return;
	}

	if (rsrp_value < RSRP_THRESHOLD_1) {
		ui_led_set_state(LED_ID_SIGNAL, UI_SIGNAL_L0);
	} else if (rsrp_value >= RSRP_THRESHOLD_1 && rsrp_value < RSRP_THRESHOLD_2) {
		ui_led_set_state(LED_ID_SIGNAL, UI_SIGNAL_L1);
	} else if (rsrp_value >= RSRP_THRESHOLD_2 && rsrp_value < RSRP_THRESHOLD_3) {
		ui_led_set_state(LED_ID_SIGNAL, UI_SIGNAL_L2);
	} else if (rsrp_value >= RSRP_THRESHOLD_3 && rsrp_value < RSRP_THRESHOLD_4) {
		ui_led_set_state(LED_ID_SIGNAL, UI_SIGNAL_L3);
	} else {
		ui_led_set_state(LED_ID_SIGNAL, UI_SIGNAL_L4);
	}

	modem_rsrp = (int8_t)rsrp_value - MODEM_INFO_RSRP_OFFSET_VAL;
	LOG_DBG("rsrp:%d", modem_rsrp);
}

void ui_led_set_state(enum led_id id, enum ui_led_state state)
{
	LOG_DBG("LED %d state change to: %d", id, state);
	leds[id].effect = &led_effect_list[state];
	led_update(&leds[id]);
}
/*
enum ui_led_pattern ui_led_get_pattern(void)
{
	return current_led_state;
}
*/
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

	modem_info_rsrp_register(modem_rsrp_handler);

	err = dk_set_leds_state(DK_NO_LEDS_MSK, DK_ALL_LEDS_MSK);
	if (err) {
		LOG_ERR("Could not set leds state, err code: %d", err);
		return err;
	}

	leds[LED_ID_LTE].id = LED_ID_LTE;
	k_delayed_work_init(&leds[LED_ID_LTE].work, work_handler);
	leds[LED_ID_DATA].id = LED_ID_DATA;
	k_delayed_work_init(&leds[LED_ID_DATA].work, work_handler);
	leds[LED_ID_SIGNAL].id = LED_ID_SIGNAL;
	k_delayed_work_init(&leds[LED_ID_SIGNAL].work, work_handler);

	ui_gpio_dev = device_get_binding(DT_LABEL(DT_NODELABEL(gpio0)));
	if (ui_gpio_dev == NULL) {
		LOG_ERR("GPIO_0 for UI bind error");
		err = -EINVAL;
	}
	err = gpio_pin_configure(ui_gpio_dev, CONFIG_SLM_RI_PIN,
				GPIO_OUTPUT);
	if (err) {
		LOG_ERR("CONFIG_SLM_RI_PIN config error: %d", err);
		return err;
	}

	return err;
}
