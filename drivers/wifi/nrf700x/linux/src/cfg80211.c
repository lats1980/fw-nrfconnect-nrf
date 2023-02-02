#include <linux_fmac_main.h>

struct wiphy_priv_context {
    struct nrf700x_adapter *nrf700x;
};

struct ndev_priv_context {
    struct nrf700x_adapter *nrf700x;
    struct wireless_dev wdev;
};

/* helper function that will retrieve main context from "priv" data of the wiphy */
static struct wiphy_priv_context *
wiphy_get_nrf700x_context(struct wiphy *wiphy) { return (struct wiphy_priv_context *) wiphy_priv(wiphy); }

/* helper function that will retrieve main context from "priv" data of the network device */
static struct ndev_priv_context *
ndev_get_nrf700x_context(struct net_device *ndev) { return (struct ndev_priv_context *) netdev_priv(ndev); }

static int nrf700x_scan(struct wiphy *wiphy, struct cfg80211_scan_request *request) {
    struct nrf700x_adapter *nrf700x = wiphy_get_nrf700x_context(wiphy)->nrf700x;

printk("request->n_channels: %u\n", request->n_channels);
printk("request->n_ssids: %d\n", request->n_ssids);

#if 0
    if(down_interruptible(&nrf700x->sem)) {
        return -ERESTARTSYS;
    }
#endif
    if (nrf700x->scan_request != NULL) {
        //up(&nrf700x->sem);
        return -EBUSY;
    }
    nrf700x->scan_request = request;

    //up(&nrf700x->sem);

    if (!schedule_work(&nrf700x->ws_scan)) {
        return -EBUSY;
    }

    return 0; /* OK */
}

static struct cfg80211_ops nrf700x_cfg_ops = {
        .scan = nrf700x_scan,
};

/* Network packet transmit.
 * Callback that called by the kernel when packet of data should be sent.
 * In this example it does nothing. */
static netdev_tx_t nvf_ndo_start_xmit(struct sk_buff *skb,
                               struct net_device *dev) {
    /* Dont forget to cleanup skb, as its ownership moved to xmit callback. */
    kfree_skb(skb);
    return NETDEV_TX_OK;
}

/* Structure of functions for network devices.
 * It should have at least ndo_start_xmit functions that called for packet to be sent. */
static struct net_device_ops nvf_ndev_ops = {
        .ndo_start_xmit = nvf_ndo_start_xmit,
};

static struct ieee80211_channel nrf700x_supported_channels_2ghz[14];
static struct ieee80211_rate nrf700x_supported_rates_2ghz[12];

static struct ieee80211_supported_band nrf700x_band_2ghz = {
        //.ht_cap.cap = IEEE80211_HT_CAP_SGI_20, /* add other band capabilities if needed, like 40 width etc. */
        //.ht_cap.ht_supported = false,
    .band = NL80211_BAND_2GHZ,
    .channels = nrf700x_supported_channels_2ghz,
    .n_channels = ARRAY_SIZE(nrf700x_supported_channels_2ghz),
    .bitrates = nrf700x_supported_rates_2ghz,
    .n_bitrates = ARRAY_SIZE(nrf700x_supported_rates_2ghz),
};

static struct ieee80211_channel nrf700x_supported_channels_5ghz[28];
static struct ieee80211_rate nrf700x_supported_rates_5ghz[8];

static struct ieee80211_supported_band nrf700x_band_5ghz = {
        //.ht_cap.cap = IEEE80211_HT_CAP_SGI_20, /* add other band capabilities if needed, like 40 width etc. */
        //.ht_cap.ht_supported = false,
    .band = NL80211_BAND_5GHZ,
    .channels = nrf700x_supported_channels_5ghz,
    .n_channels = ARRAY_SIZE(nrf700x_supported_channels_5ghz),
    .bitrates = nrf700x_supported_rates_5ghz,
    .n_bitrates = ARRAY_SIZE(nrf700x_supported_rates_5ghz),
};

int nrf700x_setup_bands(struct ieee80211_supported_band * bands)
{
    return -1;
}

int nrf700x_cfg80211_init(struct nrf700x_adapter *vif_ctx)
{
    struct wiphy_priv_context *wiphy_data = NULL;
    struct ndev_priv_context *ndev_data = NULL;
    int i, j;
    struct ieee80211_supported_band *sband;

    vif_ctx->wiphy = wiphy_new_nm(&nrf700x_cfg_ops, sizeof(struct wiphy_priv_context), WIPHY_NAME);
    if (vif_ctx->wiphy == NULL) {
        printk("%s: fail to allocate new wiphy\n", __func__);
        return -1;
    }

    wiphy_data = wiphy_get_nrf700x_context(vif_ctx->wiphy);
    wiphy_data->nrf700x = vif_ctx;

    vif_ctx->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);


	for (i = 0; i < NRF_WIFI_EVENT_GET_WIPHY_NUM_BANDS; i++) {
        if(vif_ctx->wiphy_info.sband[i].band == NRF_WIFI_BAND_2GHZ) {
            printk("Set up 2GHz band\n");
            //printk("ht cap: %u vht cap: %u\n", vif_ctx->wiphy_info.sband[i].ht_cap.nrf_wifi_ht_supported, vif_ctx->wiphy_info.sband[i].vht_cap.nrf_wifi_vht_supported);
            sband = &nrf700x_band_2ghz;
        } else if (vif_ctx->wiphy_info.sband[i].band == NRF_WIFI_BAND_5GHZ) {
            printk("Set up 5GHz band\n");
            //printk("ht cap: %u vht cap: %u\n", vif_ctx->wiphy_info.sband[i].ht_cap.nrf_wifi_ht_supported, vif_ctx->wiphy_info.sband[i].vht_cap.nrf_wifi_vht_supported);
            sband = &nrf700x_band_5ghz;
        }
        sband->n_channels = vif_ctx->wiphy_info.sband[i].nrf_wifi_n_channels;
        for (j = 0; j < sband->n_channels ;j++) {
            sband->channels[j].center_freq = vif_ctx->wiphy_info.sband[i].channels[j].center_frequency;
            //sband->channels[j].hw_value = vif_ctx->wiphy_info.sband[i].channels[j].hw_value;
        }
        for (j = 0; j < sband->n_bitrates ;j++) {
            sband->n_bitrates = vif_ctx->wiphy_info.sband[i].nrf_wifi_n_bitrates;
            sband->bitrates[j].bitrate = vif_ctx->wiphy_info.sband[i].bitrates[j].nrf_wifi_bitrate;
        }
        sband->ht_cap.ht_supported = vif_ctx->wiphy_info.sband[i].ht_cap.nrf_wifi_ht_supported;
        if (sband->ht_cap.ht_supported) {
            int k;
            sband->ht_cap.cap = vif_ctx->wiphy_info.sband[i].ht_cap.nrf_wifi_cap;
            sband->ht_cap.ampdu_factor = vif_ctx->wiphy_info.sband[i].ht_cap.nrf_wifi_ampdu_factor;
            sband->ht_cap.ampdu_density = vif_ctx->wiphy_info.sband[i].ht_cap.nrf_wifi_ampdu_density;
            sband->ht_cap.mcs.rx_highest = vif_ctx->wiphy_info.sband[i].ht_cap.mcs.nrf_wifi_rx_highest;
            sband->ht_cap.mcs.tx_params = vif_ctx->wiphy_info.sband[i].ht_cap.mcs.nrf_wifi_tx_params;
            for (k = 0; k < IEEE80211_HT_MCS_MASK_LEN; k++) {
                if (k > NRF_WIFI_IEEE80211_HT_MCS_MASK_LEN)
                    break;
                sband->ht_cap.mcs.rx_mask[k] = vif_ctx->wiphy_info.sband[i].ht_cap.mcs.nrf_wifi_rx_mask[k];
            }
        }
        sband->vht_cap.vht_supported = vif_ctx->wiphy_info.sband[i].vht_cap.nrf_wifi_vht_supported;
        if (sband->vht_cap.vht_supported) {
            sband->vht_cap.cap = vif_ctx->wiphy_info.sband[i].vht_cap.nrf_wifi_cap;
            sband->vht_cap.vht_mcs.rx_mcs_map = vif_ctx->wiphy_info.sband[i].vht_cap.vht_mcs.rx_mcs_map;
            sband->vht_cap.vht_mcs.rx_highest = vif_ctx->wiphy_info.sband[i].vht_cap.vht_mcs.rx_highest;
            sband->vht_cap.vht_mcs.tx_mcs_map = vif_ctx->wiphy_info.sband[i].vht_cap.vht_mcs.tx_mcs_map;
            sband->vht_cap.vht_mcs.tx_highest = vif_ctx->wiphy_info.sband[i].vht_cap.vht_mcs.tx_highest;
        }
        //wiphy->bands[NL80211_BAND_2GHZ] = sband;
	}

    vif_ctx->wiphy->bands[NL80211_BAND_2GHZ] = &nrf700x_band_2ghz;
    vif_ctx->wiphy->bands[NL80211_BAND_5GHZ] = &nrf700x_band_5ghz;
    vif_ctx->wiphy->max_scan_ssids = vif_ctx->wiphy_info.max_scan_ssids;
    vif_ctx->wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;

	vif_ctx->wiphy->max_scan_ie_len = vif_ctx->wiphy_info.max_scan_ie_len;
	vif_ctx->wiphy->max_remain_on_channel_duration = vif_ctx->wiphy_info.max_remain_on_channel_duration;
    //vif_ctx->wiphy->interface_modes = vif_ctx->wiphy_info.interface_modes;
    //vif_ctx->wiphy->max_ap_assoc_sta = vif_ctx->wiphy_info.max_ap_assoc_sta;
	//wiphy->cipher_suites = mwifiex_cipher_suites;
	//wiphy->n_cipher_suites = ARRAY_SIZE(mwifiex_cipher_suites);

	vif_ctx->wiphy->max_sched_scan_ssids = vif_ctx->wiphy_info.max_sched_scan_ssids;
	vif_ctx->wiphy->max_sched_scan_ie_len = vif_ctx->wiphy_info.max_sched_scan_ie_len;
	vif_ctx->wiphy->max_match_sets = vif_ctx->wiphy_info.max_match_sets;
	vif_ctx->wiphy->available_antennas_tx = vif_ctx->wiphy_info.nrf_wifi_available_antennas_tx;
	vif_ctx->wiphy->available_antennas_rx = vif_ctx->wiphy_info.nrf_wifi_available_antennas_rx;
    vif_ctx->wiphy->features = vif_ctx->wiphy_info.features;
    printk("wiphy_name: %s \n", vif_ctx->wiphy_info.wiphy_name);
    printk("band: %d ch:%u bit%u\n", vif_ctx->wiphy_info.sband[0].band, vif_ctx->wiphy_info.sband[0].nrf_wifi_n_channels, vif_ctx->wiphy_info.sband[0].nrf_wifi_n_bitrates);
    printk("band: %d ch:%u bit%u\n", vif_ctx->wiphy_info.sband[1].band, vif_ctx->wiphy_info.sband[1].nrf_wifi_n_channels, vif_ctx->wiphy_info.sband[1].nrf_wifi_n_bitrates);

    if (wiphy_register(vif_ctx->wiphy) < 0) {
        goto l_error_wiphy_register;
    }

    vif_ctx->ndev = alloc_netdev(sizeof(*ndev_data), NDEV_NAME, NET_NAME_ENUM, ether_setup);
    if (vif_ctx->ndev == NULL) {
        goto l_error_alloc_ndev;
    }

    ndev_data = ndev_get_nrf700x_context(vif_ctx->ndev);
    ndev_data->nrf700x = vif_ctx;

    ndev_data->wdev.wiphy = vif_ctx->wiphy;
    ndev_data->wdev.netdev = vif_ctx->ndev;
    ndev_data->wdev.iftype = NL80211_IFTYPE_STATION;
    vif_ctx->ndev->ieee80211_ptr = &ndev_data->wdev;

    vif_ctx->ndev->netdev_ops = &nvf_ndev_ops;

    if (register_netdev(vif_ctx->ndev)) {
        goto l_error_ndev_register;
    }

    return 0;
    l_error_ndev_register:
    free_netdev(vif_ctx->ndev);
    l_error_alloc_ndev:
    wiphy_unregister(vif_ctx->wiphy);
    l_error_wiphy_register:
    wiphy_free(vif_ctx->wiphy);

    return -1;
}

void nrf700x_uninit(struct nrf700x_adapter *ctx) {
    if (ctx == NULL) {
        return;
    }
    unregister_netdev(ctx->ndev);
    free_netdev(ctx->ndev);
    wiphy_unregister(ctx->wiphy);
    wiphy_free(ctx->wiphy);
    kfree(ctx);
}

void wifi_nrf_event_proc_scan_start_linux(void *if_priv,
					struct nrf_wifi_umac_event_trigger_scan *scan_start_event,
					unsigned int event_len)
{
	printk("scan start\n");
}

void wifi_nrf_event_proc_scan_done_linux(void *vif_ctx,
				       struct nrf_wifi_umac_event_trigger_scan *scan_done_event,
				       unsigned int event_len)
{

	enum wifi_nrf_status status = WIFI_NRF_STATUS_FAIL;
    struct wifi_nrf_ctx_linux *rpu_ctx_linux = container_of(vif_ctx, struct wifi_nrf_ctx_linux, vif_ctx_linux);

	printk("scan done\n");
    if(!rpu_ctx_linux) {
        printk("rpu_ctx_linux NULL\n");
        return;
    }

	status = wifi_nrf_fmac_scan_res_get(rpu_ctx_linux->rpu_ctx,
					    rpu_ctx_linux->vif_ctx_linux.vif_idx,
					    SCAN_DISPLAY);
	if (status != WIFI_NRF_STATUS_SUCCESS) {
		printk("%s: wifi_nrf_fmac_scan failed\n", __func__);
	}
}

void wifi_nrf_event_proc_disp_scan_res_linux(void *vif_ctx,
				struct nrf_wifi_umac_event_new_scan_display_results *scan_res,
				unsigned int event_len,
				bool more_res)
{
	struct nrf700x_adapter *vif_ctx_linux = NULL;
	struct umac_display_results *r = NULL;
	unsigned int i = 0;
    struct cfg80211_scan_info info;

	vif_ctx_linux = (struct nrf700x_adapter *)vif_ctx;
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
        bss_data.chan = ieee80211_get_channel(vif_ctx_linux->wiphy, ieee80211_channel_to_frequency(r->nwk_channel, r->nwk_band));
        bss_data.scan_width = NL80211_BSS_CHAN_WIDTH_20;
		if (r->signal.signal_type == NRF_WIFI_SIGNAL_TYPE_MBM) {
			bss_data.signal = r->signal.signal.mbm_signal;
			//printk("Signal: %u\n", r->signal.signal.mbm_signal/100);
		} else if (r->signal.signal_type == NRF_WIFI_SIGNAL_TYPE_UNSPEC) {
			//res.rssi = (r->signal.signal.unspec_signal);
		}

		bss = cfg80211_inform_bss_data(vif_ctx_linux->wiphy, &bss_data, CFG80211_BSS_FTYPE_UNKNOWN, (const u8 *)r->mac_addr, 0, r->capability, r->beacon_interval,
									ie, sizeof(ie), GFP_KERNEL);
			//memcpy(res.mac,	r->mac_addr, NRF_WIFI_ETH_ADDR_LEN);
			//res.mac_length = NRF_WIFI_ETH_ADDR_LEN;
		/* free, cfg80211_inform_bss_data() returning cfg80211_bss structure refcounter of which should be decremented if its not used. */
		if (bss) {
			cfg80211_put_bss(vif_ctx_linux->wiphy, bss);
		}
		/* NET_MGMT dropping events if too many are queued */
		//k_yield();
	}

	/*
    if(down_interruptible(&nrf700x->sem)) {
        return;
    }
	*/
	if (!more_res) {
		printk("finish scan\n");
		info.aborted = true;
		cfg80211_scan_done(vif_ctx_linux->scan_request, &info);
		vif_ctx_linux->scan_request = NULL;
		up(&vif_ctx_linux->sem);
	}
}

void wifi_nrf_wpa_supp_event_get_wiphy(void *if_priv,
		struct nrf_wifi_event_get_wiphy *wiphy_info,
		unsigned int event_len)
{
    struct nrf700x_adapter *vif_ctx = NULL;

	if (!if_priv || !wiphy_info || !event_len) {
		printk("%s: Invalid parameters\n", __func__);
		return;
	}
    vif_ctx = (struct nrf700x_adapter *)if_priv;
    memcpy(&vif_ctx->wiphy_info, wiphy_info, sizeof(vif_ctx->wiphy_info));
	if(nrf700x_cfg80211_init(vif_ctx)) {
		printk("%s fail to init cfg80211\n", __func__);
    }
}
