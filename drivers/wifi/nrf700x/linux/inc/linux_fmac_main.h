/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @brief Header containing FMAC interface specific declarations for the
 * Linux OS layer of the Wi-Fi driver.
 */

#ifndef __LINUX_FMAC_MAIN_H__
#define __LINUX_FMAC_MAIN_H__

#include "cfg80211.h"

struct wifi_nrf_ctx_linux {
	struct wifi_nrf_drv_priv_linux *drv_priv_linux;
	void *rpu_ctx;

#ifdef CONFIG_NRF700X_RADIO_TEST
	struct rpu_conf_params conf_params;
	bool rf_test_run;
	unsigned char rf_test;
#else /* CONFIG_NRF700X_RADIO_TEST */
	struct nrf700x_adapter *vif_ctx_linux[MAX_NUM_VIFS];
#ifdef CONFIG_NRF700X_WIFI_UTIL
	struct rpu_conf_params conf_params;
#endif /* CONFIG_NRF700X_WIFI_UTIL */
#endif /* CONFIG_NRF700X_RADIO_TEST */
};

struct wifi_nrf_drv_priv_linux {
	struct wifi_nrf_fmac_priv *fmac_priv;
	spinlock_t evt_q_lock;
	struct list_head fmac_event_q;
	struct work_struct ws_event;
	/* TODO: Replace with a linked list to handle unlimited RPUs */
	struct wifi_nrf_ctx_linux rpu_ctx_linux;
};

enum wifi_nrf_status wifi_nrf_fmac_dev_add_linux(struct device *dev);
void wifi_nrf_fmac_dev_rem_linux(struct wifi_nrf_drv_priv_linux *drv_priv_linux);

#endif /* __LINUX_FMAC_MAIN_H__ */
