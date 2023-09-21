/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <string.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#if defined(CLOCK_FEATURE_HFCLK_DIVIDE_PRESENT) || NRF_CLOCK_HAS_HFCLK192M
#include <nrfx_clock.h>
#endif
#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/conn_mgr_connectivity.h>
#include <net/download_client.h>

#if CONFIG_MODEM_KEY_MGMT
#include <modem/modem_key_mgmt.h>
#else
#include <zephyr/net/tls_credentials.h>
#endif
/*
extern int file_storage_init(void);
extern int file_storage_read(uint8_t *const buf, const size_t buf_size);
extern int file_storage_write(const uint8_t *const buf, const size_t buf_size);
extern int file_storage_write_stream_start(void);
extern int file_storage_write_stream_stop(void);
extern int file_storage_write_stream_fragment(const uint8_t *const buf, const size_t buf_size);
extern void file_storage_lsdir(void);
*/
#define URL CONFIG_SAMPLE_FILE_URL
#define SEC_TAG CONFIG_SAMPLE_SEC_TAG

/* Macros used to subscribe to specific Zephyr NET management events. */
#define L4_EVENT_MASK (NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED)
#define CONN_LAYER_EVENT_MASK (NET_EVENT_CONN_IF_FATAL_ERROR)

#define PROGRESS_WIDTH 50
#define STARTING_OFFSET 0

/* Zephyr NET management event callback structures. */
static struct net_mgmt_event_callback l4_cb;
static struct net_mgmt_event_callback conn_cb;
static struct net_if *net_if;

static K_SEM_DEFINE(network_connected_sem, 0, 1);
static K_SEM_DEFINE(file_downloaded_sem, 0, 1);

#if CONFIG_SAMPLE_SECURE_SOCKET
static const char cert[] = {
	#include CONFIG_SAMPLE_CERT_FILE
};
static int sec_tag_list[] = { SEC_TAG };
BUILD_ASSERT(sizeof(cert) < KB(4), "Certificate too large");
#endif

static struct download_client downloader;
static struct download_client_cfg config = {
#if CONFIG_SAMPLE_SECURE_SOCKET
	.sec_tag_list = sec_tag_list,
	.sec_tag_count = ARRAY_SIZE(sec_tag_list),
	.set_tls_hostname = true,
#endif
};

#if CONFIG_SAMPLE_COMPUTE_HASH
#include <mbedtls/sha256.h>
static mbedtls_sha256_context sha256_ctx;
#endif

static int64_t ref_time;

#if CONFIG_SAMPLE_SECURE_SOCKET
static int cert_provision(void)
{
	int err;

	printk("Provisioning certificate\n");

#if CONFIG_MODEM_KEY_MGMT
	bool exists;

	err = modem_key_mgmt_exists(SEC_TAG,
				    MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
				    &exists);
	if (err) {
		printk("Failed to check for certificates err %d\n", err);
		return err;
	}

	if (exists) {
		printk("Certificate ");
		/* Let's compare the existing credential */
		err = modem_key_mgmt_cmp(SEC_TAG,
					 MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
					 cert, sizeof(cert) - 1);

		printk("%s\n", err ? "mismatch" : "match");

		if (!err) {
			return 0;
		}
	}

	/*  Provision certificate to the modem */
	err = modem_key_mgmt_write(SEC_TAG,
				   MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
				   cert, sizeof(cert) - 1);
	if (err) {
		printk("Failed to provision certificate, err %d\n", err);
		return err;
	}
#else /* CONFIG_MODEM_KEY_MGMT */
	err = tls_credential_add(SEC_TAG,
				 TLS_CREDENTIAL_CA_CERTIFICATE,
				 cert,
				 sizeof(cert));
	if (err < 0) {
		printk("Failed to register CA certificate: %d\n", err);
		return err;
	}
#endif /* !CONFIG_MODEM_KEY_MGMT */

	return 0;
}
#endif

static void on_net_event_l4_disconnected(void)
{
	printk("Disconnected from network\n");
}

static void on_net_event_l4_connected(void)
{
	k_sem_give(&network_connected_sem);
}

static void l4_event_handler(struct net_mgmt_event_callback *cb,
			     uint32_t event,
			     struct net_if *iface)
{
	switch (event) {
	case NET_EVENT_L4_CONNECTED:
		printk("IP Up\n");
		on_net_event_l4_connected();
		break;
	case NET_EVENT_L4_DISCONNECTED:
		printk("IP down\n");
		on_net_event_l4_disconnected();
		break;
	default:
		break;
	}
}

static void connectivity_event_handler(struct net_mgmt_event_callback *cb,
				       uint32_t event,
				       struct net_if *iface)
{
	if (event == NET_EVENT_CONN_IF_FATAL_ERROR) {
		printk("Fatal error received from the connectivity layer, rebooting\n");
		return;
	}
}

static void progress_print(size_t downloaded, size_t file_size)
{
	const int percent = (downloaded * 100) / file_size;
	size_t lpad = (percent * PROGRESS_WIDTH) / 100;
	size_t rpad = PROGRESS_WIDTH - lpad;

	printk("\r[ %3d%% ] |", percent);
	for (size_t i = 0; i < lpad; i++) {
		printk("=");
	}
	for (size_t i = 0; i < rpad; i++) {
		printk(" ");
	}
	printk("| (%d/%d bytes)", downloaded, file_size);
}

static int callback(const struct download_client_evt *event)
{
	static size_t downloaded;
	static size_t file_size;
	uint32_t speed;
	int64_t ms_elapsed;

	if (downloaded == 0) {
		download_client_file_size_get(&downloader, &file_size);
		downloaded += STARTING_OFFSET;
	}

	switch (event->id) {
	case DOWNLOAD_CLIENT_EVT_FRAGMENT:
		downloaded += event->fragment.len;
		if (file_size) {
			//progress_print(downloaded, file_size);
		} else {
			//printk("\r[ %d bytes ] \n", downloaded);
		}

		// int rc = file_storage_write_stream_fragment(event->fragment.buf, event->fragment.len);
		// if (rc < 0) {
		// 	printk("Failed to store fragment, error: %d\n", rc);
		// } else {
		// 	//printk("Stored fragment of size %d to file\n", rc);
		// }
#if CONFIG_SAMPLE_COMPUTE_HASH
		mbedtls_sha256_update(&sha256_ctx, event->fragment.buf, event->fragment.len);
#endif
		return 0;

	case DOWNLOAD_CLIENT_EVT_DONE:
		ms_elapsed = k_uptime_delta(&ref_time);
		speed = ((float)file_size / ms_elapsed) * MSEC_PER_SEC;

		// (void)file_storage_write_stream_stop();

		printk("\nDownload completed in %lld ms @ %d bytes per sec, total %d bytes\n",
		       ms_elapsed, speed, downloaded);

#if CONFIG_SAMPLE_COMPUTE_HASH
		uint8_t hash[32];
		uint8_t hash_str[64 + 1];

		mbedtls_sha256_finish(&sha256_ctx, hash);
		mbedtls_sha256_free(&sha256_ctx);

		bin2hex(hash, sizeof(hash), hash_str, sizeof(hash_str));

		printk("SHA256: %s\n", hash_str);

#if CONFIG_SAMPLE_COMPARE_HASH
		if (strcmp(hash_str, CONFIG_SAMPLE_SHA256_HASH)) {
			printk("Expect: %s\n", CONFIG_SAMPLE_SHA256_HASH);
			printk("SHA256 mismatch!\n");
		}
#endif /* CONFIG_SAMPLE_COMPARE_HASH */
#endif /* CONFIG_SAMPLE_COMPUTE_HASH */

		//(void)conn_mgr_if_disconnect(net_if);
		// file_storage_lsdir();
		printk("Bye\n");
		return 0;

	case DOWNLOAD_CLIENT_EVT_ERROR:
		// (void)file_storage_write_stream_stop();

		printk("Error %d during download\n", event->error);
		if (event->error == -ECONNRESET) {
			/* With ECONNRESET, allow library to attempt a reconnect by returning 0 */
		} else {
			//(void)conn_mgr_if_disconnect(net_if);
			/* Stop download */
			return -1;
		}
		break;
	case DOWNLOAD_CLIENT_EVT_CLOSED:
		// (void)file_storage_write_stream_stop();
		printk("Socket closed\n");
		k_sem_give(&file_downloaded_sem);
		break;
	}

	return 0;
}

int main(void)
{
	int err;

#if defined(CLOCK_FEATURE_HFCLK_DIVIDE_PRESENT) || NRF_CLOCK_HAS_HFCLK192M
	/* For now hardcode to 128MHz */
	int ret;
	/*
	nrfx_clock_divider_set(NRF_CLOCK_DOMAIN_HFCLK,
			       NRF_CLOCK_HFCLK_DIV_1);
			       */
	ret = nrfx_clock_divider_set(NRF_CLOCK_DOMAIN_HFCLK, NRF_CLOCK_HFCLK_DIV_1);
	ret -= NRFX_ERROR_BASE_NUM;
	if (ret) {
		return ret;
	}

	nrfx_clock_hfclk_start();
	while (!nrfx_clock_hfclk_is_running()) {
	}

#endif
	printk("Starting %s with CPU frequency: %d MHz\n", CONFIG_BOARD, SystemCoreClock/MHZ(1));
/*
	if (file_storage_init()) {
		printk("EXIT\n");
		return 0;
	}
*/
	printk("Download client sample started\n");

	net_if = net_if_get_default();
	if (net_if == NULL) {
		printk("Pointer to network interface is NULL\n");
		return -ECANCELED;
	}

	/* Setup handler for Zephyr NET Connection Manager events. */
	net_mgmt_init_event_callback(&l4_cb, l4_event_handler, L4_EVENT_MASK);
	net_mgmt_add_event_callback(&l4_cb);

	/* Setup handler for Zephyr NET Connection Manager Connectivity layer. */
	net_mgmt_init_event_callback(&conn_cb, connectivity_event_handler, CONN_LAYER_EVENT_MASK);
	net_mgmt_add_event_callback(&conn_cb);

#if CONFIG_SAMPLE_SECURE_SOCKET
	/* Provision certificates before connecting to the network */
	err = cert_provision();
	if (err) {
		return 0;
	}
#endif

	/* Add temporary fix to prevent using Wi-Fi before WPA supplicant is ready. */
	/* Remove this whenever wpa supplicant ready events has been added. */
	k_sleep(K_SECONDS(1));

	printk("Connecting to network\n");

	err = conn_mgr_if_connect(net_if);
	if (err) {
		printk("conn_mgr_if_connect, error: %d\n", err);
		return err;
	}

	k_sem_take(&network_connected_sem, K_FOREVER);

	printk("Network connected\n");
	k_sleep(K_SECONDS(5));
	while(1) {
		printk("Start connecting\n");
		err = download_client_init(&downloader, callback);
		if (err) {
			printk("Failed to initialize the client, err %d", err);
			return 0;
		}

	#if CONFIG_SAMPLE_COMPUTE_HASH
		mbedtls_sha256_init(&sha256_ctx);
		mbedtls_sha256_starts(&sha256_ctx, false);
	#endif

		// err = file_storage_write_stream_start();
		// if (err) {
		// 	printk("Failed to start write stream: %d\n", err);
		// 	return 0;
		// }
		ref_time = k_uptime_get();


		err = download_client_get(&downloader, URL, &config, URL, STARTING_OFFSET);
		if (err) {
			printk("Failed to start the downloader, err %d", err);
		/*
			err = file_storage_write_stream_stop();
			if (err) {
				printk("Failed to stop write stream: %d\n", err);
			}
		*/
			return 0;
		}

		printk("Downloading %s\n", URL);
		k_sem_take(&file_downloaded_sem, K_FOREVER);
		/*
		err = download_client_disconnect(&downloader);
		if (err) {
			printk("Failed to disconnect the downloader, err %d", err);
		}
		*/
		k_sleep(K_SECONDS(5));
	}
	return 0;
}
