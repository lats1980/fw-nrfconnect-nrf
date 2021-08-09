/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include "slm_native_tls_ps.h"

#include <logging/log.h>
#include <psa/protected_storage.h>

LOG_MODULE_REGISTER(slm_tls_ps, CONFIG_SLM_LOG_LEVEL);

/* Most significant 32 bits of UID for TLS credentials with type 0 (arbitrary)*/
#define SLM_TLS_PS_BASE 0x140DCAF0ULL

/* Conversion from type and sec_tag to 64-bit UID */
#define SLM_TLS_PS_UID(type, sec_tag) \
	(((SLM_TLS_PS_BASE + type) << 32) + sec_tag)

/* Return corresponding error code for PSA Protected Storage statuses */
static int translate_status(psa_status_t status)
{
	switch (status) {
	case PSA_SUCCESS:
		return 0;
	case PSA_ERROR_NOT_PERMITTED:
		return -EPERM;
	case PSA_ERROR_INVALID_ARGUMENT:
		return -EINVAL;
	case PSA_ERROR_NOT_SUPPORTED:
		return -ENOTSUP;
	case PSA_ERROR_INSUFFICIENT_STORAGE:
		return -ENOSPC;
	case PSA_ERROR_DOES_NOT_EXIST:
		return -ENOENT;
	case PSA_ERROR_STORAGE_FAILURE:
	case PSA_ERROR_DATA_CORRUPT:
	case PSA_ERROR_INVALID_SIGNATURE:
	case PSA_ERROR_GENERIC_ERROR:
	default:
		return -EIO;
	}

	/* Unreachable */
	return status;
}

int slm_tls_ps_set(sec_tag_t sec_tag, uint16_t type, const void *buf,
		   size_t len)
{
	psa_status_t status;
	psa_storage_uid_t uid;
	psa_storage_create_flags_t flags = 0;

	if (IS_ENABLED(CONFIG_SLM_NATIVE_TLS_PS_WRITE_ONCE)) {
		flags |= PSA_STORAGE_FLAG_WRITE_ONCE;
	}

	uid = SLM_TLS_PS_UID(type, sec_tag);
	status = psa_ps_set(uid, len, buf, flags);

	if (status != PSA_SUCCESS) {
		LOG_ERR("Could not set credential. Error %d", status);
	}

	return translate_status(status);
}

int slm_tls_ps_get(sec_tag_t sec_tag, uint16_t type, void *buf, size_t buf_len,
		   size_t *len)
{
	psa_status_t status;
	psa_storage_uid_t uid;

	uid = SLM_TLS_PS_UID(type, sec_tag);
	status = psa_ps_get(uid, 0, buf_len, buf, len);

	if (status != PSA_SUCCESS) {
		LOG_ERR("Could not get credential. Error %d", status);
	}

	return translate_status(status);
}

int slm_tls_ps_remove(sec_tag_t sec_tag, uint16_t type)
{
	psa_status_t status;
	psa_storage_uid_t uid;

	uid = SLM_TLS_PS_UID(type, sec_tag);
	status = psa_ps_remove(uid);

	if (status != PSA_SUCCESS) {
		LOG_ERR("Could not remove credential. Error %d", status);
	}

	return translate_status(status);
}
