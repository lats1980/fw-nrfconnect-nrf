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

#define RSRP_THRESHOLD_1			20
#define RSRP_THRESHOLD_2			40
#define RSRP_THRESHOLD_3			60
#define RSRP_THRESHOLD_4			80

/* LED state list */
enum ui_led_state {
	UI_LTE_DISCONNECTED,
	UI_LTE_CONNECTING,
	UI_LTE_CONNECTED,
	UI_DATA_NONE,
	UI_DATA_SLOW,
	UI_DATA_NORMAL,
	UI_DATA_FAST,
	UI_SIGNAL_OFF,
	UI_SIGNAL_L0,
	UI_SIGNAL_L1,
	UI_SIGNAL_L2,
	UI_SIGNAL_L3,
	UI_SIGNAL_L4,

	LED_LTE_STATE_COUNT
};

/* LED ID List */
enum led_id {
	LED_ID_LTE,
	LED_ID_DATA,
	LED_ID_SIGNAL,
	LED_ID_ERROR,

	LED_ID_COUNT
};

/* Map function to LED ID */
static const uint8_t led_map[LED_ID_COUNT] = {
	[LED_ID_LTE] = 0,
	[LED_ID_DATA] = 1,
	[LED_ID_SIGNAL] = 2,
	[LED_ID_ERROR] = 3
};

struct led_effect_step {
	bool led_on;
	uint16_t substep_cnt;
	uint16_t substep_time;
};

struct led_effect {
	struct led_effect_step *steps;
	uint16_t step_cnt;
	uint16_t loop_cnt;
};

struct led {
	size_t id;
	enum ui_led_state state;
	const struct led_effect *effect;
	uint16_t effect_step;
	uint16_t effect_substep;
	uint16_t effect_loop;

	struct k_delayed_work work;
};

/* LED EFFECTS Definition */
#define LED_EFFECT_LED_ON()				\
	{						\
		.steps = ((struct led_effect_step[]) {	\
			{				\
				.led_on = true,		\
				.substep_cnt = 1,	\
				.substep_time = 0,	\
			},				\
		}),					\
		.step_cnt = 1,				\
		.loop_cnt = 1,				\
	}


#define LED_EFFECT_LED_OFF()				\
	{						\
		.steps = ((struct led_effect_step[]) {	\
			{				\
				.led_on = false,	\
				.substep_cnt = 1,	\
				.substep_time = 0,	\
			},				\
		}),					\
		.step_cnt = 1,				\
		.loop_cnt = 1,				\
	}

#define LED_EFFECT_LED_BLINK(_period, _loop_cnt)		\
	{							\
		.steps = ((struct led_effect_step[]) {		\
			{					\
				.led_on = true,			\
				.substep_cnt = 1,		\
				.substep_time = (_period),	\
			},					\
			{					\
				.led_on = false,		\
				.substep_cnt = 1,		\
				.substep_time = (_period),	\
			},					\
		}),						\
		.step_cnt = 2,					\
		.loop_cnt = _loop_cnt,				\
	}

static const struct led_effect led_effect_list[LED_LTE_STATE_COUNT] = {
	[UI_LTE_DISCONNECTED]     = LED_EFFECT_LED_OFF(),
	[UI_LTE_CONNECTING] = LED_EFFECT_LED_BLINK(500, 0),
	[UI_LTE_CONNECTED]    = LED_EFFECT_LED_ON(),
	[UI_DATA_NONE]    = LED_EFFECT_LED_OFF(),
	[UI_DATA_SLOW]    = LED_EFFECT_LED_BLINK(50, 1),
	[UI_DATA_NORMAL]    = LED_EFFECT_LED_BLINK(50, 3),
	[UI_DATA_FAST]    = LED_EFFECT_LED_BLINK(50, 5),
	[UI_SIGNAL_OFF] = LED_EFFECT_LED_OFF(),
	[UI_SIGNAL_L0] = LED_EFFECT_LED_BLINK(1000, 0),
	[UI_SIGNAL_L1] = LED_EFFECT_LED_BLINK(1000, 0),
	[UI_SIGNAL_L2] = LED_EFFECT_LED_BLINK(200, 0),
	[UI_SIGNAL_L3] = LED_EFFECT_LED_BLINK(200, 0),
	[UI_SIGNAL_L4] = LED_EFFECT_LED_BLINK(200, 0),
};

/**
 * @brief Initializes the user interface module.
 *
 * @return 0 on success or negative error value on failure.
 */
int slm_ui_init(void);

/**
 * @brief Un-initializes the user interface module.
 *
 * @return 0 on success or negative error value on failure.
 */
int slm_ui_uninit(void);

/**
 * @brief Mute/unmute UI.
 */
void slm_ui_mute(void);

/**
 * @brief Sets LED effect based in UI LED state.
 */
void ui_led_set_state(enum led_id, enum ui_led_state state);

#endif /* SLM_UI_H__ */
