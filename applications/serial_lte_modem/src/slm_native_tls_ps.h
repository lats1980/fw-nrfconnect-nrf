/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef SLM_NATIVE_TLS_PS
#define SLM_NATIVE_TLS_PS

#include <zephyr/types.h>
#include <net/tls_credentials.h>

/**@file slm_native_tls.h
 *
 * @brief Storage of TLS credentials using PSA Protected Storage API
 * @{
 */

/**
 * @brief Store a TLS credential in Protected Storage
 *
 * @param[in]  sec_tag security tag of the credential
 * @param[in]  type TLS credential type, as used in AT%CMNG
 * @param[in]  buf Buffer containing the credential
 * @param[in]  len Size of the credential
 *
 * @return 0 if successful, negative error code if failure.
 */
int slm_tls_ps_set(sec_tag_t sec_tag, uint16_t type, const void *buf,
		   size_t len);
/**
 * @brief Get a TLS credential from Protected Storage
 *
 * @param[in]  sec_tag security tag of the credential
 * @param[in]  type TLS credential type, as used in AT%CMNG
 * @param[out] buf Buffer in which to write the credential
 * @param[in]  buf_len Size of the buffer
 * @param[out] len Size of the credential
 *
 * @return 0 if successful, negative error code if failure.
 */
int slm_tls_ps_get(sec_tag_t sec_tag, uint16_t type, void *buf, size_t buf_len,
		   size_t *len);
/**
 * @brief Remove a TLS credential from Protected Storage
 *
 * @param[in]  sec_tag security tag of the credential
 * @param[in]  type TLS credential type, as used in AT%CMNG
 *
 * @return 0 if successful, negative error code if failure.
 */
int slm_tls_ps_remove(sec_tag_t sec_tag, uint16_t type);

/** @} */

#endif /* SLM_NATIVE_TLS_PS */
