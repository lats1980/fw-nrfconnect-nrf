
#include <linux/module.h>

#include <rpu_fw_patches.h>
#include <fmac_api.h>
#include <linux_fmac_main.h>

#if defined(CONFIG_NRF700X_ON_USB_ADAPTER)
#include "nrf700x_usb.h"
#endif

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Example for nRF7002DK Wi-Fi over USB driver.");

#ifndef CONFIG_NRF700X_RADIO_TEST
struct wifi_nrf_drv_priv_linux rpu_drv_priv_linux;

#ifdef CONFIG_NRF700X_DATA_TX

#define MAX_RX_QUEUES 3

#define TOTAL_TX_SIZE (CONFIG_TX_MAX_DATA_SIZE + TX_BUF_HEADROOM)

BUILD_ASSERT(CONFIG_MAX_TX_TOKENS >= 1, "At least one TX token is required");
BUILD_ASSERT(CONFIG_MAX_TX_AGGREGATION <= 16, "Max TX aggregation is 16");
BUILD_ASSERT(CONFIG_RX_NUM_BUFS >= 1, "At least one RX buffer is required");

BUILD_ASSERT(RPU_PKTRAM_SIZE >=
		((CONFIG_MAX_TX_AGGREGATION * CONFIG_MAX_TX_TOKENS * TOTAL_TX_SIZE) +
		(CONFIG_RX_NUM_BUFS * CONFIG_RX_MAX_DATA_SIZE)),
		"Packet RAM overflow in Sheliak");

static const unsigned char aggregation = 1;
static const unsigned char wmm = 1;
static const unsigned char max_num_tx_agg_sessions = 4;
static const unsigned char max_num_rx_agg_sessions = 2;
static const unsigned char reorder_buf_size = 64;
static const unsigned char max_rxampdu_size = MAX_RX_AMPDU_SIZE_64KB;

static const unsigned char max_tx_aggregation = CONFIG_MAX_TX_AGGREGATION;

static const unsigned int rx1_num_bufs = CONFIG_RX_NUM_BUFS / MAX_RX_QUEUES;
static const unsigned int rx2_num_bufs = CONFIG_RX_NUM_BUFS / MAX_RX_QUEUES;
static const unsigned int rx3_num_bufs = CONFIG_RX_NUM_BUFS / MAX_RX_QUEUES;

static const unsigned int rx1_buf_sz = CONFIG_RX_MAX_DATA_SIZE;
static const unsigned int rx2_buf_sz = CONFIG_RX_MAX_DATA_SIZE;
static const unsigned int rx3_buf_sz = CONFIG_RX_MAX_DATA_SIZE;

static const unsigned char rate_protection_type;
#else
/* Reduce buffers to Scan only operation */
static const unsigned int rx1_num_bufs = 2;
static const unsigned int rx2_num_bufs = 2;
static const unsigned int rx3_num_bufs = 2;

static const unsigned int rx1_buf_sz = 1000;
static const unsigned int rx2_buf_sz = 1000;
static const unsigned int rx3_buf_sz = 1000;
#endif

struct wifi_nrf_drv_priv_linux rpu_drv_priv_linux;

/* TODO add missing code */
#endif /* !CONFIG_NRF700X_RADIO_TEST */

static void nrf700x_scan_routine(struct work_struct *w) {
	enum wifi_nrf_status status = WIFI_NRF_STATUS_FAIL;
	struct nrf_wifi_umac_scan_info scan_info;

    struct nrf700x_adapter *nrf700x = container_of(w, struct nrf700x_adapter, ws_scan);

    if(down_interruptible(&nrf700x->sem)) {
        return;
    }

	printk("scan routine\n");
	memset(&scan_info, 0, sizeof(scan_info));
	scan_info.scan_mode = AUTO_SCAN;
	scan_info.scan_reason = SCAN_DISPLAY;
	/* Wildcard SSID to trigger active scan */
	scan_info.scan_params.num_scan_ssids = 1;
	scan_info.scan_params.scan_ssids[0].nrf_wifi_ssid_len = 0;
	scan_info.scan_params.scan_ssids[0].nrf_wifi_ssid[0] = 0;
	status = wifi_nrf_fmac_scan(rpu_drv_priv_linux.rpu_ctx_linux.rpu_ctx, nrf700x->vif_idx, &scan_info);
	if (status != WIFI_NRF_STATUS_SUCCESS) {
		printk("%s: wifi_nrf_fmac_scan failed\n", __func__);
	}
}

enum wifi_nrf_status wifi_nrf_fmac_dev_add_linux(void)
{
	enum wifi_nrf_status status = WIFI_NRF_STATUS_FAIL;
	struct wifi_nrf_ctx_linux *rpu_ctx_linux = NULL;
	struct wifi_nrf_fmac_fw_info fw_info;
	void *rpu_ctx = NULL;
	
	struct nrf_wifi_umac_add_vif_info add_vif_info;
	struct nrf_wifi_umac_chg_vif_state_info chg_vif_info;
	unsigned char vif_idx;
	uint8_t addr[6];//for test
	struct nrf700x_adapter *vif_ctx;
#ifdef CONFIG_NRF_WIFI_LOW_POWER
	int sleep_type = -1;

#ifndef CONFIG_NRF700X_RADIO_TEST
	sleep_type = HW_SLEEP_ENABLE;
#else
	sleep_type = SLEEP_DISABLE;
#endif /* CONFIG_NRF700X_RADIO_TEST */
#endif /* CONFIG_NRF_WIFI_LOW_POWER */

	rpu_ctx_linux = &rpu_drv_priv_linux.rpu_ctx_linux;
	rpu_ctx_linux->drv_priv_linux = &rpu_drv_priv_linux;
	rpu_ctx = wifi_nrf_fmac_dev_add(rpu_drv_priv_linux.fmac_priv, rpu_ctx_linux);
	if (!rpu_ctx) {
		printk("%s: wifi_nrf_fmac_dev_add failed\n", __func__);
		rpu_ctx_linux = NULL;
		goto out;
	}

	rpu_ctx_linux->rpu_ctx = rpu_ctx;

	/* Load the FW patches to the RPU */
	memset(&fw_info, 0, sizeof(fw_info));
	fw_info.lmac_patch_pri.data = wifi_nrf_lmac_patch_pri_bimg;
	fw_info.lmac_patch_pri.size = sizeof(wifi_nrf_lmac_patch_pri_bimg);
	fw_info.lmac_patch_sec.data = wifi_nrf_lmac_patch_sec_bin;
	fw_info.lmac_patch_sec.size = sizeof(wifi_nrf_lmac_patch_sec_bin);
	fw_info.umac_patch_pri.data = wifi_nrf_umac_patch_pri_bimg;
	fw_info.umac_patch_pri.size = sizeof(wifi_nrf_umac_patch_pri_bimg);
	fw_info.umac_patch_sec.data = wifi_nrf_umac_patch_sec_bin;
	fw_info.umac_patch_sec.size = sizeof(wifi_nrf_umac_patch_sec_bin);
	status = wifi_nrf_fmac_fw_load(rpu_ctx, &fw_info);
	if (status != WIFI_NRF_STATUS_SUCCESS) {
		printk("%s: wifi_nrf_fmac_fw_load failed\n", __func__);
		goto out;
	}

	status = wifi_nrf_fmac_dev_init(rpu_ctx,
#ifndef CONFIG_NRF700X_RADIO_TEST
					NULL,
#endif /* !CONFIG_NRF700X_RADIO_TEST */
#ifdef CONFIG_NRF_WIFI_LOW_POWER
					sleep_type,
#endif /* CONFIG_NRF_WIFI_LOW_POWER */
					NRF_WIFI_DEF_PHY_CALIB);
	if (status != WIFI_NRF_STATUS_SUCCESS) {
		printk("%s: wifi_nrf_fmac_dev_init failed\n", __func__);
		goto out;
	}


	memset(&add_vif_info, 0, sizeof(add_vif_info));
	add_vif_info.iftype = NRF_WIFI_IFTYPE_STATION;
	memcpy(add_vif_info.ifacename, "wlan0", strlen("wlan0"));
	/* Currently only 1 VIF is supported */
	vif_idx = wifi_nrf_fmac_add_vif(rpu_ctx,
							&rpu_ctx_linux->vif_ctx_linux,
							&add_vif_info);
	if (vif_idx >= MAX_NUM_VIFS) {
		printk("%s: FMAC returned invalid interface index\n", __func__);
		goto out;
	}
	printk("vif:%u\n", vif_idx);
	vif_ctx = &rpu_ctx_linux->vif_ctx_linux;

	status = wifi_nrf_fmac_otp_mac_addr_get(rpu_ctx,
						vif_idx,
						addr);
	if (status != WIFI_NRF_STATUS_SUCCESS) {
		printk("%s: Fetching of MAC address from OTP failed\n",
			__func__);
		goto out;
	}
	printk("mac addr: %x %x %x %x %x %x \n", addr[0], addr[1], addr[2], addr[3], addr[4],addr[5]);
	status = wifi_nrf_fmac_set_vif_macaddr(rpu_ctx, vif_idx, addr);
	if (status != WIFI_NRF_STATUS_SUCCESS) {
		printk("%s: MAC address change failed\n",
			__func__);
		goto out;
	}
	msleep(50);
	memset(&chg_vif_info, 0, sizeof(chg_vif_info));
	chg_vif_info.state = WIFI_NRF_FMAC_IF_OP_STATE_UP;
	memcpy(chg_vif_info.ifacename, "wlan0", strlen("wlan0"));
	status = wifi_nrf_fmac_chg_vif_state(rpu_ctx, vif_idx, &chg_vif_info);
	if (status != WIFI_NRF_STATUS_SUCCESS) {
		printk("%s: wifi_nrf_fmac_chg_vif_state failed\n",
			__func__);
		goto out;
	}

	msleep(100);

	status = wifi_nrf_fmac_get_wiphy(rpu_ctx, vif_idx);
	if (status != WIFI_NRF_STATUS_SUCCESS) {
		printk("%s: nrf_wifi_fmac_get_wiphy failed\n", __func__);
	}

	vif_ctx->vif_idx = vif_idx;
	vif_ctx->scan_request = NULL;
	INIT_WORK(&vif_ctx->ws_scan, nrf700x_scan_routine);
        sema_init(&vif_ctx->sem, 1);

out:
    return status;
}

void wifi_nrf_fmac_dev_rem_linux(struct wifi_nrf_drv_priv_linux *drv_priv_linux)
{
	struct wifi_nrf_ctx_linux *rpu_ctx_linux = NULL;

	rpu_ctx_linux = &drv_priv_linux->rpu_ctx_linux;
	wifi_nrf_fmac_dev_rem(rpu_ctx_linux->rpu_ctx);
}

static int __init nrf_wifi_init(void) {
	int ret;
#ifndef CONFIG_NRF700X_RADIO_TEST
	struct wifi_nrf_fmac_callbk_fns callbk_fns = { 0 };
	struct nrf_wifi_data_config_params data_config = { 0 };
	struct rx_buf_pool_params rx_buf_pools[MAX_NUM_OF_RX_QUEUES];
	//struct wifi_nrf_vif_ctx_zep * = dev->data;

	//vif_ctx_zep->rpu_ctx_zep = &rpu_drv_priv_linux.rpu_ctx_zep;
#ifdef CONFIG_NRF700X_DATA_TX
	data_config.aggregation = aggregation;
	data_config.wmm = wmm;
	data_config.max_num_tx_agg_sessions = max_num_tx_agg_sessions;
	data_config.max_num_rx_agg_sessions = max_num_rx_agg_sessions;
	data_config.max_tx_aggregation = max_tx_aggregation;
	data_config.reorder_buf_size = reorder_buf_size;
	data_config.max_rxampdu_size = max_rxampdu_size;
	data_config.rate_protection_type = rate_protection_type;

	callbk_fns.if_carr_state_chg_callbk_fn = wifi_nrf_if_carr_state_chg;
	callbk_fns.rx_frm_callbk_fn = wifi_nrf_if_rx_frm;
#endif
	rx_buf_pools[0].num_bufs = rx1_num_bufs;
	rx_buf_pools[1].num_bufs = rx2_num_bufs;
	rx_buf_pools[2].num_bufs = rx3_num_bufs;
	rx_buf_pools[0].buf_sz = rx1_buf_sz;
	rx_buf_pools[1].buf_sz = rx2_buf_sz;
	rx_buf_pools[2].buf_sz = rx3_buf_sz;

	callbk_fns.scan_start_callbk_fn = wifi_nrf_event_proc_scan_start_linux;
	callbk_fns.scan_done_callbk_fn = wifi_nrf_event_proc_scan_done_linux;
	callbk_fns.disp_scan_res_callbk_fn = wifi_nrf_event_proc_disp_scan_res_linux;
	//callbk_fns.twt_config_callbk_fn = wifi_nrf_event_proc_twt_setup_zep;
	//callbk_fns.twt_teardown_callbk_fn = wifi_nrf_event_proc_twt_teardown_zep;
	//callbk_fns.twt_sleep_callbk_fn = wifi_nrf_event_proc_twt_sleep_zep;
	//callbk_fns.event_get_reg = wifi_nrf_event_get_reg_zep;
#ifdef CONFIG_WPA_SUPP
	//callbk_fns.scan_res_callbk_fn = wifi_nrf_wpa_supp_event_proc_scan_res;
	//callbk_fns.auth_resp_callbk_fn = wifi_nrf_wpa_supp_event_proc_auth_resp;
	//callbk_fns.assoc_resp_callbk_fn = wifi_nrf_wpa_supp_event_proc_assoc_resp;
	//callbk_fns.deauth_callbk_fn = wifi_nrf_wpa_supp_event_proc_deauth;
	//callbk_fns.disassoc_callbk_fn = wifi_nrf_wpa_supp_event_proc_disassoc;
	//callbk_fns.get_station_callbk_fn = wifi_nrf_wpa_supp_event_proc_get_sta;
	//callbk_fns.get_interface_callbk_fn = wifi_nrf_wpa_supp_event_proc_get_if;
	//callbk_fns.mgmt_tx_status = wifi_nrf_wpa_supp_event_mgmt_tx_status;
	//callbk_fns.unprot_mlme_mgmt_rx_callbk_fn = wifi_nrf_wpa_supp_event_proc_unprot_mgmt;
	callbk_fns.event_get_wiphy = wifi_nrf_wpa_supp_event_get_wiphy;
	//callbk_fns.mgmt_rx_callbk_fn = wifi_nrf_wpa_supp_event_mgmt_rx_callbk_fn;
#endif /* CONFIG_WPA_SUPP */
	rpu_drv_priv_linux.fmac_priv = wifi_nrf_fmac_init(&data_config,
							rx_buf_pools,
							&callbk_fns);
#else /* !CONFIG_NRF700X_RADIO_TEST */
	rpu_drv_priv_linux.fmac_priv = wifi_nrf_fmac_init();
#endif /* CONFIG_NRF700X_RADIO_TEST */

	if (rpu_drv_priv_linux.fmac_priv == NULL) {
		printk("%s: wifi_nrf_fmac_init failed\n",
			__func__);
		goto err;
	}

#if defined(CONFIG_NRF700X_ON_USB_ADAPTER)
	ret = nrf700x_usb_init();
	if (!ret)
		printk(KERN_ERR "%s: usb init ok\n", __func__);
	else {
		printk(KERN_ERR "%s: usb init fail\n", __func__);
		goto err;
	}
	return 0;
#endif
	
err:
	return -1;
}

static void __exit nrf_wifi_exit(void) {
	struct nrf700x_adapter *vif_ctx;
	vif_ctx = &rpu_drv_priv_linux.rpu_ctx_linux.vif_ctx_linux;
    cancel_work_sync(&vif_ctx->ws_scan);
	nrf700x_uninit(vif_ctx);
#if defined(CONFIG_NRF700X_ON_USB_ADAPTER)
	nrf700x_usb_exit();
#endif
	wifi_nrf_fmac_deinit(rpu_drv_priv_linux.fmac_priv);
}

module_init(nrf_wifi_init);
module_exit(nrf_wifi_exit);