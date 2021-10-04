/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include <logging/log.h>
#include <zephyr.h>
#include <stdio.h>
#include <string.h>
#include <net/socket.h>
#include <net/socket_offload.h>
#include <sys/ring_buffer.h>
#include "slm_util.h"
#include "slm_native_tls.h"
#include "slm_at_host.h"
#include "slm_at_tcp_proxy.h"
#if defined(CONFIG_SLM_UI)
#include "slm_ui.h"
#endif
#if defined(CONFIG_SLM_DIAG)
#include "slm_diag.h"
#include "slm_stats.h"
#endif

LOG_MODULE_REGISTER(tcp_proxy, CONFIG_SLM_LOG_LEVEL);

#define THREAD_STACK_SIZE	(KB(3) + NET_IPV4_MTU)
#define THREAD_PRIORITY		K_LOWEST_APPLICATION_THREAD_PRIO

/* max 2, listening and incoming sockets */
#define MAX_POLL_FD		2

/* Some features need future modem firmware support */
#define SLM_TCP_PROXY_FUTURE_FEATURE	0

/**@brief Proxy operations. */
enum slm_tcp_proxy_operation {
	AT_SERVER_STOP,
	AT_FILTER_CLEAR =  AT_SERVER_STOP,
	AT_CLIENT_DISCONNECT = AT_SERVER_STOP,
	AT_SERVER_START,
	AT_FILTER_SET =  AT_SERVER_START,
	AT_CLIENT_CONNECT = AT_SERVER_START,
	AT_SERVER_START_WITH_DATAMODE,
	AT_CLIENT_CONNECT_WITH_DATAMODE = AT_SERVER_START_WITH_DATAMODE
};

/**@brief Proxy roles. */
enum slm_tcp_proxy_role {
	AT_TCP_ROLE_CLIENT,
	AT_TCP_ROLE_SERVER
};

/**@brief Proxy operations for auto accept. */
enum slm_tcp_proxy_aa_operation {
	AT_TCP_SVR_AA_OFF,
	AT_TCP_SVR_AA_ON
};

/**@brief Proxy operations for accept-reject. */
enum slm_tcp_proxy_ar_operation {
	AT_TCP_SVR_AR_REJECT,
	AT_TCP_SVR_AR_ACCEPT,
	AT_TCP_SVR_AR_CONNECTING,
	AT_TCP_SVR_AR_UNKNOWN
};

/**@brief TCP Proxy server state. */
enum slm_tcpsvr_state {
	TCPSVR_INIT,
#if defined(CONFIG_SLM_CUSTOMIZED_RS232)
	TCPSVR_RI_ON,
	TCPSVR_RI_OFF,
	TCPSVR_POST_RI,
#endif
	TCPSVR_CONNECTING,
	TCPSVR_CONNECTED
};

static enum slm_tcpsvr_state tcpsvr_state;
static struct k_work_delayable tcpsvr_state_work;
static char ip_allowlist[CONFIG_SLM_TCP_FILTER_SIZE][INET_ADDRSTRLEN];
RING_BUF_DECLARE(data_buf, CONFIG_SLM_SOCKET_RX_MAX * 2);
static struct k_thread tcp_thread;
static struct k_work disconnect_work;
static K_THREAD_STACK_DEFINE(tcp_thread_stack, THREAD_STACK_SIZE);
#if defined(CONFIG_SLM_CUSTOMIZED)
K_TIMER_DEFINE(conn_timer, NULL, NULL);
#endif

static struct sockaddr_in remote;
static struct tcp_proxy_t {
	int sock;		/* Socket descriptor. */
	sec_tag_t sec_tag;	/* Security tag of the credential */
	int sock_peer;		/* Socket descriptor for peer. */
	int role;		/* Client or Server proxy */
	bool datamode;		/* Data mode flag*/
	bool filtermode;	/* Filtering mode flag */
	bool aa;		/* Auto accept mode flag*/
	uint16_t ar;		/* accept-reject flag*/
#if defined(CONFIG_SLM_CUSTOMIZED)
	uint16_t timeout;	/* Peer connection timeout */
#endif
} proxy;
static struct pollfd fds[MAX_POLL_FD];
static int nfds;

/* global functions defined in different files */
void rsp_send(const uint8_t *str, size_t len);
int enter_datamode(slm_datamode_handler_t handler);
bool exit_datamode(void);
bool check_uart_flowcontrol(void);

/* global variable defined in different files */
extern struct at_param_list at_param_list;
extern char rsp_buf[CONFIG_SLM_SOCKET_RX_MAX * 2];
extern uint8_t rx_data[CONFIG_SLM_SOCKET_RX_MAX];

#if defined(CONFIG_SLM_CUSTOMIZED_RS232)
extern const struct device *gpio_dev;
#endif

extern int poweron_uart(bool sync_str);

/** forward declaration of thread function **/
static void tcpcli_thread_func(void *p1, void *p2, void *p3);
static void tcpsvr_thread_func(void *p1, void *p2, void *p3);

static int do_tcp_server_start(uint16_t port)
{
	int ret = 0;
	struct sockaddr_in local;
	int addr_len;
	char ipv4_addr[NET_IPV4_ADDR_LEN];
	int reuseaddr = 1;

#if defined(CONFIG_SLM_NATIVE_TLS)
	if (proxy.sec_tag != INVALID_SEC_TAG) {
		ret = slm_tls_loadcrdl(proxy.sec_tag);
		if (ret < 0) {
			LOG_ERR("Fail to load credential: %d", ret);
			proxy.sec_tag = INVALID_SEC_TAG;
			goto exit;
		}
	}
#else
#if !SLM_TCP_PROXY_FUTURE_FEATURE
/* TLS server not officially supported by modem yet */
	if (proxy.sec_tag != INVALID_SEC_TAG) {
		LOG_ERR("Not supported");
		ret = -EINVAL;
		goto exit;
	}
#endif
#endif
	/* Open socket */
	if (proxy.sec_tag == INVALID_SEC_TAG) {
		proxy.sock = socket(AF_INET, SOCK_STREAM | SOCK_NATIVE, IPPROTO_TCP);
	} else {
		proxy.sock = socket(AF_INET, SOCK_STREAM | SOCK_NATIVE, IPPROTO_TLS_1_2);
	}
	if (proxy.sock < 0) {
		LOG_ERR("socket() failed: %d", -errno);
		ret = -errno;
		goto exit;
	}

	if (proxy.sec_tag != INVALID_SEC_TAG) {
		sec_tag_t sec_tag_list[1] = { proxy.sec_tag };

		ret = setsockopt(proxy.sock, SOL_TLS, TLS_SEC_TAG_LIST,
				sec_tag_list, sizeof(sec_tag_t));
		if (ret) {
			LOG_ERR("set tag list failed: %d", -errno);
			ret = -errno;
			goto exit;
		}
	}

	/* Allow reuse of local addresses */
	ret = setsockopt(proxy.sock, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(int));
	if (ret < 0) {
		LOG_ERR("set reuse addr failed: %d", -errno);
		ret = -errno;
		goto exit;
	}

	/* Bind to local port */
	local.sin_family = AF_INET;
	local.sin_port = htons(port);
	if (!util_get_ipv4_addr(ipv4_addr)) {
		LOG_ERR("Unable to obtain local IPv4 address");
		ret = -ENETUNREACH;
		goto exit;
	}
	addr_len = strlen(ipv4_addr);
	if (addr_len == 0) {
		LOG_ERR("LTE not connected yet");
		ret = -ENETUNREACH;
		goto exit;
	}
	if (!check_for_ipv4(ipv4_addr, addr_len)) {
		LOG_ERR("Invalid local address");
		ret = -EINVAL;
		goto exit;
	}
	if (inet_pton(AF_INET, ipv4_addr, &local.sin_addr) != 1) {
		LOG_ERR("Parse local IP address failed: %d", -errno);
		ret = -EINVAL;
		goto exit;
	}

	ret = bind(proxy.sock, (struct sockaddr *)&local, sizeof(struct sockaddr_in));
	if (ret) {
		LOG_ERR("bind() failed: %d", -errno);
		ret = -errno;
		goto exit;
	}

	/* Enable listen */
	ret = listen(proxy.sock, 1);
	if (ret < 0) {
		LOG_ERR("listen() failed: %d", -errno);
		ret = -EINVAL;
		goto exit;
	}

	k_thread_create(&tcp_thread, tcp_thread_stack,
			K_THREAD_STACK_SIZEOF(tcp_thread_stack),
			tcpsvr_thread_func, NULL, NULL, NULL,
			THREAD_PRIORITY, K_USER, K_NO_WAIT);
	proxy.role = AT_TCP_ROLE_SERVER;
	sprintf(rsp_buf, "\r\n#XTCPSVR: %d,\"started\"\r\n", proxy.sock);
	rsp_send(rsp_buf, strlen(rsp_buf));

exit:
	if (ret < 0) {
		if (proxy.sock != INVALID_SOCKET) {
			close(proxy.sock);
		}
#if defined(CONFIG_SLM_NATIVE_TLS)
		if (proxy.sec_tag != INVALID_SEC_TAG) {
			if (slm_tls_unloadcrdl(proxy.sec_tag) != 0) {
				LOG_ERR("Fail to unload credential");
			}
		}
#endif
		slm_at_tcp_proxy_init();
		sprintf(rsp_buf, "\r\n#XTCPSVR: %d\r\n", ret);
		rsp_send(rsp_buf, strlen(rsp_buf));
	}

	return ret;
}

static int do_tcp_server_stop(void)
{
	int ret;

	if (proxy.sock == INVALID_SOCKET) {
		LOG_WRN("Proxy server is not running");
		return -EINVAL;
	}
	ret = close(proxy.sock);
	if (ret) {
		LOG_WRN("Close socket fails: %d", ret);
	} else {
#if defined(CONFIG_SLM_CUSTOMIZED)
		k_thread_join(&tcp_thread, K_SECONDS(CONFIG_SLM_TCP_POLL_TIME * 2));
#endif
	}

	return ret;
}

static int do_tcp_client_connect(const char *url,
				 const char *hostname,
				 uint16_t port)
{
	int ret;

	/* Open socket */
	if (proxy.sec_tag == INVALID_SEC_TAG) {
		proxy.sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	} else {
		proxy.sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);

	}
	if (proxy.sock < 0) {
		LOG_ERR("socket() failed: %d", -errno);
		ret = -errno;
		goto exit;
	}
	if (proxy.sec_tag != INVALID_SEC_TAG) {
		sec_tag_t sec_tag_list[1] = { proxy.sec_tag };

		ret = setsockopt(proxy.sock, SOL_TLS, TLS_SEC_TAG_LIST,
				sec_tag_list, sizeof(sec_tag_t));
		if (ret) {
			LOG_ERR("set tag list failed: %d", -errno);
			ret = -errno;
			goto exit;
		}
	}

	/* Connect to remote host */
	if (check_for_ipv4(url, strlen(url))) {
		remote.sin_family = AF_INET;
		remote.sin_port = htons(port);
		LOG_DBG("IPv4 Address %s", log_strdup(url));
		/* NOTE inet_pton() returns 1 as success */
		ret = inet_pton(AF_INET, url, &remote.sin_addr);
		if (ret != 1) {
			LOG_ERR("inet_pton() failed: %d", ret);
			ret = -EINVAL;
			goto exit;
		}
	} else {
		struct addrinfo *result;
		struct addrinfo hints = {
			.ai_family = AF_INET,
			.ai_socktype = SOCK_STREAM
		};

		ret = getaddrinfo(url, NULL, &hints, &result);
		if (ret || result == NULL) {
			LOG_ERR("getaddrinfo() failed: %d", ret);
			ret = -EINVAL;
			goto exit;
		}

		remote.sin_family = AF_INET;
		remote.sin_port = htons(port);
		remote.sin_addr.s_addr =
		((struct sockaddr_in *)result->ai_addr)->sin_addr.s_addr;
		/* Free the address. */
		freeaddrinfo(result);
	}

#if defined(CONFIG_SLM_DIAG)
	/* Clear connection fail */
	slm_diag_clear_event(SLM_DIAG_DATA_CONNECTION_FAIL);
	/* Clear call fail */
	slm_diag_clear_event(SLM_DIAG_CALL_FAIL);
#endif
	if (proxy.sec_tag != INVALID_SEC_TAG) {
		if (strlen(hostname) > 0) {
			ret = setsockopt(proxy.sock, SOL_TLS,
					 TLS_HOSTNAME, hostname,
					 strlen(hostname));
		} else {
			ret = setsockopt(proxy.sock, SOL_TLS,
					 TLS_HOSTNAME, url, strlen(url));
		}
		if (ret < 0) {
			LOG_ERR("Failed to set TLS_HOSTNAME");
			goto exit;
		}
	}
	ret = connect(proxy.sock, (struct sockaddr *)&remote,
		sizeof(struct sockaddr_in));
	if (ret < 0) {
		LOG_ERR("connect() failed: %d", -errno);
		ret = -errno;
		goto exit;
	}
#if defined(CONFIG_SLM_CUSTOMIZED_RS232)
	/* Activate DCD pin */
	ret = gpio_pin_set_raw(gpio_dev, CONFIG_SLM_DCD_PIN, 0);
	if (ret) {
		LOG_ERR("Cannot activate DCD pin");
		goto exit;
	}
#endif
	k_thread_create(&tcp_thread, tcp_thread_stack,
			K_THREAD_STACK_SIZEOF(tcp_thread_stack),
			tcpcli_thread_func, NULL, NULL, NULL,
			THREAD_PRIORITY, K_USER, K_NO_WAIT);

	proxy.role = AT_TCP_ROLE_CLIENT;
	sprintf(rsp_buf, "\r\n#XTCPCLI: %d,\"connected\"\r\n", proxy.sock);
	rsp_send(rsp_buf, strlen(rsp_buf));

exit:
	if (ret < 0) {
		if (proxy.sock != INVALID_SOCKET) {
			close(proxy.sock);
		}
		slm_at_tcp_proxy_init();
		sprintf(rsp_buf, "\r\n#XTCPCLI: %d\r\n", ret);
		rsp_send(rsp_buf, strlen(rsp_buf));
#if defined(CONFIG_SLM_DIAG)
		/* Fail to create conntion */
		slm_diag_set_event(SLM_DIAG_DATA_CONNECTION_FAIL);
#endif
	}

	return ret;
}

static int do_tcp_client_disconnect(void)
{
	int ret;

	if (proxy.sock == INVALID_SOCKET) {
		LOG_WRN("Client is not running");
		return -EINVAL;
	}
	ret = close(proxy.sock);
	if (ret) {
		LOG_WRN("Close socket fails: %d", ret);
	} else {
#if defined(CONFIG_SLM_CUSTOMIZED)
		k_thread_join(&tcp_thread, K_SECONDS(CONFIG_SLM_TCP_POLL_TIME));
#endif
	}

	return 0;
}

static int do_tcp_send(const uint8_t *data, int datalen)
{
	int ret = 0;
	uint32_t offset = 0;
	int sock;

	if (proxy.role == AT_TCP_ROLE_CLIENT && proxy.sock != INVALID_SOCKET) {
		sock = proxy.sock;
	} else if (proxy.role == AT_TCP_ROLE_SERVER && proxy.sock_peer != INVALID_SOCKET) {
		sock = proxy.sock_peer;
#if defined(CONFIG_SLM_CUSTOMIZED)
		k_timer_stop(&conn_timer);
#endif
	} else {
		LOG_ERR("Not connected yet");
		return -EINVAL;
	}

	while (offset < datalen) {
		ret = send(sock, data + offset, datalen - offset, 0);
		if (ret < 0) {
			LOG_ERR("send() failed: %d", -errno);
			sprintf(rsp_buf, "\r\n#XTCPSEND: %d\r\n", -errno);
			rsp_send(rsp_buf, strlen(rsp_buf));
			if (errno != EAGAIN && errno != ETIMEDOUT) {
				if (proxy.role == AT_TCP_ROLE_CLIENT) {
					do_tcp_client_disconnect();
				} else {
					k_work_submit(&disconnect_work);
				}
			}
			ret = -errno;
			break;
		}
		offset += ret;
	}

	if (ret >= 0) {
		sprintf(rsp_buf, "\r\n#XTCPSEND: %d\r\n", offset);
		rsp_send(rsp_buf, strlen(rsp_buf));
#if defined(CONFIG_SLM_CUSTOMIZED)
		/* restart activity timer */
		if (proxy.role == AT_TCP_ROLE_SERVER) {
			k_timer_start(&conn_timer, K_SECONDS(proxy.timeout),
				      K_NO_WAIT);
		}
#endif
#if defined(CONFIG_SLM_UI)
		if (offset > 0) {
			if (offset < NET_IPV4_MTU/3) {
				ui_led_set_state(LED_ID_DATA, UI_DATA_SLOW);
			} else if (offset < 2*NET_IPV4_MTU/3) {
				ui_led_set_state(LED_ID_DATA, UI_DATA_NORMAL);
			} else {
				ui_led_set_state(LED_ID_DATA, UI_DATA_FAST);
			}
		}
#endif
		return 0;
	} else {
		return ret;
	}
}

static int do_tcp_send_datamode(const uint8_t *data, int datalen)
{
	int ret = 0;
	uint32_t offset = 0;
	int sock;

	if (proxy.role == AT_TCP_ROLE_CLIENT && proxy.sock != INVALID_SOCKET) {
		sock = proxy.sock;
	} else if (proxy.role == AT_TCP_ROLE_SERVER && proxy.sock_peer != INVALID_SOCKET) {
		sock = proxy.sock_peer;
#if defined(CONFIG_SLM_CUSTOMIZED)
		k_timer_stop(&conn_timer);
#endif
	} else {
		LOG_ERR("Not connected yet");
		return -EINVAL;
	}

	while (offset < datalen) {
		ret = send(sock, data + offset, datalen - offset, 0);
		if (ret < 0) {
			LOG_ERR("send() failed: %d", -errno);
			if (errno != EAGAIN && errno != ETIMEDOUT) {
				if (proxy.role == AT_TCP_ROLE_CLIENT) {
					do_tcp_client_disconnect();
				} else {
					k_work_submit(&disconnect_work);
				}
			}
#if defined(CONFIG_SLM_CUSTOMIZED)
			ret = -errno;
#endif
			break;
		}
		offset += ret;
	}

#if defined(CONFIG_SLM_CUSTOMIZED)
	if (ret >= 0) {
		/* restart activity timer */
		if (proxy.role == AT_TCP_ROLE_SERVER) {
			k_timer_start(&conn_timer, K_SECONDS(proxy.timeout), K_NO_WAIT);
		}
	}

#endif
#if defined(CONFIG_SLM_UI)
	if (offset > 0) {
		if (offset < NET_IPV4_MTU/3) {
			ui_led_set_state(LED_ID_DATA, UI_DATA_SLOW);
		} else if (offset < 2*NET_IPV4_MTU/3) {
			ui_led_set_state(LED_ID_DATA, UI_DATA_NORMAL);
		} else {
			ui_led_set_state(LED_ID_DATA, UI_DATA_FAST);
		}
	}
#endif

	return (offset > 0) ? offset : -1;
}

static int tcp_data_save(uint8_t *data, uint32_t length)
{
	if (ring_buf_space_get(&data_buf) < length) {
		return -1; /* RX overrun */
	}

	return ring_buf_put(&data_buf, data, length);
}

static void tcp_data_handle(uint8_t *data, uint32_t length)
{
	int ret;

#if defined(CONFIG_SLM_UI)
	if (length < NET_IPV4_MTU/3) {
		ui_led_set_state(LED_ID_DATA, UI_DATA_SLOW);
	} else if (length < 2*NET_IPV4_MTU/3) {
		ui_led_set_state(LED_ID_DATA, UI_DATA_NORMAL);
	} else {
		ui_led_set_state(LED_ID_DATA, UI_DATA_FAST);
	}
#endif

	if (proxy.role == AT_TCP_ROLE_CLIENT || tcpsvr_state == TCPSVR_CONNECTED) {
		if (proxy.datamode) {
			rsp_send(data, length);
		} else if (slm_util_hex_check(data, length)) {
			uint8_t data_hex[length * 2];

			ret = slm_util_htoa(data, length, data_hex, length * 2);
			if (ret < 0) {
				LOG_ERR("hex convert error: %d", ret);
				return;
			}

			int err = tcp_data_save(data_hex, ret);
			if (err < 0) {
				LOG_ERR("TCP data buffer overflow");
			} else {
				sprintf(rsp_buf, "\r\n#XTCPDATA: %d,%d\r\n", DATATYPE_HEXADECIMAL,
					ret);
				rsp_send(rsp_buf, strlen(rsp_buf));
			}
		} else {
			ret = tcp_data_save(data, length);
			if (ret < 0) {
				LOG_ERR("TCP data buffer overflow");
			} else if (tcpsvr_state == TCPSVR_CONNECTED) {
				sprintf(rsp_buf, "\r\n#XTCPDATA: %d,%d\r\n", DATATYPE_PLAINTEXT,
					length);
				rsp_send(rsp_buf, strlen(rsp_buf));
			}
		}
	} else {
#if defined(CONFIG_SLM_CUSTOMIZED)
		if (!proxy.datamode && slm_util_hex_check(data, length)) {
#else
		if (slm_util_hex_check(data, length)) {
#endif
			uint8_t data_hex[length * 2];

			ret = slm_util_htoa(data, length, data_hex, length * 2);
			if (ret < 0) {
				LOG_ERR("hex convert error: %d", ret);
				return;
			}
			ret = tcp_data_save(data_hex, ret);
			if (ret < 0) {
				LOG_ERR("TCP data buffer overflow");
			}
		} else {
			ret = tcp_data_save(data, length);
			if (ret < 0) {
				LOG_ERR("TCP data buffer overflow");
			}
		}
	}

}

static void tcp_terminate_connection(int cause)
{
#if defined(CONFIG_SLM_CUSTOMIZED_RS232)
	int err = 0;
#endif
#if defined(CONFIG_SLM_CUSTOMIZED)
	k_timer_stop(&conn_timer);
#endif
	if (proxy.datamode) {
		(void)exit_datamode();
	}
	if (proxy.sock_peer != INVALID_SOCKET) {
		close(proxy.sock_peer);
		proxy.sock_peer = INVALID_SOCKET;
		nfds--;
		/* Send URC for server-initiated disconnect */
		sprintf(rsp_buf, "\r\n#XTCPSVR: %d,\"disconnected\"\r\n", cause);
		rsp_send(rsp_buf, strlen(rsp_buf));
#if defined(CONFIG_SLM_CUSTOMIZED_RS232)
		/* De-activate DCD pin */
		err = gpio_pin_set_raw(gpio_dev, CONFIG_SLM_DCD_PIN, 1);
		if (err) {
			LOG_ERR("Cannot de-activate DCD pin");
		}
#endif
		tcpsvr_state = TCPSVR_INIT;
	}
}

static void terminate_connection_wk(struct k_work *work)
{
	ARG_UNUSED(work);

	tcp_terminate_connection(-ENETDOWN);
}

static int tcp_datamode_callback(uint8_t op, const uint8_t *data, int len)
{
	int ret = 0;

	if (op == DATAMODE_SEND) {
		ret = do_tcp_send_datamode(data, len);
		LOG_DBG("datamode send: %d", ret);
	} else if (op == DATAMODE_EXIT) {
		if (proxy.role == AT_TCP_ROLE_CLIENT) {
			proxy.datamode = false;
		}
		if (proxy.role == AT_TCP_ROLE_SERVER && proxy.sock_peer != INVALID_SOCKET) {
			k_work_submit(&disconnect_work);
		}
	}

	return ret;
}

static int tcpsvr_input(int infd)
{
	int ret;

	if (fds[infd].fd == proxy.sock) {
		socklen_t len = sizeof(struct sockaddr_in);
		char peer_addr[INET_ADDRSTRLEN];
		bool filtered = true;

		/* If server auto-accept is on, accept this connection.
		 * Otherwise, accept the connection according to AT#TCPSVRAR
		 */
		if (proxy.aa == AT_TCP_SVR_AA_OFF) {
			if (proxy.ar == AT_TCP_SVR_AR_UNKNOWN) {
				proxy.ar = AT_TCP_SVR_AR_CONNECTING;
				return 0;
			} else if (proxy.ar == AT_TCP_SVR_AR_CONNECTING) {
				return 0;
			} else if (proxy.ar == AT_TCP_SVR_AR_REJECT) {
				proxy.ar = AT_TCP_SVR_AR_UNKNOWN;
				ret = accept(proxy.sock,
					     (struct sockaddr *)&remote, &len);
				if (ret >= 0) {
					close(ret);
				}
				return 0;
			} else if (proxy.ar == AT_TCP_SVR_AR_ACCEPT) {
				proxy.ar = AT_TCP_SVR_AR_UNKNOWN;
			}
		}
		ret = accept(proxy.sock,
				(struct sockaddr *)&remote, &len);
		LOG_DBG("accept(): %d", ret);
		if (ret < 0) {
			LOG_ERR("accept() failed: %d", -errno);
#if defined(CONFIG_SLM_DIAG)
			/* Fail to create conntion */
			slm_diag_set_event(SLM_DIAG_DATA_CONNECTION_FAIL);
#endif
			return -errno;
		}
		if (nfds >= MAX_POLL_FD) {
			LOG_WRN("Full. Close connection.");
			close(ret);
			return -ECONNREFUSED;
		}
		/* Client IPv4 filtering */
		if (inet_ntop(AF_INET, &remote.sin_addr, peer_addr,
			INET_ADDRSTRLEN) == NULL) {
			LOG_ERR("inet_ntop() failed: %d", -errno);
			close(ret);
			return -errno;
		}
		if (proxy.filtermode) {
			for (int i = 0; i < CONFIG_SLM_TCP_FILTER_SIZE; i++) {
				if (strlen(ip_allowlist[i]) > 0 &&
				    strcmp(ip_allowlist[i], peer_addr) == 0) {
					filtered = false;
					break;
				}
			}
			if (filtered) {
				LOG_WRN("Connection filtered");
				close(ret);
				return -ECONNREFUSED;
			}
		}
		proxy.sock_peer = ret;
		LOG_DBG("New connection - %d", proxy.sock_peer);
		fds[nfds].fd = proxy.sock_peer;
		fds[nfds].events = POLLIN;
		nfds++;
		sprintf(rsp_buf, "\r\n#XTCPSVR: \"%s\",\"connected\"\r\n",
			peer_addr);
		k_work_reschedule(&tcpsvr_state_work, K_MSEC(10));
	} else {
#if defined(CONFIG_SLM_CUSTOMIZED)
		k_timer_stop(&conn_timer);
#endif
		ret = recv(fds[infd].fd, (void *)rx_data, sizeof(rx_data), 0);
		if (ret > 0) {
			tcp_data_handle(rx_data, ret);
		}
		if (ret < 0) {
			LOG_WRN("recv() error: %d", -errno);
		}
#if defined(CONFIG_SLM_CUSTOMIZED)
		/* Restart conn timer */
		LOG_DBG("restart timer: POLLIN");
		k_timer_start(&conn_timer, K_SECONDS(proxy.timeout), K_NO_WAIT);
#endif
	}

	return ret;
}

/* TCP server thread */
static void tcpsvr_thread_func(void *p1, void *p2, void *p3)
{
	int ret, current_size;
	bool in_datamode;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	fds[0].fd = proxy.sock;
	fds[0].events = POLLIN;
	nfds = 1;
	ring_buf_reset(&data_buf);
	while (true) {
#if defined(CONFIG_SLM_CUSTOMIZED)
		if (proxy.timeout > 0 &&
			k_timer_status_get(&conn_timer) > 0) {
			LOG_WRN("Connection timeout");
			tcp_terminate_connection(-ETIMEDOUT);
		}
#endif
		ret = poll(fds, nfds, MSEC_PER_SEC * CONFIG_SLM_TCP_POLL_TIME);
		if (ret < 0) {  /* IO error */
			LOG_WRN("poll() error: %d", -errno);
			ret = -EIO;
			goto exit;
		}
		if (ret == 0) {  /* timeout */
			continue;
		}
		current_size = nfds;
		for (int i = 0; i < current_size; i++) {
			LOG_DBG("Poll revents[%d] 0x%08x", i, fds[i].revents);
			if ((fds[i].revents & POLLIN) == POLLIN) {
				ret = tcpsvr_input(i);
				if (ret < 0) {
					LOG_WRN("tcpsvr_input error: %d", ret);
				}
			}
			if ((fds[i].revents & POLLERR) == POLLERR) {
				LOG_INF("POLLERR: %d", i);
				if (fds[i].fd == proxy.sock) {
					ret = -EIO;
					goto exit;
				}
				tcp_terminate_connection(-EIO);
				continue;
			}
			if ((fds[i].revents & POLLHUP) == POLLHUP) {
				LOG_INF("POLLHUP: %d", i);
				if (fds[i].fd == proxy.sock) {
#if defined(CONFIG_SLM_DIAG)
					slm_diag_set_event(SLM_DIAG_CALL_FAIL);
#endif
					ret = -ENETDOWN;
					goto exit;
				}
				if (tcpsvr_state == TCPSVR_CONNECTED) {
					tcp_terminate_connection(-ECONNRESET);
				} else {
					LOG_WRN("Early disconnected");
					k_msleep(1000);
				}
				continue;
			}
			if ((fds[i].revents & POLLNVAL) == POLLNVAL) {
				LOG_INF("POLLNVAL: %d", i);
				if (fds[i].fd == proxy.sock) {
					ret = 0;
					if (proxy.sock_peer != INVALID_SOCKET) {
						tcp_terminate_connection(-ECONNABORTED);
					}
					goto exit;
				}
				tcp_terminate_connection(-ECONNABORTED);
			}
		}
	}
exit:
	if (proxy.sock_peer != INVALID_SOCKET) {
#if defined(CONFIG_SLM_CUSTOMIZED)
		k_timer_stop(&conn_timer);
#endif
		ret = close(proxy.sock_peer);
		if (ret < 0) {
			LOG_WRN("close(%d) fail: %d", proxy.sock_peer, -errno);
		}
	}
	if (proxy.sock != INVALID_SOCKET) {
		ret = close(proxy.sock);
		if (ret < 0) {
			LOG_WRN("close(%d) fail: %d", proxy.sock, -errno);
		}
	}
#if defined(CONFIG_SLM_NATIVE_TLS)
	if (proxy.sec_tag != INVALID_SEC_TAG) {
		ret = slm_tls_unloadcrdl(proxy.sec_tag);
		if (ret < 0) {
			LOG_ERR("Fail to unload credential: %d", ret);
		}
	}
#endif
	in_datamode = proxy.datamode;
	slm_at_tcp_proxy_init();
	sprintf(rsp_buf, "\r\n#XTCPSVR: %d,\"stopped\"\r\n", ret);
	rsp_send(rsp_buf, strlen(rsp_buf));
	if (in_datamode) {
		if (exit_datamode()) {
			sprintf(rsp_buf, "\r\n#XTCPSVR: 0,\"datamode\"\r\n");
			rsp_send(rsp_buf, strlen(rsp_buf));
		}
	}

	LOG_INF("TCP server thread terminated");
}

/* TCP client thread */
static void tcpcli_thread_func(void *p1, void *p2, void *p3)
{
	int ret;
#if defined(CONFIG_SLM_DIAG)
	int nw_reg_1 = 0, nw_reg_2 = 0;
#endif
	bool in_datamode;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	fds[0].fd = proxy.sock;
	fds[0].events = POLLIN;
	ring_buf_reset(&data_buf);
	while (true) {
		ret = poll(&fds[0], 1, MSEC_PER_SEC * CONFIG_SLM_TCP_POLL_TIME);
		if (ret < 0) {  /* IO error */
			LOG_WRN("poll() error: %d", ret);
			ret = -EIO;
			goto exit;
		}
		if (ret == 0) {  /* timeout */
			continue;
		}
		LOG_DBG("Poll events 0x%08x", fds[0].revents);
		if ((fds[0].revents & POLLERR) == POLLERR) {
			LOG_ERR("POLLERR");
			ret = -EIO;
			goto exit;
		}
		if ((fds[0].revents & POLLNVAL) == POLLNVAL) {
			LOG_INF("TCP client disconnected.");
			proxy.sock = INVALID_SOCKET;
			goto exit;
		}
		if ((fds[0].revents & POLLHUP) == POLLHUP) {
			LOG_INF("Peer disconnect: %d", fds[0].fd);
			goto exit;
		}
		if ((fds[0].revents & POLLIN) == POLLIN) {
			ret = recv(fds[0].fd, (void *)rx_data, sizeof(rx_data), 0);
			if (ret < 0) {
				LOG_WRN("recv() error: %d", -errno);
				continue;
			}
			if (ret == 0) {
				continue;
			}
			tcp_data_handle(rx_data, ret);
		}
	}
exit:
#if defined(CONFIG_SLM_DIAG)
	/* Workaround to check nw status changes and disconnection */
	nw_reg_1 = slm_stats_get_nw_reg_status();
	k_sleep(K_MSEC(10));
	nw_reg_2 = slm_stats_get_nw_reg_status();
	if (nw_reg_1 != nw_reg_2) {
		slm_diag_set_event(SLM_DIAG_CALL_FAIL);
	}
#endif
	if (proxy.sock != INVALID_SOCKET) {
		ret = close(proxy.sock);
		if (ret < 0) {
			LOG_WRN("close(%d) fail: %d", proxy.sock, -errno);
		}
	}
	in_datamode = proxy.datamode;
	slm_at_tcp_proxy_init();
	sprintf(rsp_buf, "\r\n#XTCPCLI: %d,\"disconnected\"\r\n", ret);
	rsp_send(rsp_buf, strlen(rsp_buf));
	if (in_datamode) {
		if (exit_datamode()) {
			sprintf(rsp_buf, "\r\n#XTCPCLI: 0,\"datamode\"\r\n");
			rsp_send(rsp_buf, strlen(rsp_buf));
		}
	}
#if defined(CONFIG_SLM_CUSTOMIZED_RS232)
	/* De-activate DCD pin */
	ret = gpio_pin_set_raw(gpio_dev, CONFIG_SLM_DCD_PIN, 1);
	if (ret) {
		LOG_ERR("Cannot de-activate DCD pin");
	}
#endif
	LOG_INF("TCP client thread terminated");
}

/**@brief handle AT#XTCPFILTER commands
 *  AT#XTCPFILTER=<op>[,<IP_ADDR#1>[,<IP_ADDR#2>[,...]]]
 *  AT#XTCPFILTER?
 *  AT#XTCPFILTER=?
 */
int handle_at_tcp_filter(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;
	uint16_t op;
	int param_count = at_params_valid_count_get(&at_param_list);

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
		err = at_params_unsigned_short_get(&at_param_list, 1, &op);
		if (err) {
			return err;
		}
		if (op == AT_FILTER_SET) {
			char address[INET_ADDRSTRLEN];
			int size;

			if (param_count > (CONFIG_SLM_TCP_FILTER_SIZE + 2)) {
				return -EINVAL;
			}
			memset(ip_allowlist, 0x00, sizeof(ip_allowlist));
			for (int i = 2; i < param_count; i++) {
				size = INET_ADDRSTRLEN;
				err = util_string_get(&at_param_list, i, address, &size);
				if (err) {
					return err;
				}
				if (!check_for_ipv4(address, size)) {
					return -EINVAL;
				}
				memcpy(ip_allowlist[i - 2], address, size);
			}
			proxy.filtermode = true;
			err = 0;
		} else if (op == AT_FILTER_CLEAR) {
			memset(ip_allowlist, 0x00, sizeof(ip_allowlist));
			proxy.filtermode = false;
			err = 0;
		} break;

	case AT_CMD_TYPE_READ_COMMAND:
		sprintf(rsp_buf, "\r\n#XTCPFILTER: %d", proxy.filtermode);
		for (int i = 0; i < CONFIG_SLM_TCP_FILTER_SIZE; i++) {
			if (strlen(ip_allowlist[i]) > 0) {
				strcat(rsp_buf, ",\"");
				strcat(rsp_buf, ip_allowlist[i]);
				strcat(rsp_buf, "\"");
			}
		}
		strcat(rsp_buf, "\r\n");
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	case AT_CMD_TYPE_TEST_COMMAND:
		sprintf(rsp_buf, "\r\n#XTCPFILTER: (%d,%d)",
			AT_FILTER_CLEAR, AT_FILTER_SET);
		strcat(rsp_buf, ",<IP_ADDR#1>[,<IP_ADDR#2>[,...]]\r\n");
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	default:
		break;
	}

	return err;
}

#if defined(CONFIG_SLM_CUSTOMIZED)
/**@brief handle AT#XTCPSVR commands
 *  AT#XTCPSVR=<op>[,<port>,<timeout>,[sec_tag]]
 *  AT#XTCPSVR?
 *  AT#XTCPSVR=?
 */
#else
/**@brief handle AT#XTCPSVR commands
 *  AT#XTCPSVR=<op>[,<port>[,[sec_tag]]
 *  AT#XTCPSVR?
 *  AT#XTCPSVR=?
 */
#endif
int handle_at_tcp_server(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;
	uint16_t op;
	uint16_t port;
	int param_count = at_params_valid_count_get(&at_param_list);

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
		err = at_params_unsigned_short_get(&at_param_list, 1, &op);
		if (err) {
			return err;
		}
		if (op == AT_SERVER_START || op == AT_SERVER_START_WITH_DATAMODE) {
			if (proxy.sock != INVALID_SOCKET) {
				LOG_ERR("Server is already running.");
				return -EINVAL;
			}
			err = at_params_unsigned_short_get(&at_param_list, 2, &port);
			if (err) {
				return err;
			}
#if defined(CONFIG_SLM_CUSTOMIZED)
			if (param_count > 3) {
				err = at_params_unsigned_short_get(&at_param_list, 3,
								   &proxy.timeout);
				if (err) {
					return err;
				}
			}
			if (param_count > 4) {
				at_params_unsigned_int_get(&at_param_list, 4, &proxy.sec_tag);
			}
#else
			if (param_count > 3) {
				at_params_unsigned_int_get(&at_param_list, 3, &proxy.sec_tag);
			}
#endif
#if defined(CONFIG_SLM_DATAMODE_HWFC)
			if (op == AT_SERVER_START_WITH_DATAMODE && !check_uart_flowcontrol()) {
				LOG_ERR("Data mode requires HWFC.");
				return -EINVAL;
			}
#endif
			err = do_tcp_server_start((uint16_t)port);
			if (err == 0 && op == AT_SERVER_START_WITH_DATAMODE) {
				proxy.datamode = true;
			}
		} else if (op == AT_SERVER_STOP) {
			err = do_tcp_server_stop();
		} break;

	case AT_CMD_TYPE_READ_COMMAND:
#if defined(CONFIG_SLM_CUSTOMIZED)
		if (proxy.sock != INVALID_SOCKET &&
		    proxy.role == AT_TCP_ROLE_SERVER) {
			sprintf(rsp_buf, "\r\n#XTCPSVR: %d,%d,%d,%d\r\n",
				proxy.sock, proxy.sock_peer, proxy.timeout, proxy.datamode);
		} else {
			sprintf(rsp_buf, "\r\n#XTCPSVR: %d,%d\r\n",
				INVALID_SOCKET, INVALID_SOCKET);
		}
#else
		sprintf(rsp_buf, "\r\n#XTCPSVR: %d,%d,%d\r\n",
			proxy.sock, proxy.sock_peer, proxy.datamode);
#endif
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	case AT_CMD_TYPE_TEST_COMMAND:
#if defined(CONFIG_SLM_CUSTOMIZED)
		sprintf(rsp_buf, "\r\n#XTCPSVR: (%d,%d,%d),<port>,<timeout>,<sec_tag>\r\n",
			AT_SERVER_STOP, AT_SERVER_START,
			AT_SERVER_START_WITH_DATAMODE);
#else
		sprintf(rsp_buf, "\r\n#XTCPSVR: (%d,%d,%d),<port>,<sec_tag>\r\n",
			AT_SERVER_STOP, AT_SERVER_START, AT_SERVER_START_WITH_DATAMODE);
#endif
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	default:
		break;
	}

	return err;
}

/**@brief handle AT#XTCPSVRAA commands
 *  AT#XTCPSVRAA=<op>
 *  AT#XTCPSVRAA?
 *  AT#XTCPSVRAA=?
 */
int handle_at_tcp_server_auto_accept(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;
	uint16_t op;

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
		err = at_params_short_get(&at_param_list, 1, &op);
		if (err) {
			return err;
		}
		if (op != AT_TCP_SVR_AA_OFF && op != AT_TCP_SVR_AA_ON) {
			return err;
		}
		proxy.aa = op;
		err = 0;
		break;

	case AT_CMD_TYPE_READ_COMMAND:
		sprintf(rsp_buf, "\r\n#XTCPSVRAA: %d\r\n", proxy.aa);
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	case AT_CMD_TYPE_TEST_COMMAND:
		sprintf(rsp_buf, "\r\n#XTCPSVRAA: (%d,%d)\r\n",
			AT_TCP_SVR_AA_OFF, AT_TCP_SVR_AA_ON);
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	default:
		break;
	}

	return err;
}

/**@brief handle AT#XTCPSVRAR commands
 *  AT#XTCPSVRAR=<op>
 *  AT#XTCPSVRAR? READ command not supported
 *  AT#XTCPSVRAR=?
 */
int handle_at_tcp_server_accept_reject(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;
	uint16_t op;

	if (proxy.ar != AT_TCP_SVR_AR_CONNECTING) {
		return err;
	}

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
		err = at_params_short_get(&at_param_list, 1, &op);
		if (err) {
			return err;
		}
		if (op != AT_TCP_SVR_AR_ACCEPT && op != AT_TCP_SVR_AR_REJECT) {
			return err;
		}
		proxy.ar = op;
		err = 0;
		break;

	case AT_CMD_TYPE_READ_COMMAND:
		break;

	case AT_CMD_TYPE_TEST_COMMAND:
		sprintf(rsp_buf, "\r\n#XTCPSVRAR: (%d,%d)\r\n",
			AT_TCP_SVR_AR_REJECT, AT_TCP_SVR_AR_ACCEPT);
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	default:
		break;
	}

	return err;
}

/**@brief handle AT#XTCPCLI commands
 *  AT#XTCPCLI=<op>[,<url>,<port>[,[sec_tag],[hostname]]
 *  AT#XTCPCLI?
 *  AT#XTCPCLI=?
 */
int handle_at_tcp_client(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;
	uint16_t op;
	int param_count = at_params_valid_count_get(&at_param_list);

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
		err = at_params_unsigned_short_get(&at_param_list, 1, &op);
		if (err) {
			return err;
		}
		if (op == AT_CLIENT_CONNECT || op == AT_CLIENT_CONNECT_WITH_DATAMODE) {
			uint16_t port;
			char url[TCPIP_MAX_URL];
			char hostname[TCPIP_MAX_URL];
			int size = TCPIP_MAX_URL;
			int hn_size = TCPIP_MAX_URL;

			memset(url, 0, sizeof(url));
			memset(hostname, 0, sizeof(hostname));
			if (proxy.sock != INVALID_SOCKET) {
				LOG_ERR("Client is already running.");
				return -EINVAL;
			}
			err = util_string_get(&at_param_list, 2, url, &size);
			if (err) {
				return err;
			}
			err = at_params_unsigned_short_get(&at_param_list, 3, &port);
			if (err) {
				return err;
			}
			if (param_count > 4) {
				at_params_unsigned_int_get(&at_param_list, 4, &proxy.sec_tag);
			}
			if (param_count > 5) {
				err = util_string_get(&at_param_list, 5, hostname, &hn_size);
				if (err) {
					return err;
				}
			}
#if defined(CONFIG_SLM_DATAMODE_HWFC)
			if (op == AT_CLIENT_CONNECT_WITH_DATAMODE && !check_uart_flowcontrol()) {
				LOG_ERR("Data mode requires HWFC.");
				return -EINVAL;
			}
#endif
			err = do_tcp_client_connect(url, hostname, (uint16_t)port);
			if (err == 0 && op == AT_CLIENT_CONNECT_WITH_DATAMODE) {
				proxy.datamode = true;
				enter_datamode(tcp_datamode_callback);
			}
		} else if (op == AT_CLIENT_DISCONNECT) {
			err = do_tcp_client_disconnect();
		} break;

	case AT_CMD_TYPE_READ_COMMAND:
		sprintf(rsp_buf, "\r\n#XTCPCLI: %d,%d\r\n", proxy.sock, proxy.datamode);
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	case AT_CMD_TYPE_TEST_COMMAND:
		sprintf(rsp_buf, "\r\n#XTCPCLI: (%d,%d,%d),<url>,<port>,<sec_tag>,<hostname>\r\n",
			AT_CLIENT_DISCONNECT, AT_CLIENT_CONNECT, AT_CLIENT_CONNECT_WITH_DATAMODE);
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	default:
		break;
	}

	return err;
}

/**@brief handle AT#XTCPSEND commands
 *  AT#XTCPSEND=<datatype>,<data>
 *  AT#XTCPSEND? READ command not supported
 *  AT#XTCPSEND=? TEST command not supported
 */
int handle_at_tcp_send(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;
	uint16_t datatype;
	char data[NET_IPV4_MTU];
	int size = NET_IPV4_MTU;

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
		err = at_params_unsigned_short_get(&at_param_list, 1, &datatype);
		if (err) {
			return err;
		}
		err = util_string_get(&at_param_list, 2, data, &size);
		if (err) {
			return err;
		}
		if (datatype == DATATYPE_HEXADECIMAL) {
			uint8_t data_hex[size / 2];

			err = slm_util_atoh(data, size, data_hex, size / 2);
			if (err > 0) {
				err = do_tcp_send(data_hex, err);
			}
		} else {
			err = do_tcp_send(data, size);
		}
		break;

	default:
		break;
	}

	return err;
}

/**@brief handle AT#XTCPRECV commands
 *  AT#XTCPRECV[=<length>]
 *  AT#XTCPRECV? READ command not supported
 *  AT#XTCPRECV=? TEST command not supported
 */
int handle_at_tcp_recv(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;
	uint16_t length = 0;

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
	{
		uint32_t sz_send = 0;

		if (at_params_valid_count_get(&at_param_list) > 1) {
			err = at_params_unsigned_short_get(&at_param_list, 1, &length);
			if (err) {
				return err;
			}
		}
		if (ring_buf_is_empty(&data_buf) == 0) {
			sz_send = ring_buf_get(&data_buf, rsp_buf, sizeof(rsp_buf));
			if (length > 0 && sz_send > length) {
				sz_send = length;
			}
			rsp_send(rsp_buf, sz_send);
			rsp_send("\r\n", 2);
		}
		sprintf(rsp_buf, "\r\n#XTCPRECV: %d\r\n", sz_send);
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
	} break;

	default:
		break;
	}

	return err;
}

static void tcpsvr_state_work_fn(struct k_work *work)
{
	LOG_DBG("Current TCPSVR state: %d", tcpsvr_state);
	switch (tcpsvr_state) {
	case TCPSVR_INIT:
	{
#if defined(CONFIG_SLM_CUSTOMIZED_RS232)
		/* Toggle RI pins for pre-defined duration */
		if (gpio_pin_set(gpio_dev, CONFIG_SLM_RI_PIN, 1) != 0) {
			LOG_ERR("Cannot write RI gpio high");
		}
		tcpsvr_state = TCPSVR_RI_ON;
		k_work_reschedule(&tcpsvr_state_work, K_MSEC(CONFIG_SLM_RI_ON_DURATION));
#else
		tcpsvr_state = TCPSVR_CONNECTING;
		k_work_reschedule(&tcpsvr_state_work, K_MSEC(10));
#endif
	} break;
#if defined(CONFIG_SLM_CUSTOMIZED_RS232)
	case TCPSVR_RI_ON:
	{
		/* Toggle RI pins for pre-defined duration */
		if (gpio_pin_set(gpio_dev, CONFIG_SLM_RI_PIN, 0) != 0) {
			LOG_ERR("Cannot write RI gpio low");
		}
		tcpsvr_state = TCPSVR_RI_OFF;
		k_work_reschedule(&tcpsvr_state_work, K_MSEC(CONFIG_SLM_RI_OFF_DURATION));
	} break;
	case TCPSVR_RI_OFF:
	{
		tcpsvr_state = TCPSVR_POST_RI;
		k_work_reschedule(&tcpsvr_state_work, K_MSEC(CONFIG_SLM_POST_RI_DURATION));
	} break;
	case TCPSVR_POST_RI:
	{
		tcpsvr_state = TCPSVR_CONNECTING;
		k_work_reschedule(&tcpsvr_state_work, K_MSEC(10));
	} break;
#endif /* CONFIG_SLM_CUSTOMIZED_RS232 */
	case TCPSVR_CONNECTING:
	{
#if defined(CONFIG_SLM_CUSTOMIZED_RS232)
		/* Activate DCD pin */
		if (gpio_pin_set_raw(gpio_dev, CONFIG_SLM_DCD_PIN, 0) != 0) {
			LOG_ERR("Cannot activate DCD pin");
			return;
		}
#endif
#if defined(CONFIG_SLM_DIAG)
		/* Clear connection fail */
		slm_diag_clear_event(SLM_DIAG_DATA_CONNECTION_FAIL);
		/* Clear call fail */
		slm_diag_clear_event(SLM_DIAG_CALL_FAIL);
#endif
		if (proxy.datamode) {
			enter_datamode(tcp_datamode_callback);
		}
		rsp_send(rsp_buf, strlen(rsp_buf));
#if defined(CONFIG_SLM_CUSTOMIZED)
		/* Start a one-shot timer to close the connection */
		k_timer_start(&conn_timer, K_SECONDS(proxy.timeout), K_NO_WAIT);
#endif
		tcpsvr_state = TCPSVR_CONNECTED;

		uint32_t size = ring_buf_capacity_get(&data_buf) - ring_buf_space_get(&data_buf);

		if (size > 0) {
			if (proxy.datamode) {
				size = ring_buf_get(&data_buf, rsp_buf, sizeof(rsp_buf));
				rsp_send(rsp_buf, size);
			} else {
				/* there could be multiple receiving data so set arbitrary type */
				sprintf(rsp_buf, "\r\n#XTCPDATA: %d,%d\r\n", DATATYPE_ARBITRARY,
					size);
				rsp_send(rsp_buf, strlen(rsp_buf));
			}
		}
	} break;

	default:
		break;
	}
	LOG_DBG("New TCPSVR state: %d", tcpsvr_state);
}

/**@brief API to initialize TCP proxy AT commands handler
 */
int slm_at_tcp_proxy_init(void)
{
	proxy.sock = INVALID_SOCKET;
	proxy.sock_peer = INVALID_SOCKET;
	proxy.role = INVALID_ROLE;
	proxy.datamode = false;
	proxy.aa = AT_TCP_SVR_AA_ON;
	proxy.ar = AT_TCP_SVR_AR_UNKNOWN;
#if defined(CONFIG_SLM_CUSTOMIZED)
	proxy.timeout = CONFIG_SLM_TCP_CONN_TIME;
#endif
	proxy.sec_tag = INVALID_SEC_TAG;
	nfds = 0;
	for (int i = 0; i < MAX_POLL_FD; i++) {
		fds[i].fd = INVALID_SOCKET;
	}
	memset(ip_allowlist, 0x00, sizeof(ip_allowlist));
	proxy.filtermode = false;
	k_work_init(&disconnect_work, terminate_connection_wk);
	tcpsvr_state = 0;
	k_work_init_delayable(&tcpsvr_state_work, tcpsvr_state_work_fn);

	return 0;
}

/**@brief API to uninitialize TCP proxy AT commands handler
 */
int slm_at_tcp_proxy_uninit(void)
{
	if (proxy.role == AT_TCP_ROLE_CLIENT) {
		return do_tcp_client_disconnect();
	}
	if (proxy.role == AT_TCP_ROLE_SERVER) {
		return do_tcp_server_stop();
	}

	return 0;
}
