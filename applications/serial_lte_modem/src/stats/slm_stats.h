/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */

#ifndef SLM_STATS_
#define SLM_STATS_

/**@file slm_stats.h
 *
 * @brief functions to collect SLM statistics
 * @{
 */

/**
 * @brief Initialize SLM stats collector.
 *
 * @retval 0 If the operation was successful.
 *           Otherwise, a (negative) error code is returned.
 */
int slm_stats_init(void);

/**
 * @brief Uninitialize SLM stats collector.
 *
 * @retval 0 If the operation was successful.
 *           Otherwise, a (negative) error code is returned.
 */
int slm_stats_uninit(void);

/**
 * @brief subscribe network and registration stats
 *
 * @retval 0 If the operation was successful.
 *           Otherwise, a (negative) error code is returned.
 */
int slm_stats_subscribe(void);

/**
 * @brief get network and registration stats
 *
 * @retval 0 If the operation was successful.
 *           Otherwise, a (negative) error code is returned.
 */
int slm_stats_get_nw_reg_status(void);

/** @} */

#endif /* SLM_STATS_ */
