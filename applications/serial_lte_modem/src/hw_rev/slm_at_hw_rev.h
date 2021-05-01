/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef SLM_AT_HW_REV_
#define SLM_AT_HW_REV_

/**@file slm_at_hw_rev.h
 *
 * @brief Vendor-specific AT command for hardware revision service.
 * @{
 */

/**
 * @brief Initialize HW REV AT command parser.
 *
 * @retval 0 If the operation was successful.
 *           Otherwise, a (negative) error code is returned.
 */
int slm_at_hw_rev_init(void);

/**
 * @brief Uninitialize HW REV AT command parser.
 *
 * @retval 0 If the operation was successful.
 *           Otherwise, a (negative) error code is returned.
 */
int slm_at_hw_rev_uninit(void);

/** @} */

#endif /* SLM_AT_HW_REV_ */
