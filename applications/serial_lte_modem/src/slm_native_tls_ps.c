/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include "slm_native_tls_ps.h"
#include "slm_native_tls.h"
#include <logging/log.h>
#include <stdio.h>
#include <zephyr.h>
#include <psa/protected_storage.h>
LOG_MODULE_REGISTER(slm_tls_ps, CONFIG_SLM_LOG_LEVEL);

/* global functions defined in different files */
void rsp_send(const uint8_t *str, size_t len);

/* global variable defined in different files */
extern struct at_param_list at_param_list;
extern char rsp_buf[CONFIG_AT_CMD_RESPONSE_MAX_LEN];

/* Most significant 32 bits of UID for TLS credentials with type 0 (arbitrary)*/
#define SLM_TLS_PS_BASE 0x140DCAF0ULL
#define SLM_TLS_TBL_UUID	0xD98A7DBBB4DCBF2CULL

/* Conversion from type and sec_tag to 64-bit UID */
#define SLM_TLS_PS_UID(type, sec_tag) \
	(((SLM_TLS_PS_BASE + type) << 32) + sec_tag)
#define SLM_TLS_TYPE_LENGTH (2)
#define SLM_TLS_TBL_MAX_SIZE (NATIVE_TLS_SEC_TAG_COUNT * SLM_TLS_TYPE_LENGTH)
#define SLM_SEC_TAG_TO_SLOT(sec_tag, type) \
	((sec_tag - MIN_NATIVE_TLS_SEC_TAG) * SLM_TLS_TYPE_LENGTH + ((type > UINT8_MAX) ? 1:0))
#define SLM_SLOT_TO_SEC_TAG(slot) \
	((slot/SLM_TLS_TYPE_LENGTH) + MIN_NATIVE_TLS_SEC_TAG)
#define SLM_TLS_PSA_CREDENTIAL_TYPE_MAX (16)
/* pointer to credential table buffer */
static uint8_t *crdl_tbl;
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

/* Conversion from type and sec_tag to SLM_TLS_TBL_MAX_SIZE bytes bitmask */
inline static void SLM_TLS_TBL_MAP(char* buf, sec_tag_t sec_tag, uint16_t type, bool set)
{
	int16_t slot = SLM_SEC_TAG_TO_SLOT(sec_tag, type);
	if(type >= 8) {
		WRITE_BIT(buf[slot + 1], (type % 8) , set);
	} else {
		WRITE_BIT(buf[slot], type, set);
	}
}
void slm_tls_tbl_dump(void)
{
	size_t len;
	psa_status_t status;
	sec_tag_t sec_tag;
	uint16_t type;
	char xrsp_buf[32];

	crdl_tbl = k_malloc(SLM_TLS_TBL_MAX_SIZE);
	status = slm_tls_tbl_get(crdl_tbl, &len);
	if(status != PSA_SUCCESS) {
		LOG_ERR("get psa tbl failed %d",status);
	} else {
		LOG_DBG("slm_tls_tbl_get %d", len);
		for(int i = 0; i < SLM_TLS_TBL_MAX_SIZE;) {
			if(crdl_tbl[i] != 0 || crdl_tbl[i+1] !=0 ) {
				sec_tag = SLM_SLOT_TO_SEC_TAG(i);
				type = (crdl_tbl[i+1] << 8) | (crdl_tbl[i]);
				for(uint8_t j=0; j < UINT8_MAX ;j++) {
					if(type & BIT(j)) {
						sprintf(xrsp_buf, "#XCMNG: %u,%u\r\n", sec_tag, j);
						rsp_send(xrsp_buf, strlen(xrsp_buf));
					}
				}
			}
			i+=SLM_TLS_TYPE_LENGTH;
		}
	}

	k_free(crdl_tbl);
}
int slm_tls_tbl_get(void *buf, size_t *len)
{
	psa_status_t status;

	status = psa_ps_get(SLM_TLS_TBL_UUID, 0, SLM_TLS_TBL_MAX_SIZE, buf, len);

	if (status != PSA_SUCCESS) {
		LOG_ERR("Could not get credential table. Error %d", status);
	}
	return translate_status(status);
}
static int slm_tls_tbl_set(sec_tag_t sec_tag, uint16_t type, bool set)
{
	psa_status_t status;
	struct psa_storage_info_t info;
	psa_storage_create_flags_t flags = 0;
	size_t len = SLM_TLS_TBL_MAX_SIZE;

	if(type >=SLM_TLS_PSA_CREDENTIAL_TYPE_MAX) {
		return -ENOTSUP;
	}
	crdl_tbl = k_malloc(SLM_TLS_TBL_MAX_SIZE);
	if (crdl_tbl == NULL) {
		LOG_ERR("Fail to allocate slm cred table memory");
		return -ENOMEM;
	}
	memset(crdl_tbl, 0, SLM_TLS_TBL_MAX_SIZE);

	status = psa_ps_get_info(SLM_TLS_TBL_UUID, &info);

	if(status == PSA_SUCCESS) {
		status = slm_tls_tbl_get(crdl_tbl, &len);
	}
	status = psa_ps_remove(SLM_TLS_TBL_UUID);
	SLM_TLS_TBL_MAP(crdl_tbl, sec_tag, type, set);
	status = psa_ps_set(SLM_TLS_TBL_UUID, SLM_TLS_TBL_MAX_SIZE, crdl_tbl, flags);
	k_free(crdl_tbl);
	if(status != PSA_SUCCESS) {
		LOG_ERR("final set cred table status %d\n", status);
	}
	return translate_status(status);
}
int slm_tls_ps_set(sec_tag_t sec_tag, uint16_t type, const void *buf,
		   size_t len)
{
	psa_status_t status;
	psa_storage_uid_t uid;
	psa_storage_create_flags_t flags = 0;

	uid = SLM_TLS_PS_UID(type, sec_tag);
	if (IS_ENABLED(CONFIG_SLM_NATIVE_TLS_PS_WRITE_ONCE)) {
		flags |= PSA_STORAGE_FLAG_WRITE_ONCE;
	} else {
		status = psa_ps_remove(uid);
	}
	status = psa_ps_set(uid, len, buf, flags);

	if (status != PSA_SUCCESS) {
		LOG_ERR("Could not set credential. Error %d", status);
		(void)slm_tls_tbl_set(sec_tag, type, false);
	} else {
		status = slm_tls_tbl_set(sec_tag, type, true);
		if(status != PSA_SUCCESS) {
			status = psa_ps_remove(uid);
		}
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
	slm_tls_tbl_set(sec_tag, type, false);

	return translate_status(status);
}
