/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

/**@file
 *
 * @brief   User interface module for serial LTE modem.
 *
 * Module that handles user interaction through LEDs.
 */

#ifndef SLM_UI_H__
#define SLM_UI_H__

#include <zephyr.h>
#include <dk_buttons_and_leds.h>

#define UI_LED_ON_PERIOD_NORMAL		500
#define UI_LED_OFF_PERIOD_NORMAL	500

/**@brief UI LED state pattern definitions. */
enum ui_led_pattern {
	UI_LTE_DISCONNECTED,
	UI_LTE_CONNECTING,
	UI_LTE_CONNECTED
};

struct led_effect {
	uint32_t led_mask;
	uint32_t period;
	uint32_t loop_count;
};

/**
 * @brief Initializes the user interface module.
 *
 * @return 0 on success or negative error value on failure.
 */
int slm_ui_init(void);

/**
 * @brief Sets the LED pattern.
 *
 * @param pattern LED pattern.
 */
void ui_led_set_pattern(enum ui_led_pattern pattern);

#endif /* SLM_UI_H__ */
