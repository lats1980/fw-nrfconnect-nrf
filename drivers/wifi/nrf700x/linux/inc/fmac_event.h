#ifndef __FMAC_EVENT_H__
#define __FMAC_EVENT_H__

#include <linux/kernel.h>
#include <fmac_api.h>

enum fmac_event_type {
	FMAC_EVENT_UMAC,
	FMAC_EVENT_CARR_STATE,
	FMAC_EVENT_DATA
};

struct fmac_event {
	struct list_head q;
	enum fmac_event_type type;
	void *vif_ctx;
	u32 datalen;
	void *data;
	bool more_res;
};

void nrf_wifi_umac_event_trigger_scan_linux(void *os_vif_ctx,
					struct nrf_wifi_umac_event_trigger_scan *trigger_scan_event,
					unsigned int event_len);

void nrf_wifi_umac_event_new_scan_display_results_linux(void *os_vif_ctx,
					struct nrf_wifi_umac_event_new_scan_display_results *scan_res,
					unsigned int event_len,
					bool more_res);

void nrf_wifi_umac_event_new_scan_results_linux(void *os_vif_ctx,
				   struct nrf_wifi_umac_event_new_scan_results *scan_res,
				   unsigned int event_len,
				   bool more_res);

void nrf_wifi_umac_event_mlme_linux(void *os_vif_ctx,
					    struct nrf_wifi_umac_event_mlme *mlme,
					    unsigned int event_len);

enum wifi_nrf_status nrf_wifi_umac_event_carr_state_chg(void *vif_ctx,
						enum wifi_nrf_fmac_if_carr_state carr_state);

void nrf_wifi_umac_event_rx_frm_linux(void *os_vif_ctx, void *frm);

#endif /* __FMAC_EVENT_H__ */