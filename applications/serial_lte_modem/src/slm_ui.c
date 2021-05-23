/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <stdio.h>
#include <logging/log.h>

#include "slm_ui.h"
#include <drivers/gpio.h>
#if defined(CONFIG_SLM_GPIO)
#include "slm_at_gpio.h"
#endif
#if defined(CONFIG_SLM_DIAG)
#include "slm_diag.h"
#endif
extern const struct device *gpio_dev;

LOG_MODULE_REGISTER(ui, CONFIG_SLM_LOG_LEVEL);

static struct led leds[LED_ID_COUNT];

static void work_handler(struct k_work *work)
{
	struct led *led = CONTAINER_OF(work, struct led, work);
	int slm_pin = 0;
	const struct led_effect_step *effect_step =
		&led->effect->steps[led->effect_step];

	__ASSERT_NO_MSG(led->effect->steps[led->effect_step].substep_cnt > 0);
	slm_pin = slm_gpio_get_ui_pin(led->fn);
	if (slm_pin < 0) {
		LOG_ERR("Fail to get UI pin");
		return;
	}
	LOG_DBG("Set UI pin %d state %d", slm_pin, effect_step->led_on);
	gpio_pin_set(gpio_dev, (gpio_pin_t)slm_pin, effect_step->led_on);

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

void ui_led_set_state(enum led_id id, enum ui_led_state new_state)
{
	LOG_DBG("LED %d state change to: %d", id, new_state);
	/* If state is mute, only accept unmute as new state */
	if (leds[id].state == UI_MUTE) {
		if (new_state != UI_UNMUTE) {
			LOG_DBG("Ignore while in mute state");
			return;
		}
	}
	leds[id].state = new_state;
	leds[id].effect = &led_effect_list[new_state];
	led_update(&leds[id]);
}

int slm_ui_init(void)
{
	int err = -EINVAL;

	err = slm_ui_set(UI_LEDS_OFF);
	if (err != 0) {
		LOG_ERR("Could not set UI state, err code: %d", err);
	}
#if defined(CONFIG_SLM_UI_LTE_STATE)
	leds[LED_ID_LTE].fn = SLM_GPIO_FN_LTE;
	leds[LED_ID_LTE].state = UI_LTE_DISCONNECTED;
	do_gpio_pin_configure_set(CONFIG_SLM_UI_LTE_STATE_PIN, SLM_GPIO_FN_LTE);
	k_delayed_work_init(&leds[LED_ID_LTE].work, work_handler);
#endif
#if defined(CONFIG_SLM_UI_DATA_ACTIVITY)
	leds[LED_ID_DATA].fn = SLM_GPIO_FN_DATA;
	leds[LED_ID_DATA].state = UI_DATA_NONE;
	do_gpio_pin_configure_set(CONFIG_SLM_UI_DATA_ACTIVITY_PIN, SLM_GPIO_FN_DATA);
	k_delayed_work_init(&leds[LED_ID_DATA].work, work_handler);
#endif
#if defined(CONFIG_SLM_UI_LTE_SIGNAL)
	leds[LED_ID_SIGNAL].fn = SLM_GPIO_FN_SIGNAL;
	leds[LED_ID_SIGNAL].state = UI_SIGNAL_OFF;
	do_gpio_pin_configure_set(CONFIG_SLM_UI_LTE_SIGNAL_PIN, SLM_GPIO_FN_SIGNAL);
	k_delayed_work_init(&leds[LED_ID_SIGNAL].work, work_handler);
#endif
#if defined(CONFIG_SLM_UI_DIAG)
	leds[LED_ID_DIAG].fn = SLM_GPIO_FN_DIAG;
	leds[LED_ID_DIAG].state = UI_DIAG_OFF;
	do_gpio_pin_configure_set(CONFIG_SLM_UI_DIAG_PIN, SLM_GPIO_FN_DIAG);
	k_delayed_work_init(&leds[LED_ID_DIAG].work, work_handler);
#endif

	err = gpio_pin_configure(gpio_dev, CONFIG_SLM_RI_PIN, GPIO_OUTPUT);
	if (err) {
		LOG_ERR("CONFIG_SLM_RI_PIN config error: %d", err);
		return err;
	}
	err = gpio_pin_configure(gpio_dev, CONFIG_SLM_DCD_PIN, GPIO_OUTPUT);
	if (err) {
		LOG_ERR("CONFIG_SLM_DCD_PIN config error: %d", err);
		return err;
	}
#if defined(CONFIG_SLM_MOD_FLASH)
	leds[LED_ID_MOD_LED].fn = SLM_GPIO_FN_MOD_FLASH;
	leds[LED_ID_MOD_LED].state = UI_ONLINE_OFF;
	do_gpio_pin_configure_set(CONFIG_SLM_MOD_FLASH_PIN, SLM_GPIO_FN_MOD_FLASH);
	k_delayed_work_init(&leds[LED_ID_MOD_LED].work, work_handler);
#endif

	return err;
}

int slm_ui_uninit(void)
{
	int err = -EINVAL;

#if defined(CONFIG_SLM_UI_LTE_STATE)
	leds[LED_ID_LTE].fn = LED_ID_LTE;
	leds[LED_ID_LTE].state = UI_LTE_DISCONNECTED;
	k_delayed_work_cancel(&leds[LED_ID_LTE].work);
#endif
#if defined(CONFIG_SLM_UI_DATA_ACTIVITY)
	leds[LED_ID_DATA].fn = LED_ID_DATA;
	leds[LED_ID_DATA].state = UI_DATA_NONE;
	k_delayed_work_cancel(&leds[LED_ID_DATA].work);
#endif
#if defined(CONFIG_SLM_UI_LTE_SIGNAL)
	leds[LED_ID_SIGNAL].fn = LED_ID_SIGNAL;
	leds[LED_ID_SIGNAL].state = UI_SIGNAL_OFF;
	k_delayed_work_cancel(&leds[LED_ID_SIGNAL].work);
#endif
#if defined(CONFIG_SLM_UI_DIAG)
	leds[LED_ID_DIAG].fn = SLM_GPIO_FN_DIAG;
	leds[LED_ID_DIAG].state = UI_DIAG_OFF;
	k_delayed_work_cancel(&leds[LED_ID_DIAG].work);
#endif
#if defined(CONFIG_SLM_MOD_FLASH)
	leds[LED_ID_MOD_LED].fn = SLM_GPIO_FN_MOD_FLASH;
	leds[LED_ID_MOD_LED].state = UI_ONLINE_OFF;
	k_delayed_work_cancel(&leds[LED_ID_MOD_LED].work);
#endif
	err = slm_ui_set(UI_LEDS_OFF);
	if (err != 0) {
		LOG_ERR("Could not set UI state, err code: %d", err);
	}

	return err;
}

int slm_ui_set(enum ui_leds_state state)
{
	int err = -EINVAL, onoff = 0, slm_pin = 0;

	if (state != UI_LEDS_OFF && state != UI_LEDS_ON) {
		return err;
	}

	if (state == UI_LEDS_ON) {
		onoff = 1;
	}

	for (int i = 0; i < LED_ID_COUNT; i++) {
		slm_pin = slm_gpio_get_ui_pin(leds[i].fn);
		if (slm_pin < 0) {
			LOG_ERR("Fail to get UI pin");
			return err;
		}
		err = gpio_pin_set(gpio_dev, (gpio_pin_t)slm_pin, onoff);
		if (err != 0) {
			LOG_ERR("Fail to set GPIO pin: %d", slm_pin);
			break;
		}
	}

	return err;
}
