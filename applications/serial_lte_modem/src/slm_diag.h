/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

/**@file
 *
 * @brief   Diagnostic module for serial LTE modem.
 *
 * Module that handles error diagnostic indicator.
 */

#ifndef SLM_DIAG_H__
#define SLM_DIAG_H__

#include <zephyr.h>

enum slm_diag_event {
	SLM_DIAG_RADIO_FAIL = 1,
	SLM_DIAG_LOW_BATTERY,
	SLM_DIAG_UICC_FAIL,
	SLM_DIAG_DATA_CONNECTION_FAIL,
	SLM_DIAG_CALL_FAIL,

	SLM_DIAG_EVENT_COUNT
};

/**
 * @brief Initializes the diagnostic module.
 *
 * @return 0 on success or negative error value on failure.
 */
int slm_diag_init(void);

/**
 * @brief Un-initializes the diagnostic module.
 *
 * @return 0 on success or negative error value on failure.
 */
int slm_diag_uninit(void);

void slm_diag_set_event(enum slm_diag_event event);

void slm_diag_clear_event(enum slm_diag_event event);

#endif /* SLM_DIAG_H__ */
