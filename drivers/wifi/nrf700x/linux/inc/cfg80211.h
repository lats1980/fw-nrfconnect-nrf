#include <net/cfg80211.h>
#include <fmac_api.h>

#define WIPHY_NAME "nrf700x"
#define NDEV_NAME "nrf700x%d"

struct nrf700x_adapter {
    struct wiphy *wiphy;
    struct net_device *ndev;

    struct semaphore sem;
    struct work_struct ws_scan;
    struct cfg80211_scan_request *scan_request;

	unsigned char vif_idx;
    struct nrf_wifi_event_get_wiphy wiphy_info;
};

int nrf700x_cfg80211_init(struct nrf700x_adapter *ret);
void nrf700x_uninit(struct nrf700x_adapter *ctx);

void wifi_nrf_event_proc_scan_start_linux(void *if_priv,
					struct nrf_wifi_umac_event_trigger_scan *scan_start_event,
					unsigned int event_len);

void wifi_nrf_event_proc_scan_done_linux(void *vif_ctx,
				       struct nrf_wifi_umac_event_trigger_scan *scan_done_event,
				       unsigned int event_len);

void wifi_nrf_event_proc_disp_scan_res_linux(void *vif_ctx,
				struct nrf_wifi_umac_event_new_scan_display_results *scan_res,
				unsigned int event_len,
				bool more_res);

void wifi_nrf_wpa_supp_event_get_wiphy(void *if_priv,
		struct nrf_wifi_event_get_wiphy *wiphy_info,
		unsigned int event_len);