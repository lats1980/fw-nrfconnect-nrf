/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#include <stdio.h>
#include <logging/log.h>

#include "slm_diag.h"
#if defined(CONFIG_SLM_UI)
#include "slm_ui.h"
#include <dk_buttons_and_leds.h>
#endif

LOG_MODULE_REGISTER(diag, CONFIG_SLM_LOG_LEVEL);

#define DIAG_INTER_EVENT_PERIOD 3000

static uint32_t slm_diag_event_mask;
static struct k_delayed_work slm_diag_update_work;

static void diag_event_update(struct k_work *work)
{
	static uint8_t current_diag_event, current_step;
	static bool led_on;

	if (slm_diag_event_mask == 0){
		k_delayed_work_submit(&slm_diag_update_work, K_MSEC(500));
	}

	if ( slm_diag_event_mask & 1 << current_diag_event) {
		LOG_DBG("Diag mask: %d event:%d",
			slm_diag_event_mask,
			current_diag_event);
		dk_set_led(LED_ID_ERROR, !led_on);
		led_on = !led_on;
		if (!led_on) {
			current_step++;
		}
		if(current_step == current_diag_event) {
			k_delayed_work_submit(&slm_diag_update_work,
					      K_MSEC(DIAG_INTER_EVENT_PERIOD));
			current_diag_event++;
			current_step = 0;
		} else {
			k_delayed_work_submit(&slm_diag_update_work,
					      K_MSEC(500));
		}
	} else {
		LOG_DBG("Diag mask: %d event:%d",
			slm_diag_event_mask,
			current_diag_event);
		current_diag_event++;
		k_delayed_work_submit(&slm_diag_update_work, K_MSEC(100));
	}
	if (current_diag_event == SLM_DIAG_EVENT_COUNT) {
		current_diag_event = SLM_DIAG_RADIO_FAIL;
	}
}

int slm_diag_init(void)
{
	int err = 0;

	k_delayed_work_init(&slm_diag_update_work, diag_event_update);
	k_delayed_work_submit(&slm_diag_update_work, K_NO_WAIT);

	return err;
}

int slm_diag_uninit(void)
{
	int err = 0;

	k_delayed_work_cancel(&slm_diag_update_work);

	return err;
}

void slm_diag_set_event(enum slm_diag_event event)
{
	slm_diag_event_mask |= 1 << event;
}

void slm_diag_clear_event(enum slm_diag_event event)
{
	slm_diag_event_mask &= ~(1UL << event);
}
