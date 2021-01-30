/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#ifndef SLM_AT_STATS_
#define SLM_AT_STATS_

/**@file slm_at_stats.h
 *
 * @brief Vendor-specific AT command for SLM statistics.
 * @{
 */

#include <zephyr/types.h>
#include "slm_at_host.h"

/**
 * @brief Initialize SLM stats.
 *
 * @retval 0 If the operation was successful.
 *           Otherwise, a (negative) error code is returned.
 */
int slm_at_stats_init(void);

/**
 * @brief Uninitialize SLM stats.
 *
 * @retval 0 If the operation was successful.
 *           Otherwise, a (negative) error code is returned.
 */
int slm_at_stats_uninit(void);

/**
 * @brief STATS AT command parser.
 *
 * @param at_cmd  AT command string.
 *
 * @retval 0 If the operation was successful.
 *           Otherwise, a (negative) error code is returned.
 */
int slm_at_stats_parse(const char *at_cmd);

/**
 * @brief List SLM stats AT commands.
 *
 */
void slm_at_stats_clac(void);

/** @} */

#endif /* SLM_AT_HTTPC_ */
