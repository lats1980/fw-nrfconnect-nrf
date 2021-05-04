/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <stdio.h>
#include <logging/log.h>

#include "slm_ui.h"
#include <drivers/gpio.h>
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
		&led->effect->steps[led->effect_step];

	__ASSERT_NO_MSG(led->effect->steps[led->effect_step].substep_cnt > 0);
	LOG_DBG("LED %d state %d", led_map[led->id], effect_step->led_on);
	dk_set_led(led_map[led->id], effect_step->led_on);

	led->effect_substep++;
	if (led->effect_substep == effect_step->substep_cnt) {
		led->effect_substep = 0;
		led->effect_step++;

		if (led->effect_step == led->effect->step_cnt) {
			if (led->effect->loop_cnt == 0) {
				led->effect_step = 0;
			} else {
				led->effect_loop++;
				if (led->effect_loop < led->effect->loop_cnt) {
					led->effect_step = 0;
				}
			}
		}
	}

	if (led->effect_step < led->effect->step_cnt) {
		int32_t next_delay =
			led->effect->steps[led->effect_step].substep_time;

		k_delayed_work_submit(&led->work, K_MSEC(next_delay));
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

	if (led->effect->step_cnt > 0) {
		int32_t next_delay =
			led->effect->steps[led->effect_step].substep_time;

		k_delayed_work_submit(&led->work, K_MSEC(next_delay));
	} else {
		LOG_DBG("LED effect with no effect");
	}
}

void ui_led_set_state(enum led_id id, enum ui_led_state state)
{
	LOG_DBG("LED %d state change to: %d", id, state);
	if (leds[id].state == state && (state < UI_DATA_NONE || state > UI_DATA_FAST)) {
		return;
	}
	leds[id].state = state;
	leds[id].effect = &led_effect_list[state];
	led_update(&leds[id]);
}

int slm_ui_init(void)
{
	int err = 0;

	err = dk_leds_init();
	if (err) {
		LOG_ERR("Could not initialize leds, err code: %d", err);
		return err;
	}

	err = dk_set_leds_state(DK_NO_LEDS_MSK, DK_ALL_LEDS_MSK);
	if (err) {
		LOG_ERR("Could not set leds state, err code: %d", err);
		return err;
	}

	leds[LED_ID_LTE].id = LED_ID_LTE;
	leds[LED_ID_LTE].state = UI_LTE_DISCONNECTED;
	k_delayed_work_init(&leds[LED_ID_LTE].work, work_handler);
	leds[LED_ID_DATA].id = LED_ID_DATA;
	leds[LED_ID_DATA].state = UI_LTE_DISCONNECTED;
	k_delayed_work_init(&leds[LED_ID_DATA].work, work_handler);
	leds[LED_ID_SIGNAL].id = LED_ID_SIGNAL;
	leds[LED_ID_SIGNAL].state = UI_SIGNAL_OFF;
	k_delayed_work_init(&leds[LED_ID_SIGNAL].work, work_handler);

	ui_gpio_dev = device_get_binding(DT_LABEL(DT_NODELABEL(gpio0)));
	if (ui_gpio_dev == NULL) {
		LOG_ERR("GPIO_0 for UI bind error");
		return -EINVAL;
	}
	err = gpio_pin_configure(ui_gpio_dev, CONFIG_SLM_RI_PIN,
				GPIO_OUTPUT);
	if (err) {
		LOG_ERR("CONFIG_SLM_RI_PIN config error: %d", err);
		return err;
	}
	err = gpio_pin_configure(ui_gpio_dev, CONFIG_SLM_DCD_PIN,
				GPIO_OUTPUT);
	if (err) {
		LOG_ERR("CONFIG_SLM_DCD_PIN config error: %d", err);
		return err;
	}

	return err;
}

int slm_ui_uninit(void)
{
	int err = 0;

	err = dk_set_leds_state(DK_NO_LEDS_MSK, DK_ALL_LEDS_MSK);
	if (err) {
		LOG_ERR("Could not set leds state, err code: %d", err);
		return err;
	}

	leds[LED_ID_LTE].id = LED_ID_LTE;
	leds[LED_ID_LTE].state = UI_LTE_DISCONNECTED;
	k_delayed_work_cancel(&leds[LED_ID_LTE].work);
	leds[LED_ID_DATA].id = LED_ID_DATA;
	leds[LED_ID_DATA].state = UI_DATA_NONE;
	k_delayed_work_cancel(&leds[LED_ID_DATA].work);
	leds[LED_ID_SIGNAL].id = LED_ID_SIGNAL;
	leds[LED_ID_SIGNAL].state = UI_SIGNAL_OFF;
	k_delayed_work_cancel(&leds[LED_ID_SIGNAL].work);

	return err;
}
