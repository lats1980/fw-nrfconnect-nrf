/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef SLM_AT_UC_
#define SLM_AT_UC_

/**@file slm_at_uc.h
 *
 * @brief Vendor-specific AT command for settings service.
 * @{
 */

/**
 * @brief Initialize user config AT command service.
 *
 * @retval 0 If the operation was successful.
 *           Otherwise, a (negative) error code is returned.
 */
int slm_at_uc_init(void);

/**
 * @brief Uninitialize user config AT command service.
 *
 * @retval 0 If the operation was successful.
 *           Otherwise, a (negative) error code is returned.
 */
int slm_at_uc_uninit(void);

/** @} */

#endif /* SLM_AT_UC_ */
