#include "fmac_event.h"

#include <linux_fmac_main.h>

#include "shim.h"
#include "queue.h"

/* helper function that will retrieve main context from "priv" data of the network device */
static struct ndev_priv_context *
ndev_get_priv(struct net_device *ndev) { return (struct ndev_priv_context *) netdev_priv(ndev); }

static void nrf_cfg80211_scan_routine(struct work_struct *w) {
	enum wifi_nrf_status status = WIFI_NRF_STATUS_FAIL;
	struct nrf_wifi_umac_scan_info scan_info;
	struct nrf_vif_priv *nrf_vif = container_of(w, struct nrf_vif_priv, ws_scan);
	struct wifi_nrf_rpu_priv_lnx *rpu_priv = nrf_vif->rpu_priv;

	memset(&scan_info, 0, sizeof(scan_info));

	scan_info.scan_mode = AUTO_SCAN;
	if (nrf_vif->vif_status == NRF_VIF_CONNECTING) {
		scan_info.scan_reason = SCAN_CONNECT;
		memcpy(scan_info.scan_params.scan_ssids[0].nrf_wifi_ssid,
			   nrf_vif->auth_info.ssid.nrf_wifi_ssid,
			   nrf_vif->auth_info.ssid.nrf_wifi_ssid_len);
		scan_info.scan_params.scan_ssids[0].nrf_wifi_ssid_len =
			nrf_vif->auth_info.ssid.nrf_wifi_ssid_len;
	} else if (nrf_vif->vif_status == NRF_VIF_IDLE) {
		scan_info.scan_reason = SCAN_DISPLAY;
		scan_info.scan_params.scan_ssids[0].nrf_wifi_ssid_len = 0;
		scan_info.scan_params.scan_ssids[0].nrf_wifi_ssid[0] = 0;
	}

	/* Wildcard SSID to trigger active scan */
	scan_info.scan_params.num_scan_ssids = 1;
	if (nrf_vif->scan_request != NULL) {
		printk("%s n_ssids: %d n_channels: %u\n", __func__, nrf_vif->scan_request->n_ssids, nrf_vif->scan_request->n_channels);
		printk("%s duration: %u ie_len: %zu\n", __func__, nrf_vif->scan_request->duration, nrf_vif->scan_request->ie_len);
		if (nrf_vif->scan_request->n_ssids > 0) {
			int i;
			for (i = 0; i < nrf_vif->scan_request->n_ssids; i++) {
				printk("%s ssid len: %u\n", __func__, nrf_vif->scan_request->ssids[i].ssid_len);
			}
		}
		/* Copy extra_ies */
		if (nrf_vif->scan_request->ie_len > 0) {
			printk("Copy extra_ies\n");
			if (nrf_vif->scan_request->ie_len > NRF_WIFI_MAX_IE_LEN) {
				printk("Fail to copy extra_ies\n");
			}
			memcpy(scan_info.scan_params.ie.ie, nrf_vif->scan_request->ie, nrf_vif->scan_request->ie_len);
			scan_info.scan_params.ie.ie_len = nrf_vif->scan_request->ie_len;
		}
	}

	status = wifi_nrf_fmac_scan(rpu_priv->rpu_ctx, nrf_vif->vif_idx, &scan_info);
	if (status != WIFI_NRF_STATUS_SUCCESS) {
		printk("%s: wifi_nrf_fmac_scan failed\n", __func__);
	}
}

static void nrf_cfg80211_connected_routine(struct work_struct *w) {
	enum wifi_nrf_status status = WIFI_NRF_STATUS_FAIL;
	struct nrf_wifi_umac_chg_sta_info chg_sta_info;
	struct nrf_vif_priv *nrf_vif = container_of(w, struct nrf_vif_priv, ws_connected);
	struct wifi_nrf_rpu_priv_lnx *rpu_priv = nrf_vif->rpu_priv;

	memset(&chg_sta_info, 0, sizeof(chg_sta_info));
	memcpy(chg_sta_info.mac_addr, nrf_vif->assoc_info.nrf_wifi_bssid, ETH_ALEN);

	printk("change station to authorized\n");
	/* BIT(NL80211_STA_FLAG_AUTHORIZED) */
	chg_sta_info.sta_flags2.nrf_wifi_mask = 1 << 1;
	/* BIT(NL80211_STA_FLAG_AUTHORIZED) */
	chg_sta_info.sta_flags2.nrf_wifi_set = 1 << 1;
	status = wifi_nrf_fmac_chg_sta(rpu_priv->rpu_ctx, nrf_vif->vif_idx, &chg_sta_info);
	if (status != WIFI_NRF_STATUS_SUCCESS) {
		printk("%s: wifi_nrf_fmac_chg_sta failed\n", __func__);
	}
}

#ifdef CONFIG_NRF700X_DATA_TX
static void nrf_cfg80211_data_tx_routine(struct work_struct *w) {
	struct nrf_vif_priv *nrf_vif = container_of(w, struct nrf_vif_priv, ws_data_tx);
	struct wifi_nrf_rpu_priv_lnx *rpu_priv = nrf_vif->rpu_priv;
	enum wifi_nrf_status status = WIFI_NRF_STATUS_FAIL;
	void *netbuf = NULL;

	netbuf = wifi_nrf_utils_q_dequeue(rpu_priv->drv_priv->fmac_priv->opriv,
						nrf_vif->data_txq);

	if (netbuf == NULL) {
		printk("%s: fail to get tx data from queue\n", __func__);
		return;
	}
	status = wifi_nrf_fmac_start_xmit(rpu_priv->rpu_ctx,
					nrf_vif->vif_idx,
					netbuf);
	if (status != WIFI_NRF_STATUS_SUCCESS) {
		printk("%s: wifi_nrf_fmac_start_xmit failed\n", __func__);
	}
	if (wifi_nrf_utils_q_len(rpu_priv->drv_priv->fmac_priv->opriv,
						nrf_vif->data_txq) > 0) {
		schedule_work(&nrf_vif->ws_data_tx);
	}
}
#endif

static int nrf_cfg80211_scan(struct wiphy *wiphy, struct cfg80211_scan_request *request)
{
	struct ndev_priv_context *ndev_data = NULL;
	struct nrf_vif_priv *nrf_vif;

	ndev_data = netdev_priv(request->wdev->netdev);
	nrf_vif = ndev_data->nrf_vif;
	if (nrf_vif->vif_status != NRF_VIF_IDLE) {
		return -EBUSY;
	}

	if (nrf_vif->scan_request != NULL) {
		return -EBUSY;
	}
	nrf_vif->scan_request = request;
	printk("%s n_ssids: %d n_channels: %u\n", __func__, request->n_ssids, request->n_channels);
	printk("%s duration: %u ie_len: %zu\n", __func__, request->duration, request->ie_len);
	if (request->n_ssids > 0) {
		int i;
		for (i = 0; i < request->n_ssids; i++) {
			printk("ssid len: %u\n", request->ssids[i].ssid_len);
		}
	}
	if (!schedule_work(&nrf_vif->ws_scan)) {
		return -EBUSY;
	}

	return 0;
}

static int nrf_cfg80211_connect(struct wiphy *wiphy, struct net_device *dev,
			 struct cfg80211_connect_params *sme)
{
	struct nrf_vif_priv *nrf_vif;
	struct ndev_priv_context *ndev_data = NULL;

	ndev_data = ndev_get_priv(dev);
	nrf_vif = ndev_data->nrf_vif;

	if (ndev_data->wdev.iftype != NL80211_IFTYPE_STATION)
		return -EOPNOTSUPP;

	if (nrf_vif->scan_request) {
		return -EBUSY;
	}

	if (nrf_vif->vif_status != NRF_VIF_IDLE) {
		return -EBUSY;
	}

	memset(&nrf_vif->auth_info, 0, sizeof(nrf_vif->auth_info));
	memset(&nrf_vif->assoc_info, 0, sizeof(nrf_vif->assoc_info));
	if (sme->ssid) {
		//printk("%s: set ssid len: %u\n", __func__, sme->ssid_len);
		//printk("%s: sme ssid: %s\n", __func__, sme->ssid);
		memcpy(nrf_vif->auth_info.ssid.nrf_wifi_ssid, sme->ssid, sme->ssid_len);
		nrf_vif->auth_info.ssid.nrf_wifi_ssid_len = sme->ssid_len;
		memcpy(nrf_vif->assoc_info.ssid.nrf_wifi_ssid, sme->ssid, sme->ssid_len);
		nrf_vif->assoc_info.ssid.nrf_wifi_ssid_len = sme->ssid_len;
	} else {
		printk("%s: sme without ssid\n", __func__);
		return -EINVAL;
	}
	if (sme->bssid) {
		memcpy(nrf_vif->auth_info.nrf_wifi_bssid, sme->bssid, ETH_ALEN);
		memcpy(nrf_vif->assoc_info.nrf_wifi_bssid, sme->bssid, ETH_ALEN);
	}

	nrf_vif->vif_status = NRF_VIF_CONNECTING;
	/* Currently only support open system */
	nrf_vif->auth_info.auth_type = NRF_WIFI_AUTHTYPE_OPEN_SYSTEM;
	nrf_vif->auth_info.nrf_wifi_flags |= NRF_WIFI_CMD_AUTHENTICATE_LOCAL_STATE_CHANGE;
	nrf_vif->assoc_info.control_port = 1;
	if (sme->mfp == NL80211_MFP_NO)
		nrf_vif->assoc_info.use_mfp = NRF_WIFI_MFP_NO;
	else if (sme->mfp == NL80211_MFP_REQUIRED || sme->mfp == NL80211_MFP_OPTIONAL)
		nrf_vif->assoc_info.use_mfp = NRF_WIFI_MFP_REQUIRED;
	else {
		return -EOPNOTSUPP;
	}

	if (!schedule_work(&nrf_vif->ws_scan)) {
		return -EBUSY;
	}
	return 0;
}

static int nrf_cfg80211_disconnect(struct wiphy *wiphy, struct net_device *dev,
			  u16 reason_code)
{
	enum wifi_nrf_status status = WIFI_NRF_STATUS_FAIL;
	struct ndev_priv_context *ndev_data = NULL;
	struct nrf_vif_priv *nrf_vif = NULL;
	struct nrf_wifi_umac_disconn_info deauth_info;
	struct wifi_nrf_rpu_priv_lnx *rpu_priv = nrf_vif->rpu_priv;

	printk("%s: %u", __func__, reason_code);

	ndev_data = ndev_get_priv(dev);
	nrf_vif = ndev_data->nrf_vif;

	memset(&deauth_info, 0, sizeof(deauth_info));
	deauth_info.reason_code = reason_code;
	//memcpy(deauth_info.mac_addr, addr, sizeof(deauth_info.mac_addr));
	status = wifi_nrf_fmac_deauth(rpu_priv->rpu_ctx, nrf_vif->vif_idx, &deauth_info);
	if (status != WIFI_NRF_STATUS_SUCCESS) {
		printk("%s: wifi_nrf_fmac_deauth failed\n", __func__);
		return -EINVAL;
	} else {
		printk("%s:dis-authentication request sent successfully\n", __func__);
	}

	return 0;
}

static struct cfg80211_ops nrf_cfg80211_ops = {
	.scan = nrf_cfg80211_scan,
	.connect = nrf_cfg80211_connect,
	.disconnect = nrf_cfg80211_disconnect,
};

/* Network packet transmit.
 * Callback that called by the kernel when packet of data should be sent.
 * In this example it does nothing. */
static netdev_tx_t nrf_ndo_start_xmit(struct sk_buff *skb, struct net_device *dev) {
	/* Dont forget to cleanup skb, as its ownership moved to xmit callback. */
#ifdef CONFIG_NRF700X_DATA_TX
	netdev_tx_t ret;
	struct ndev_priv_context *ndev_data = NULL;
	struct wifi_nrf_rpu_priv_lnx *rpu_priv;
	enum wifi_nrf_status status = WIFI_NRF_STATUS_FAIL;
	void *netbuf = NULL;

	printk("%s: tx: %u\n", __func__, skb->len);

	if (skb->dev != dev) {
		printk("%s: wrong net dev\n", __func__);
		goto out;
	}

	ndev_data = ndev_get_priv(dev);
	rpu_priv = ndev_data->nrf_vif->rpu_priv;
/*
	if ((vif_ctx_zep->if_carr_state != WIFI_NRF_FMAC_IF_CARR_STATE_ON) ||
		(!vif_ctx_zep->authorized && !is_eapol(pkt))) {
		goto out;
	}
*/

	/* TODO: check data_txq len for flow control */
	netbuf = net_pkt_to_nbuf(skb);
	if (netbuf == NULL) {
		ret = NET_XMIT_DROP;
		goto out;
	}

	status = wifi_nrf_utils_q_enqueue(rpu_priv->drv_priv->fmac_priv->opriv,
						ndev_data->nrf_vif->data_txq,
						netbuf);
	if (status != WIFI_NRF_STATUS_SUCCESS) {
		printk("%s: wifi_nrf_fmac_start_xmit failed\n", __func__);
		if (netbuf != NULL) {
			kfree(netbuf);
		}
		ret = NETDEV_TX_BUSY;
		return ret;
	}
	schedule_work(&ndev_data->nrf_vif->ws_data_tx);

	ret = NETDEV_TX_OK;
	goto out;
#else
	ret = NET_XMIT_DROP;
	goto out;
#endif /* CONFIG_NRF700X_DATA_TX */
out:
	kfree_skb(skb);

	return ret;
}

/* Structure of functions for network devices.
 * It should have at least ndo_start_xmit functions that called for packet to be sent. */
static struct net_device_ops nvf_ndev_ops = {
		.ndo_start_xmit = nrf_ndo_start_xmit,
};

static struct ieee80211_channel nrf_cfg80211_supported_channels_2ghz[14];
static struct ieee80211_rate nrf_cfg80211_supported_rates_2ghz[12];

static struct ieee80211_supported_band nrf_cfg80211_band_2ghz = {
	.band = NL80211_BAND_2GHZ,
	.channels = nrf_cfg80211_supported_channels_2ghz,
	.n_channels = ARRAY_SIZE(nrf_cfg80211_supported_channels_2ghz),
	.bitrates = nrf_cfg80211_supported_rates_2ghz,
	.n_bitrates = ARRAY_SIZE(nrf_cfg80211_supported_rates_2ghz),
};

static struct ieee80211_channel nrf_cfg80211_supported_channels_5ghz[28];
static struct ieee80211_rate nrf_cfg80211_supported_rates_5ghz[8];

static struct ieee80211_supported_band nrf_cfg80211_band_5ghz = {
	.band = NL80211_BAND_5GHZ,
	.channels = nrf_cfg80211_supported_channels_5ghz,
	.n_channels = ARRAY_SIZE(nrf_cfg80211_supported_channels_5ghz),
	.bitrates = nrf_cfg80211_supported_rates_5ghz,
	.n_bitrates = ARRAY_SIZE(nrf_cfg80211_supported_rates_5ghz),
};

int nrf_cfg80211_setup_bands(struct ieee80211_supported_band * bands)
{
	return -1;
}

struct wifi_nrf_rpu_priv_lnx *nrf_cfg80211_init(struct device *dev)
{
	struct wiphy *wiphy;
	struct wifi_nrf_rpu_priv_lnx *rpu_priv;

	wiphy = wiphy_new_nm(&nrf_cfg80211_ops, sizeof(struct wifi_nrf_rpu_priv_lnx), WIPHY_NAME);
	if (wiphy == NULL) {
		printk("%s: fail to allocate new wiphy\n", __func__);
		return NULL;
	}
	rpu_priv = wiphy_priv(wiphy);

	/* Currently only supports station mode */
	wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);

	if(dev) {
		set_wiphy_dev(wiphy, dev);
	}
	rpu_priv->wiphy = wiphy;

	return rpu_priv;
}

void nrf_cfg80211_uninit(struct wifi_nrf_rpu_priv_lnx *rpu_priv) {
	int i;

	if (rpu_priv == NULL) {
		return;
	}
#ifdef CONFIG_NRF700X_DATA_TX
	/* Free the data_txq */
	/*
	if (ctx->data_txq != NULL) {
		wifi_nrf_utils_q_free(ctx->rpu_priv->drv_priv->fmac_priv->opriv, ctx->data_txq);
	}
	*/
#endif
	for (i = 0; i < MAX_NUM_VIFS; i++) {
		if (rpu_priv->vif_priv[i].ndev == NULL) {
			//printk("net device of vif %u is not alocated\n", i);
			continue;
		}
		if (rpu_priv->vif_priv[i].scan_request) {
			struct cfg80211_scan_info info = {
				.aborted = true,
			};
			//printk("vif %u abort scanning\n", i);
			cfg80211_scan_done(rpu_priv->vif_priv[i].scan_request, &info);
			rpu_priv->vif_priv[i].scan_request = NULL;
		}
		if (rpu_priv->vif_priv[i].ndev->reg_state == NETREG_REGISTERED) {
			unregister_netdev(rpu_priv->vif_priv[i].ndev);
			free_netdev(rpu_priv->vif_priv[i].ndev);
		}
	}
	if (rpu_priv->wiphy->registered)
		wiphy_unregister(rpu_priv->wiphy);
	wiphy_free(rpu_priv->wiphy);
}

void event_proc_scan_start(void *os_vif_ctx,
					struct nrf_wifi_umac_event_trigger_scan *scan_start_event,
					unsigned int event_len)
{
	printk("%s: %u\n", __func__, scan_start_event->valid_fields);
}

void event_proc_scan_done(void *os_vif_ctx,
			struct nrf_wifi_umac_event_trigger_scan *scan_done_event,
			unsigned int event_len)
{
	enum wifi_nrf_status status = WIFI_NRF_STATUS_FAIL;
	struct nrf_vif_priv *nrf_vif = (struct nrf_vif_priv *)os_vif_ctx;
	struct wifi_nrf_rpu_priv_lnx *rpu_priv = nrf_vif->rpu_priv;

	printk("%s: %u\n", __func__, scan_done_event->valid_fields);

	if (nrf_vif->vif_status == NRF_VIF_CONNECTING) {
		status = wifi_nrf_fmac_scan_res_get(rpu_priv->rpu_ctx,
							nrf_vif->vif_idx,
							SCAN_CONNECT);
		if (status != WIFI_NRF_STATUS_SUCCESS) {
			printk("%s: wifi_nrf_fmac_scan failed\n", __func__);
		}
	}
	else {
		status = wifi_nrf_fmac_scan_res_get(rpu_priv->rpu_ctx,
							nrf_vif->vif_idx,
							SCAN_DISPLAY);
		if (status != WIFI_NRF_STATUS_SUCCESS) {
			printk("%s: wifi_nrf_fmac_scan failed\n", __func__);
		}
	}
}

void event_proc_disp_scan_res(void *vif_ctx,
				struct nrf_wifi_umac_event_new_scan_display_results *scan_res,
				unsigned int event_len,
				bool more_res)
{
	struct nrf_vif_priv *vif_priv = NULL;
	struct umac_display_results *r = NULL;
	unsigned int i = 0;
	struct cfg80211_scan_info info;

	vif_priv = (struct nrf_vif_priv *)vif_ctx;
	for (i = 0; i < scan_res->event_bss_count; i++) {
		struct cfg80211_bss *bss = NULL;
		struct cfg80211_inform_bss bss_data;
		int ie_len;
		u8 ie[NRF_WIFI_MAX_SSID_LEN + 2];

		memset(ie, 0, sizeof(ie));
		r = &scan_res->display_results[i];
		ie[0] = WLAN_EID_SSID;
		ie[1] = r->ssid.nrf_wifi_ssid_len;

		if (r->ssid.nrf_wifi_ssid_len > NRF_WIFI_MAX_SSID_LEN) {
			printk("ssid len > buf size\n");
		}

		memcpy(ie + 2, r->ssid.nrf_wifi_ssid, r->ssid.nrf_wifi_ssid_len);
		ie_len = r->ssid.nrf_wifi_ssid_len + 2;
		//printk("ssid: %s\n", ie + 2);
		//printk("bssid: %pM\n", r->mac_addr);
		//printk("Band: %d\n", r->nwk_band);
		//printk("Channel: %u\n", r->nwk_channel);
		//printk("Capability: %u\n", r->capability);
		//printk("Beacon interval: %u\n", r->beacon_interval);
		bss_data.chan = ieee80211_get_channel(vif_priv->wiphy, ieee80211_channel_to_frequency(r->nwk_channel, r->nwk_band));
		bss_data.scan_width = NL80211_BSS_CHAN_WIDTH_20;
		bss_data.boottime_ns = ktime_get_boottime_ns();
		if (r->signal.signal_type == NRF_WIFI_SIGNAL_TYPE_MBM) {
			bss_data.signal = r->signal.signal.mbm_signal;
			//printk("Signal: %u\n", r->signal.signal.mbm_signal/100);
		} else if (r->signal.signal_type == NRF_WIFI_SIGNAL_TYPE_UNSPEC) {
			//res.rssi = (r->signal.signal.unspec_signal);
		}
		bss = cfg80211_inform_bss_data(vif_priv->wiphy, &bss_data, CFG80211_BSS_FTYPE_UNKNOWN, (const u8 *)r->mac_addr, 0, r->capability, r->beacon_interval,
									ie, sizeof(ie), GFP_KERNEL);
		if (bss) {
			cfg80211_put_bss(vif_priv->wiphy, bss);
		}
	}

	if (!more_res) {
		printk("finish scan\n");
		info.aborted = false;
		cfg80211_scan_done(vif_priv->scan_request, &info);
		vif_priv->scan_request = NULL;
	}
}

void event_proc_scan_res(void *os_vif_ctx,
					struct nrf_wifi_umac_event_new_scan_results *scan_res,
					unsigned int event_len,
					bool more_res)
{
	struct nrf_vif_priv *vif_ctx = NULL;

	vif_ctx = (struct nrf_vif_priv *)os_vif_ctx;
	printk("%s: valid_fields:%u\n", __func__, scan_res->valid_fields);
	printk("%s: freq:%u addr: %pM\n", __func__, scan_res->frequency, scan_res->mac_addr);
	vif_ctx->auth_info.frequency = scan_res->frequency;
	vif_ctx->assoc_info.center_frequency = scan_res->frequency;
	memcpy(vif_ctx->auth_info.nrf_wifi_bssid, scan_res->mac_addr, ETH_ALEN);
	memcpy(vif_ctx->assoc_info.nrf_wifi_bssid, scan_res->mac_addr, ETH_ALEN);
	if (!more_res) {
		printk("finish scan\n");
		if (vif_ctx->vif_status == NRF_VIF_CONNECTING) {
			struct wifi_nrf_rpu_priv_lnx *rpu_priv = vif_ctx->rpu_priv;
			enum wifi_nrf_status status = WIFI_NRF_STATUS_FAIL;

			struct cfg80211_bss *bss = NULL;
			struct cfg80211_inform_bss bss_data;
			const unsigned char *ie = NULL;
			unsigned int ie_len = 0;
			u64 tsf;

			if (scan_res->valid_fields & NRF_WIFI_EVENT_NEW_SCAN_RESULTS_IES_VALID && scan_res->ies) {
				ie = scan_res->ies;
				ie_len = scan_res->ies_len;
				bss_data.chan = ieee80211_get_channel(vif_ctx->wiphy, scan_res->frequency);
				bss_data.scan_width = NL80211_BSS_CHAN_WIDTH_20;
				if (scan_res->signal.signal_type == NRF_WIFI_SIGNAL_TYPE_MBM) {
					bss_data.signal = scan_res->signal.signal.mbm_signal;
				} else if (scan_res->signal.signal_type == NRF_WIFI_SIGNAL_TYPE_UNSPEC) {
					bss_data.signal = scan_res->signal.signal.unspec_signal;
				}
				tsf = div_u64(ktime_get_boottime_ns(), 1000);
				bss = cfg80211_inform_bss_data(vif_ctx->wiphy, &bss_data, CFG80211_BSS_FTYPE_UNKNOWN, (const u8 *)scan_res->mac_addr, tsf, scan_res->capability, scan_res->beacon_interval,
											ie, ie_len, GFP_KERNEL);
				if (bss) {
					cfg80211_put_bss(vif_ctx->wiphy, bss);
				}
			}
			status = wifi_nrf_fmac_auth(rpu_priv->rpu_ctx, vif_ctx->vif_idx, &vif_ctx->auth_info);
			if (status != WIFI_NRF_STATUS_SUCCESS) {
				printk("%s: MLME command failed (auth)\n", __func__);
			} else {
				printk("%s:Authentication request sent successfully\n", __func__);
			}
		}
	}
}

void event_proc_auth_resp(void *vif_ctx,
						struct nrf_wifi_umac_event_mlme *auth_resp,
						unsigned int event_len)
{
	struct nrf_vif_priv *vif_priv = NULL;
	struct wifi_nrf_rpu_priv_lnx *rpu_priv;
	enum wifi_nrf_status status = WIFI_NRF_STATUS_FAIL;

	printk("%s: valid_fields:%u\n", __func__, auth_resp->valid_fields);

	vif_priv = (struct nrf_vif_priv *)vif_ctx;
	rpu_priv = vif_priv->rpu_priv;
	if (vif_priv->vif_status == NRF_VIF_CONNECTING) {
		status = wifi_nrf_fmac_assoc(rpu_priv->rpu_ctx, vif_priv->vif_idx, &vif_priv->assoc_info);
		if (status != WIFI_NRF_STATUS_SUCCESS) {
			printk("%s: MLME command failed (assoc)\n", __func__);
		} else {
			printk("%s: Association request sent successfully\n", __func__);
		}
	}
}

void event_proc_assoc_resp(void *vif_ctx,
						 struct nrf_wifi_umac_event_mlme *assoc_resp,
						 unsigned int event_len)
{
	const struct ieee80211_mgmt *mgmt = NULL;
	const unsigned char *frame = NULL;
	unsigned int frame_len = 0;
	unsigned short status = WLAN_STATUS_UNSPECIFIED_FAILURE;

	struct nrf_vif_priv *vif_priv = NULL;
	vif_priv = (struct nrf_vif_priv *)vif_ctx;

	printk("%s: valid_fields:%u\n", __func__, assoc_resp->valid_fields);

	frame = assoc_resp->frame.frame;
	frame_len = assoc_resp->frame.frame_len;
	mgmt = (const struct ieee80211_mgmt *)frame;

	if (frame_len < 24 + sizeof(mgmt->u.assoc_resp)) {
		printk("%s: Association response frame too short\n", __func__);
		return;
	}

	status = le16_to_cpu(mgmt->u.assoc_resp.status_code);
	if (status != WLAN_STATUS_SUCCESS) {
		printk("assoc fail\n");
		vif_priv->vif_status = NRF_VIF_IDLE;
		cfg80211_connect_result(vif_priv->ndev, vif_priv->assoc_info.nrf_wifi_bssid, NULL, 0, NULL, 0, status, GFP_KERNEL);
	} else {
		vif_priv->vif_status = NRF_VIF_CONNECTED;
		cfg80211_connect_result(vif_priv->ndev, NULL, NULL, 0, NULL, 0, status, GFP_KERNEL);
		schedule_work(&vif_priv->ws_connected);
	}
	vif_priv->scan_request = NULL;
}

void cfg80211_process_fmac_event(struct fmac_event *event)
{
	struct nrf_wifi_umac_hdr *umac_hdr = NULL;
	unsigned char if_id = 0;
	bool more_res = false;

	umac_hdr = event->data;
	if_id = umac_hdr->ids.wdev_id;

	printk("%s: fmac event %d\n", __func__, umac_hdr->cmd_evnt);
	switch (umac_hdr->cmd_evnt) {
	case NRF_WIFI_UMAC_EVENT_TRIGGER_SCAN_START:
		event_proc_scan_start(event->vif_ctx, event->data, event->datalen);
		break;
	case NRF_WIFI_UMAC_EVENT_SCAN_DONE:
		event_proc_scan_done(event->vif_ctx, event->data, event->datalen);
		break;
	case NRF_WIFI_UMAC_EVENT_SCAN_DISPLAY_RESULT:
		if (umac_hdr->seq != 0) {
			more_res = true;
		}
		event_proc_disp_scan_res(event->vif_ctx,
				event->data,
				event->datalen,
				more_res);
		break;
#ifdef CONFIG_WPA_SUPP
	case NRF_WIFI_UMAC_EVENT_SCAN_RESULT:
		if (umac_hdr->seq != 0) {
			more_res = true;
		}
		event_proc_scan_res(event->vif_ctx,
				event->data,
				event->datalen,
				more_res);
		break;
	case NRF_WIFI_UMAC_EVENT_AUTHENTICATE:
		event_proc_auth_resp(event->vif_ctx,
				event->data,
				event->datalen);
		break;
	case NRF_WIFI_UMAC_EVENT_ASSOCIATE:
		event_proc_assoc_resp(event->vif_ctx,
				event->data,
				event->datalen);
		break;
#endif//CONFIG_WPA_SUPP
	default:
		printk("%s: No callback registered for event %d\n",
					  __func__,
					  umac_hdr->cmd_evnt);
		break;
	}
}

void wifi_nrf_wpa_supp_event_proc_disassoc(void *if_priv,
					   struct nrf_wifi_umac_event_mlme *disassoc,
					   unsigned int event_len)
{
	printk("%s: valid_fields:%u\n", __func__, disassoc->valid_fields);
}

void wifi_nrf_wpa_supp_event_get_wiphy(void *if_priv,
		struct nrf_wifi_event_get_wiphy *wiphy_info,
		unsigned int event_len)
{
	struct nrf_vif_priv *vif_ctx = NULL;
	int i, j;
	struct ndev_priv_context *ndev_data = NULL;
	struct ieee80211_supported_band *sband;

	if (!if_priv || !wiphy_info || !event_len) {
		printk("%s: Invalid parameters\n", __func__);
		return;
	}
	vif_ctx = (struct nrf_vif_priv *)if_priv;
	//memcpy(&vif_ctx->wiphy_info, wiphy_info, sizeof(vif_ctx->wiphy_info));

	for (i = 0; i < NRF_WIFI_EVENT_GET_WIPHY_NUM_BANDS; i++) {
		if(wiphy_info->sband[i].band == NRF_WIFI_BAND_2GHZ) {
			printk("Set up 2GHz band\n");
			//printk("ht cap: %u vht cap: %u\n", wiphy_info->sband[i].ht_cap.nrf_wifi_ht_supported, wiphy_info->sband[i].vht_cap.nrf_wifi_vht_supported);
			sband = &nrf_cfg80211_band_2ghz;
		} else if (wiphy_info->sband[i].band == NRF_WIFI_BAND_5GHZ) {
			printk("Set up 5GHz band\n");
			//printk("ht cap: %u vht cap: %u\n", wiphy_info->sband[i].ht_cap.nrf_wifi_ht_supported, wiphy_info->sband[i].vht_cap.nrf_wifi_vht_supported);
			sband = &nrf_cfg80211_band_5ghz;
		}
		sband->n_channels = wiphy_info->sband[i].nrf_wifi_n_channels;
		for (j = 0; j < sband->n_channels ;j++) {
			sband->channels[j].center_freq = wiphy_info->sband[i].channels[j].center_frequency;
			//sband->channels[j].hw_value = wiphy_info->sband[i].channels[j].hw_value;
		}
		for (j = 0; j < sband->n_bitrates ;j++) {
			sband->n_bitrates = wiphy_info->sband[i].nrf_wifi_n_bitrates;
			sband->bitrates[j].bitrate = wiphy_info->sband[i].bitrates[j].nrf_wifi_bitrate;
		}
		sband->ht_cap.ht_supported = wiphy_info->sband[i].ht_cap.nrf_wifi_ht_supported;
		if (sband->ht_cap.ht_supported) {
			int k;
			sband->ht_cap.cap = wiphy_info->sband[i].ht_cap.nrf_wifi_cap;
			sband->ht_cap.ampdu_factor = wiphy_info->sband[i].ht_cap.nrf_wifi_ampdu_factor;
			sband->ht_cap.ampdu_density = wiphy_info->sband[i].ht_cap.nrf_wifi_ampdu_density;
			sband->ht_cap.mcs.rx_highest = wiphy_info->sband[i].ht_cap.mcs.nrf_wifi_rx_highest;
			sband->ht_cap.mcs.tx_params = wiphy_info->sband[i].ht_cap.mcs.nrf_wifi_tx_params;
			for (k = 0; k < IEEE80211_HT_MCS_MASK_LEN; k++) {
				if (k > NRF_WIFI_IEEE80211_HT_MCS_MASK_LEN)
					break;
				sband->ht_cap.mcs.rx_mask[k] = wiphy_info->sband[i].ht_cap.mcs.nrf_wifi_rx_mask[k];
			}
		}
		sband->vht_cap.vht_supported = wiphy_info->sband[i].vht_cap.nrf_wifi_vht_supported;
		if (sband->vht_cap.vht_supported) {
			sband->vht_cap.cap = wiphy_info->sband[i].vht_cap.nrf_wifi_cap;
			sband->vht_cap.vht_mcs.rx_mcs_map = wiphy_info->sband[i].vht_cap.vht_mcs.rx_mcs_map;
			sband->vht_cap.vht_mcs.rx_highest = wiphy_info->sband[i].vht_cap.vht_mcs.rx_highest;
			sband->vht_cap.vht_mcs.tx_mcs_map = wiphy_info->sband[i].vht_cap.vht_mcs.tx_mcs_map;
			sband->vht_cap.vht_mcs.tx_highest = wiphy_info->sband[i].vht_cap.vht_mcs.tx_highest;
		}
		//wiphy->bands[NL80211_BAND_2GHZ] = sband;
	}
	vif_ctx->wiphy->bands[NL80211_BAND_2GHZ] = &nrf_cfg80211_band_2ghz;
	vif_ctx->wiphy->bands[NL80211_BAND_5GHZ] = &nrf_cfg80211_band_5ghz;
	vif_ctx->wiphy->max_scan_ssids = wiphy_info->max_scan_ssids;
	vif_ctx->wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;
	vif_ctx->wiphy->max_scan_ie_len = wiphy_info->max_scan_ie_len;
	vif_ctx->wiphy->max_remain_on_channel_duration = wiphy_info->max_remain_on_channel_duration;
	//vif_ctx->wiphy->max_ap_assoc_sta = wiphy_info->max_ap_assoc_sta;
	//wiphy->cipher_suites = mwifiex_cipher_suites;
	//wiphy->n_cipher_suites = ARRAY_SIZE(mwifiex_cipher_suites);

	vif_ctx->wiphy->max_sched_scan_ssids = wiphy_info->max_sched_scan_ssids;
	vif_ctx->wiphy->max_sched_scan_ie_len = wiphy_info->max_sched_scan_ie_len;
	vif_ctx->wiphy->max_match_sets = wiphy_info->max_match_sets;
	vif_ctx->wiphy->available_antennas_tx = wiphy_info->nrf_wifi_available_antennas_tx;
	vif_ctx->wiphy->available_antennas_rx = wiphy_info->nrf_wifi_available_antennas_rx;
	vif_ctx->wiphy->features = wiphy_info->features;
	printk("wiphy_name: %s \n", wiphy_info->wiphy_name);
	printk("band: %d ch:%u bit%u\n", wiphy_info->sband[0].band, wiphy_info->sband[0].nrf_wifi_n_channels, wiphy_info->sband[0].nrf_wifi_n_bitrates);
	printk("band: %d ch:%u bit%u\n", wiphy_info->sband[1].band, wiphy_info->sband[1].nrf_wifi_n_channels, wiphy_info->sband[1].nrf_wifi_n_bitrates);

	vif_ctx->wiphy->flags |= WIPHY_FLAG_HAVE_AP_SME |
			WIPHY_FLAG_AP_PROBE_RESP_OFFLOAD |
			WIPHY_FLAG_AP_UAPSD |
			WIPHY_FLAG_HAS_REMAIN_ON_CHANNEL |
			WIPHY_FLAG_HAS_CHANNEL_SWITCH |
			WIPHY_FLAG_PS_ON_BY_DEFAULT;

	if (wiphy_register(vif_ctx->wiphy) < 0) {
		goto l_error_wiphy_register;
	} else {
		vif_ctx->scan_request = NULL;
		vif_ctx->vif_status = NRF_VIF_IDLE;
		INIT_WORK(&vif_ctx->ws_scan, nrf_cfg80211_scan_routine);
		INIT_WORK(&vif_ctx->ws_connected, nrf_cfg80211_connected_routine);
#ifdef CONFIG_NRF700X_DATA_TX
		INIT_WORK(&vif_ctx->ws_data_tx, nrf_cfg80211_data_tx_routine);
#endif
	}

#ifdef CONFIG_NRF700X_DATA_TX
	vif_ctx->data_txq = wifi_nrf_utils_q_alloc(vif_ctx->rpu_priv->drv_priv->fmac_priv->opriv);
	if (vif_ctx->data_txq == NULL) {
		goto l_error_alloc_ndev;
	}
#endif

	vif_ctx->ndev = alloc_netdev(sizeof(*ndev_data), NDEV_NAME, NET_NAME_ENUM, ether_setup);
	if (vif_ctx->ndev == NULL) {
		goto l_error_alloc_ndev;
	}

	ndev_data = ndev_get_priv(vif_ctx->ndev);
	ndev_data->nrf_vif = vif_ctx;

	ndev_data->wdev.wiphy = vif_ctx->wiphy;
	ndev_data->wdev.netdev = vif_ctx->ndev;
	ndev_data->wdev.iftype = NL80211_IFTYPE_STATION;
	vif_ctx->ndev->ieee80211_ptr = &ndev_data->wdev;
	vif_ctx->ndev->netdev_ops = &nvf_ndev_ops;
	//SET_NETDEV_DEV(vif_ctx->ndev, wiphy_dev(vif_ctx->wiphy));
	eth_hw_addr_set(vif_ctx->ndev, vif_ctx->mac_addr);
	if (register_netdev(vif_ctx->ndev)) {
		goto l_error_ndev_register;
	}

	return;
l_error_ndev_register:
	free_netdev(vif_ctx->ndev);
l_error_alloc_ndev:
	wiphy_unregister(vif_ctx->wiphy);
l_error_wiphy_register:
	wiphy_free(vif_ctx->wiphy);
	printk("%s: Fail to set up wiphy\n", __func__);
	return;
}

#ifdef CONFIG_NRF700X_DATA_TX
void wifi_nrf_if_rx_frm(void *vif_ctx, void *frm)
{
	struct nrf_vif_priv *vif_priv = NULL;
	struct sk_buff *skb;
	struct nwb *nwb;
	unsigned char *data;
	unsigned int len;

	if (!frm) {
		printk("%s frm is NULL\n", __func__);
		return;
	}
	nwb = frm;
	vif_priv = (struct nrf_vif_priv *)vif_ctx;

	len = nwb->len;
	data = nwb->data;

	if (len == 0) {
		printk("Zero frame length\n");
		return;
	}
	printk("%s: len: %u\n", __func__, len);

	skb = dev_alloc_skb(len);
	if (!skb) {
		printk("Fail to allocate skb. Packet droped!\n");
		return;
	}
	memcpy(skb_put(skb, len), data, len);
	skb->dev = vif_priv->ndev;
	skb->protocol = eth_type_trans(skb, vif_priv->ndev);
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	netif_rx_ni(skb);
}

enum wifi_nrf_status wifi_nrf_if_carr_state_chg(void *vif_ctx,
						enum wifi_nrf_fmac_if_carr_state carr_state)
{
	enum wifi_nrf_status status = WIFI_NRF_STATUS_FAIL;
	struct nrf_vif_priv *vif_priv = NULL;
	vif_priv = (struct nrf_vif_priv *)vif_ctx;

	printk("%s: state: %u\n", __func__, carr_state);

	if (vif_priv == NULL) {
		printk("vif not ready\n");
		return WIFI_NRF_STATUS_SUCCESS;
	}

	if (vif_priv->ndev == NULL) {
		printk("ndev not ready\n");
		return WIFI_NRF_STATUS_SUCCESS;
	}

	if (carr_state == WIFI_NRF_FMAC_IF_CARR_STATE_ON) {
		if (!netif_carrier_ok(vif_priv->ndev)) {
			netif_carrier_on(vif_priv->ndev);
			netif_start_queue(vif_priv->ndev);
			status = WIFI_NRF_STATUS_SUCCESS;
		}
	} else if (carr_state == WIFI_NRF_FMAC_IF_CARR_STATE_OFF) {
		if (netif_carrier_ok(vif_priv->ndev)) {
			netif_stop_queue(vif_priv->ndev);
			netif_carrier_off(vif_priv->ndev);
			status = WIFI_NRF_STATUS_SUCCESS;
		}
	}

	return status;
}

#endif /* CONFIG_NRF700X_DATA_TX */
