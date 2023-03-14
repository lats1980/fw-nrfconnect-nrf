#ifndef __CFG80211_H__
#define __CFG80211_H__

#include <net/cfg80211.h>
#include <fmac_api.h>

#define WIPHY_NAME "nrf700x"
#define NDEV_NAME "nrf700x%d"

enum nrf700x_vif_status {
	NRF700X_VIF_IDLE,
	NRF700X_VIF_CONNECTING,
	NRF700X_VIF_CONNECTED
};

struct nrf700x_adapter {
    struct wiphy *wiphy;
    struct net_device *ndev;
	struct wifi_nrf_ctx_linux *rpu_ctx_linux;

	struct list_head *fmac_event_q;

    struct work_struct ws_scan;
	struct work_struct ws_connected;
    struct cfg80211_scan_request *scan_request;
	struct nrf_wifi_umac_auth_info auth_info;
	struct nrf_wifi_umac_assoc_info assoc_info;
	struct cfg80211_bss *bss;
#ifdef CONFIG_NRF700X_DATA_TX
	void *data_txq;
	struct work_struct ws_data_tx;
#endif
	unsigned char vif_idx;
	enum nrf700x_vif_status vif_status;
	u8 mac_addr[ETH_ALEN];
};

struct nrf700x_adapter *nrf700x_cfg80211_init(struct device *dev);

void nrf700x_uninit(struct nrf700x_adapter *ctx);

void cfg80211_process_fmac_event(struct fmac_event *event);

void wifi_nrf_wpa_supp_event_proc_auth_resp(void *if_priv,
					    struct nrf_wifi_umac_event_mlme *auth_resp,
					    unsigned int event_len);

void wifi_nrf_wpa_supp_event_proc_assoc_resp(void *if_priv,
					     struct nrf_wifi_umac_event_mlme *assoc_resp,
					     unsigned int event_len);

void wifi_nrf_wpa_supp_event_proc_disassoc(void *if_priv,
					   struct nrf_wifi_umac_event_mlme *disassoc,
					   unsigned int event_len);

#ifdef CONFIG_NRF700X_DATA_TX
void wifi_nrf_wpa_supp_event_get_wiphy(void *if_priv,
		struct nrf_wifi_event_get_wiphy *wiphy_info,
		unsigned int event_len);

void wifi_nrf_if_rx_frm(void *os_vif_ctx, void *frm);

enum wifi_nrf_status wifi_nrf_if_carr_state_chg(void *vif_ctx,
						enum wifi_nrf_fmac_if_carr_state carr_state);
#endif

#endif /* __CFH80211_H__ */