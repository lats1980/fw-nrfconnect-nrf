/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef SLM_AT_TWI_
#define SLM_AT_TWI_

/**@file slm_at_twi.h
 *
 * @brief Vendor-specific AT command for TWI service.
 * @{
 */

#include <zephyr/types.h>
#include "slm_at_host.h"

/**
 * @brief TWI AT command parser.
 *
 * @param at_cmd AT command or data string.
 *
 * @retval 0 If the operation was successful.
 *           Otherwise, negative code means error.
 */
int slm_at_twi_parse(const char *at_cmd);

/**
 * @brief Initialize TWI AT command parser.
 *
 * @retval 0 If the operation was successful.
 *           Otherwise, a (negative) error code is returned.
 */
int slm_at_twi_init(void);

/**
 * @brief Uninitialize TWI AT command parser.
 *
 * @retval 0 If the operation was successful.
 *           Otherwise, a (negative) error code is returned.
 */
int slm_at_twi_uninit(void);

/**
 * @brief List TWI AT commands.
 *
 */
void slm_at_twi_clac(void);

/** @} */

#endif /* SLM_AT_TWI_ */
