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

#include "fmac_event.h"
#include "cfg80211.h"

struct wifi_nrf_rpu_priv_lnx {
	struct wifi_nrf_drv_priv_lnx *drv_priv;
	void *rpu_ctx;
	struct wiphy *wiphy;
	struct nrf_vif_priv vif_priv[MAX_NUM_VIFS];
};

struct wifi_nrf_drv_priv_lnx {
	struct wifi_nrf_fmac_priv *fmac_priv;
	spinlock_t evt_q_lock;
	struct list_head fmac_event_q;
	struct work_struct ws_event;
};

struct wifi_nrf_rpu_priv_lnx *wifi_nrf_fmac_dev_add_lnx(struct device *dev);
void wifi_nrf_fmac_dev_rem_lnx(struct wifi_nrf_rpu_priv_lnx *drv_priv);

#endif /* __LINUX_FMAC_MAIN_H__ */
