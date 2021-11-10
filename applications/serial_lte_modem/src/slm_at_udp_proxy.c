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
#include <net/tls_credentials.h>
#include "slm_util.h"
#include "slm_at_host.h"
#include "slm_at_udp_proxy.h"
#if defined(CONFIG_SLM_UI)
#include "slm_ui.h"
#endif

LOG_MODULE_REGISTER(udp_proxy, CONFIG_SLM_LOG_LEVEL);

#define THREAD_STACK_SIZE	KB(4)
#define THREAD_PRIORITY		K_LOWEST_APPLICATION_THREAD_PRIO

/*
 * Known limitation in this version
 * - Multiple concurrent
 * - Receive more than IPv4 MTU one-time
 * - IPv6 support
 * - does not support proxy
 */

/**@brief Proxy operations. */
enum slm_udp_proxy_operation {
	SERVER_STOP,
	CLIENT_DISCONNECT = SERVER_STOP,
	SERVER_START,
	CLIENT_CONNECT = SERVER_START,
	SERVER_START_WITH_DATAMODE,
	CLIENT_CONNECT_WITH_DATAMODE = SERVER_START_WITH_DATAMODE,
	SERVER_START6 ,
	CLIENT_CONNECT6 = SERVER_START6,
	SERVER_START6_WITH_DATAMODE,
	CLIENT_CONNECT6_WITH_DATAMODE = SERVER_START6_WITH_DATAMODE,
};

static struct k_thread udp_thread;
static K_THREAD_STACK_DEFINE(udp_thread_stack, THREAD_STACK_SIZE);
static k_tid_t udp_thread_id;

static bool udp_datamode;

/**@brief Proxy roles. */
enum slm_udp_role {
	UDP_ROLE_CLIENT,
	UDP_ROLE_SERVER,
	UDP_ROLD_UNSPEC
};

static struct udp_proxy {
	int sock;		/* Socket descriptor. */
	int family;		/* Socket address family */
	sec_tag_t sec_tag;	/* Security tag of the credential */
	enum slm_udp_role role;	/* Client or Server proxy */
	union {			/* remote host */
		struct sockaddr_in remote;   /* IPv4 host */
		struct sockaddr_in6 remote6; /* IPv6 host */
	};
} proxy;


/* global functions defined in different files */
void rsp_send(const uint8_t *str, size_t len);
int enter_datamode(slm_datamode_handler_t handler);
bool check_uart_flowcontrol(void);
bool exit_datamode(void);

/* global variable defined in different files */
extern struct at_param_list at_param_list;
extern char rsp_buf[CONFIG_AT_CMD_RESPONSE_MAX_LEN];
extern uint8_t rx_data[CONFIG_SLM_SOCKET_RX_MAX];

/** forward declaration of thread function **/
static void udp_thread_func(void *p1, void *p2, void *p3);

static int do_udp_server_start(uint16_t port)
{
	int ret = 0;

	/* Open socket */
	proxy.sock = socket(proxy.family, SOCK_DGRAM, IPPROTO_UDP);
	if (proxy.sock < 0) {
		LOG_ERR("socket() failed: %d", -errno);
		sprintf(rsp_buf, "\r\n#XUDPSVR: %d\r\n", -errno);
		rsp_send(rsp_buf, strlen(rsp_buf));
		return -errno;
	}

	/* Bind to local port */

	if (proxy.family == AF_INET) {
		char ipv4_addr[NET_IPV4_ADDR_LEN] = {0};

		util_get_ip_addr(ipv4_addr, NULL);
		if (strlen(ipv4_addr) == 0) {
			LOG_ERR("Unable to obtain local IPv4 address");
			close(proxy.sock);
			return -EAGAIN;
		}

		struct sockaddr_in local = {
			.sin_family = AF_INET,
			.sin_port = htons(port)
		};

		if (inet_pton(AF_INET, ipv4_addr, &local.sin_addr) != 1) {
			LOG_ERR("Parse local IPv4 address failed: %d", -errno);
			close(proxy.sock);
			return -EINVAL;
		}
		ret = bind(proxy.sock, (struct sockaddr *)&local, sizeof(struct sockaddr_in));
	} else {
		char ipv6_addr[NET_IPV6_ADDR_LEN] = {0};

		util_get_ip_addr(NULL, ipv6_addr);
		if (strlen(ipv6_addr) == 0) {
			LOG_ERR("Unable to obtain local IPv6 address");
			close(proxy.sock);
			return -EAGAIN;
		}

		struct sockaddr_in6 local = {
			.sin6_family = AF_INET6,
			.sin6_port = htons(port)
		};

		if (inet_pton(AF_INET6, ipv6_addr, &local.sin6_addr) != 1) {
			LOG_ERR("Parse local IPv6 address failed: %d", -errno);
			close(proxy.sock);
			return -EINVAL;
		}
		ret = bind(proxy.sock, (struct sockaddr *)&local, sizeof(struct sockaddr_in6));
	}

	if (ret) {
		LOG_ERR("bind() failed: %d", -errno);
		sprintf(rsp_buf, "\r\n#XUDPSVR: %d\r\n", -errno);
		rsp_send(rsp_buf, strlen(rsp_buf));
		close(proxy.sock);
		return -errno;
	}

	udp_thread_id = k_thread_create(&udp_thread, udp_thread_stack,
			K_THREAD_STACK_SIZEOF(udp_thread_stack),
			udp_thread_func, NULL, NULL, NULL,
			THREAD_PRIORITY, K_USER, K_NO_WAIT);

	proxy.role = UDP_ROLE_SERVER;
	sprintf(rsp_buf, "\r\n#XUDPSVR: %d,\"started\"\r\n", proxy.sock);
	rsp_send(rsp_buf, strlen(rsp_buf));
	LOG_DBG("UDP server started");

	return ret;
}

static int do_udp_server_stop(int error)
{
	int ret = 0;

	if (proxy.sock != INVALID_SOCKET) {
		ret = close(proxy.sock);
		if (ret < 0) {
			LOG_WRN("close() failed: %d", -errno);
			ret = -errno;
		} else {
			proxy.sock = INVALID_SOCKET;
			if (proxy.family == AF_INET) {
				memset(&proxy.remote, 0, sizeof(struct sockaddr_in));
			} else {
				memset(&proxy.remote6, 0, sizeof(struct sockaddr_in6));
			}
			(void)slm_at_udp_proxy_init();
		}
		sprintf(rsp_buf, "\r\n#XUDPSVR: %d,\"stopped\"\r\n", error);
		rsp_send(rsp_buf, strlen(rsp_buf));
	}

	return ret;
}

static int do_udp_client_connect(const char *url, uint16_t port, int sec_tag)
{
	int ret;

	/* Open socket */
	if (sec_tag == INVALID_SEC_TAG) {
		proxy.sock = socket(proxy.family, SOCK_DGRAM, IPPROTO_UDP);
	} else {
		proxy.sock = socket(proxy.family, SOCK_DGRAM, IPPROTO_DTLS_1_2);

	}
	if (proxy.sock < 0) {
		LOG_ERR("socket() failed: %d", -errno);
		sprintf(rsp_buf, "\r\n#XUDPCLI: %d\r\n", -errno);
		rsp_send(rsp_buf, strlen(rsp_buf));
		ret = -errno;
		goto exit;
	}
	if (sec_tag != INVALID_SEC_TAG) {
		sec_tag_t sec_tag_list[1] = { sec_tag };

		ret = setsockopt(proxy.sock, SOL_TLS, TLS_SEC_TAG_LIST,
				sec_tag_list, sizeof(sec_tag_t));
		if (ret) {
			LOG_ERR("set tag list failed: %d", -errno);
			sprintf(rsp_buf, "\r\n#XUDPCLI: %d\r\n", -errno);
			rsp_send(rsp_buf, strlen(rsp_buf));
			ret = -errno;
			goto exit;
		}
	}

	/* Connect to remote host */
	struct sockaddr sa = {
		.sa_family = AF_UNSPEC
	};

	ret = util_resolve_host(0, url, port, proxy.family, &sa);
	if (ret) {
		LOG_ERR("getaddrinfo() error: %s", log_strdup(gai_strerror(ret)));
		goto exit;
	}

	if (sa.sa_family == AF_INET) {
		ret = connect(proxy.sock, &sa, sizeof(struct sockaddr_in));
	} else {
		ret = connect(proxy.sock, &sa, sizeof(struct sockaddr_in6));
	}

	if (ret < 0) {
		LOG_ERR("connect() failed: %d", -errno);
		sprintf(rsp_buf, "\r\n#XUDPCLI: %d\r\n", -errno);
		rsp_send(rsp_buf, strlen(rsp_buf));
		ret = -errno;
		goto exit;
	}

	udp_thread_id = k_thread_create(&udp_thread, udp_thread_stack,
			K_THREAD_STACK_SIZEOF(udp_thread_stack),
			udp_thread_func, NULL, NULL, NULL,
			THREAD_PRIORITY, K_USER, K_NO_WAIT);

	proxy.role = UDP_ROLE_CLIENT;
	sprintf(rsp_buf, "\r\n#XUDPCLI: %d,\"connected\"\r\n", proxy.sock);
	rsp_send(rsp_buf, strlen(rsp_buf));

	return 0;
exit:
	close(proxy.sock);
	proxy.sock = INVALID_SOCKET;
	sprintf(rsp_buf, "\r\n#XUDPCLI: %d,\"not connected\"\r\n", ret);
	rsp_send(rsp_buf, strlen(rsp_buf));

	return ret;

}

static int do_udp_client_disconnect(void)
{
	int ret = 0;

	if (proxy.sock != INVALID_SOCKET) {
		ret = close(proxy.sock);
		if (ret < 0) {
			LOG_WRN("close() failed: %d", -errno);
			ret = -errno;
		}
		(void)slm_at_udp_proxy_init();
		sprintf(rsp_buf, "\r\n#XUDPCLI: \"disconnected\"\r\n");
		rsp_send(rsp_buf, strlen(rsp_buf));
	}

	return ret;
}

static int do_udp_send(const uint8_t *data, int datalen)
{
	int ret = 0;
	uint32_t offset = 0;

	if (proxy.sock == INVALID_SOCKET) {
		LOG_ERR("Not connected yet");
		return -EINVAL;
	}

	while (offset < datalen) {
		if (proxy.role == UDP_ROLE_SERVER) {
			/* send to rememberd remote */
			if (proxy.family == AF_INET) {
				ret = sendto(proxy.sock, data + offset, datalen - offset, 0,
					(struct sockaddr *)&(proxy.remote),
					sizeof(struct sockaddr_in));
			} else {
				ret = sendto(proxy.sock, data + offset, datalen - offset, 0,
					(struct sockaddr *)&(proxy.remote6),
					sizeof(struct sockaddr_in6));
			}
		} else {
			ret = send(proxy.sock, data + offset, datalen - offset,
				0);
		}
		if (ret < 0) {
			LOG_ERR("send() failed: %d", -errno);
			if (errno != EAGAIN && errno != ETIMEDOUT) {
				sprintf(rsp_buf, "\r\n#XUDPSEND: %d\r\n", -errno);
				rsp_send(rsp_buf, strlen(rsp_buf));
				if (proxy.role == UDP_ROLE_SERVER) {
					do_udp_server_stop(-errno);
				} else {
					do_udp_client_disconnect();
				}
			}
			ret = -errno;
			break;
		}
		offset += ret;
	}

	if (ret >= 0) {
		sprintf(rsp_buf, "\r\n#XUDPSEND: %d\r\n", offset);
		rsp_send(rsp_buf, strlen(rsp_buf));
#if defined(CONFIG_SLM_UI)
		if (offset > 0) {
			if(proxy.family == AF_INET) {
				if (offset < NET_IPV4_MTU/3) {
					ui_led_set_state(LED_ID_DATA, UI_DATA_SLOW);
				} else if (offset < 2*NET_IPV4_MTU/3) {
					ui_led_set_state(LED_ID_DATA, UI_DATA_NORMAL);
				} else {
					ui_led_set_state(LED_ID_DATA, UI_DATA_FAST);
				}
			} else {
				if (offset < NET_IPV6_MTU/3) {
					ui_led_set_state(LED_ID_DATA, UI_DATA_SLOW);
				} else if (offset < 2*NET_IPV6_MTU/3) {
					ui_led_set_state(LED_ID_DATA, UI_DATA_NORMAL);
				} else {
					ui_led_set_state(LED_ID_DATA, UI_DATA_FAST);
				}
			}
		}
#endif
		return 0;
	} else {
		return ret;
	}
}

static int do_udp_send_datamode(const uint8_t *data, int datalen)
{
	int ret = 0;
	uint32_t offset = 0;

	while (offset < datalen) {
		if (proxy.role == UDP_ROLE_SERVER) {
			/* send to rememberd remote */
			if (proxy.family == AF_INET) {
				ret = sendto(proxy.sock, data + offset, datalen - offset, 0,
					(struct sockaddr *)&(proxy.remote),
					sizeof(struct sockaddr_in));
			} else {
				ret = sendto(proxy.sock, data + offset, datalen - offset, 0,
					(struct sockaddr *)&(proxy.remote6),
					sizeof(struct sockaddr_in6));
			}
		} else {
			ret = send(proxy.sock, data + offset, datalen - offset,
				0);
		}
		if (ret < 0) {
			LOG_ERR("send() failed: %d", -errno);
			if (errno != EAGAIN && errno != ETIMEDOUT) {
				(void)exit_datamode();
				if (proxy.role == UDP_ROLE_SERVER) {
					do_udp_server_stop(-errno);
				} else {
					do_udp_client_disconnect();
				}
			}
			break;
		}
		offset += ret;
	}

#if defined(CONFIG_SLM_UI)
	if (offset > 0) {
		if(proxy.family == AF_INET) {
			if (offset < NET_IPV4_MTU/3) {
				ui_led_set_state(LED_ID_DATA, UI_DATA_SLOW);
			} else if (offset < 2*NET_IPV4_MTU/3) {
				ui_led_set_state(LED_ID_DATA, UI_DATA_NORMAL);
			} else {
				ui_led_set_state(LED_ID_DATA, UI_DATA_FAST);
			}
		} else {
			if (offset < NET_IPV6_MTU/3) {
				ui_led_set_state(LED_ID_DATA, UI_DATA_SLOW);
			} else if (offset < 2*NET_IPV6_MTU/3) {
				ui_led_set_state(LED_ID_DATA, UI_DATA_NORMAL);
			} else {
				ui_led_set_state(LED_ID_DATA, UI_DATA_FAST);
			}
		}
	}
#endif
	return (offset > 0) ? offset : -1;
}

static void udp_thread_func(void *p1, void *p2, void *p3)
{
	int ret;
	struct pollfd fds;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	fds.fd = proxy.sock;
	fds.events = POLLIN;

	do {
		ret = poll(&fds, 1, MSEC_PER_SEC * CONFIG_SLM_UDP_POLL_TIME);
		if (ret < 0) {  /* IO error */
			LOG_WRN("poll() error: %d", ret);
			continue;
		}
		if (ret == 0) {  /* timeout */
			continue;
		}
		LOG_DBG("Poll events 0x%08x", fds.revents);
		if ((fds.revents & POLLERR) == POLLERR) {
			LOG_DBG("Socket error");
			return;
		}
		if ((fds.revents & POLLNVAL) == POLLNVAL) {
			LOG_DBG("Socket closed");
			return;
		}
		if ((fds.revents & POLLIN) != POLLIN) {
			continue;
		} else {
			if (proxy.role == UDP_ROLE_SERVER) {
				/* remember remote from last recvfrom */
				if (proxy.family == AF_INET) {
					int size = sizeof(struct sockaddr_in);

					memset(&proxy.remote, 0, sizeof(struct sockaddr_in));
					ret = recvfrom(proxy.sock, (void *)rx_data, sizeof(rx_data), 0,
						(struct sockaddr *)&(proxy.remote), &size);
				} else {
					int size = sizeof(struct sockaddr_in6);

					memset(&proxy.remote6, 0, sizeof(struct sockaddr_in6));
					ret = recvfrom(proxy.sock, (void *)rx_data, sizeof(rx_data), 0,
						(struct sockaddr *)&(proxy.remote6), &size);
				}
			} else {
				ret = recv(proxy.sock, (void *)rx_data, sizeof(rx_data), 0);
			}
		}
		if (ret < 0) {
			LOG_WRN("recv() error: %d", -errno);
			continue;
		}
		if (ret == 0) {
			continue;
		}
#if defined(CONFIG_SLM_UI)
		if(proxy.family == AF_INET) {
			if (ret < NET_IPV4_MTU/3) {
				ui_led_set_state(LED_ID_DATA, UI_DATA_SLOW);
			} else if (ret < 2*NET_IPV4_MTU/3) {
				ui_led_set_state(LED_ID_DATA, UI_DATA_NORMAL);
			} else {
				ui_led_set_state(LED_ID_DATA, UI_DATA_FAST);
			}
		} else {
			if (ret < NET_IPV6_MTU/3) {
				ui_led_set_state(LED_ID_DATA, UI_DATA_SLOW);
			} else if (ret < 2*NET_IPV6_MTU/3) {
				ui_led_set_state(LED_ID_DATA, UI_DATA_NORMAL);
			} else {
				ui_led_set_state(LED_ID_DATA, UI_DATA_FAST);
			}
		}	
#endif
		if (udp_datamode) {
			rsp_send(rx_data, ret);
		} else if (slm_util_hex_check(rx_data, ret)) {
			uint8_t data_hex[ret * 2];

			ret = slm_util_htoa(rx_data, ret, data_hex, ret * 2);
			if (ret > 0) {
				sprintf(rsp_buf, "\r\n#XUDPDATA: %d,%d\r\n", DATATYPE_HEXADECIMAL,
					ret);
				rsp_send(rsp_buf, strlen(rsp_buf));
				rsp_send(data_hex, ret);
				rsp_send("\r\n", 2);
			} else {
				LOG_WRN("hex convert error: %d", ret);
			}
		} else {
			sprintf(rsp_buf, "\r\n#XUDPDATA: %d,%d\r\n", DATATYPE_PLAINTEXT, ret);
			rsp_send(rsp_buf, strlen(rsp_buf));
			rsp_send(rx_data, ret);
			rsp_send("\r\n", 2);
		}
	} while (true);

	LOG_DBG("Quit receive thread");
}

static int udp_datamode_callback(uint8_t op, const uint8_t *data, int len)
{
	int ret = 0;

	if (op == DATAMODE_SEND) {
		ret = do_udp_send_datamode(data, len);
		LOG_DBG("datamode send: %d", ret);
	} else if (op == DATAMODE_EXIT) {
		udp_datamode = false;
		LOG_DBG("datamode exit");
	}

	return ret;
}

/**@brief handle AT#XUDPSVR commands
 *  AT#XUDPSVR=<op>[,<port>]
 *  AT#XUDPSVR? READ command not supported
 *  AT#XUDPSVR=?
 */
int handle_at_udp_server(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;
	uint16_t op;
	uint16_t port;

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
		err = at_params_unsigned_short_get(&at_param_list, 1, &op);
		if (err) {
			return err;
		}
		if (op == SERVER_START || op == SERVER_START_WITH_DATAMODE 
			|| op == SERVER_START6 || op == SERVER_START6_WITH_DATAMODE) {
			err = at_params_unsigned_short_get(&at_param_list, 2, &port);
			if (err) {
				return err;
			}
			if (proxy.sock > 0) {
				LOG_WRN("Server is running");
				return -EINVAL;
			}
#if defined(CONFIG_SLM_DATAMODE_HWFC)
			if ((op == SERVER_START_WITH_DATAMODE 
				|| op == SERVER_START6_WITH_DATAMODE) && !check_uart_flowcontrol()) {
				LOG_ERR("Data mode requires HWFC.");
				return -EINVAL;
			}
#endif
			if(op == SERVER_START || op == SERVER_START_WITH_DATAMODE) {
				proxy.family = AF_INET;
			} else if(op == SERVER_START6 || op == SERVER_START6_WITH_DATAMODE) {
				proxy.family = AF_INET6;
			} else {
				proxy.family = AF_UNSPEC;
			}
			err = do_udp_server_start((uint16_t)port);
			if (err == 0 && (op == SERVER_START_WITH_DATAMODE
				|| op == SERVER_START6_WITH_DATAMODE)) {
				udp_datamode = true;
				enter_datamode(udp_datamode_callback);
			}
		} else if (op == SERVER_STOP) {
			if (proxy.sock < 0) {
				LOG_WRN("Server is not running");
				return -EINVAL;
			}
			err = do_udp_server_stop(0);
		} break;

	case AT_CMD_TYPE_READ_COMMAND:
		sprintf(rsp_buf, "\r\n#XUDPSVR: %d,%d,%d\r\n", proxy.sock, udp_datamode, proxy.family);
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	case AT_CMD_TYPE_TEST_COMMAND:
		sprintf(rsp_buf, "\r\n#XUDPSVR: (%d,%d,%d,%d,%d),<port>\r\n",
			SERVER_STOP, SERVER_START, SERVER_START_WITH_DATAMODE,
			SERVER_START6, SERVER_START6_WITH_DATAMODE);
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	default:
		break;
	}

	return err;
}

/**@brief handle AT#XUDPCLI commands
 *  AT#XUDPCLI=<op>[,<url>,<port>[,<sec_tag>]
 *  AT#XUDPCLI? READ command not supported
 *  AT#XUDPCLI=?
 */
int handle_at_udp_client(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;
	uint16_t op;

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
		err = at_params_unsigned_short_get(&at_param_list, 1, &op);
		if (err) {
			return err;
		}
		if (op == CLIENT_CONNECT || op == CLIENT_CONNECT_WITH_DATAMODE
			|| op == CLIENT_CONNECT6 || op == CLIENT_CONNECT6_WITH_DATAMODE) {
			uint16_t port;
			char url[SLM_MAX_URL];
			int size = SLM_MAX_URL;

			if (proxy.sock != INVALID_SOCKET) {
				LOG_ERR("Client is connected.");
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
			proxy.sec_tag  = INVALID_SEC_TAG;
			if (at_params_valid_count_get(&at_param_list) > 4) {
				at_params_unsigned_int_get(&at_param_list, 4, &proxy.sec_tag);
			}
#if defined(CONFIG_SLM_DATAMODE_HWFC)
			if ((op == CLIENT_CONNECT_WITH_DATAMODE 
				|| op == CLIENT_CONNECT6_WITH_DATAMODE)  && !check_uart_flowcontrol()) {
				LOG_ERR("Data mode requires HWFC.");
				return -EINVAL;
			}
#endif
			if((op == CLIENT_CONNECT) || (op == CLIENT_CONNECT_WITH_DATAMODE)) {
				proxy.family = AF_INET;
			} else if((op == CLIENT_CONNECT6) || (op == CLIENT_CONNECT6_WITH_DATAMODE)) {
				proxy.family = AF_INET6;
			} else {
				proxy.family = AF_UNSPEC;
			}

			err = do_udp_client_connect(url, (uint16_t)port, proxy.sec_tag);
			if (err == 0 && (op == CLIENT_CONNECT_WITH_DATAMODE
				|| op == CLIENT_CONNECT6_WITH_DATAMODE)) {
				udp_datamode = true;
				enter_datamode(udp_datamode_callback);
			}
		} else if (op == CLIENT_DISCONNECT) {
			if (proxy.sock < 0) {
				LOG_WRN("Client is not connected");
				return -EINVAL;
			}
			err = do_udp_client_disconnect();
		} break;

	case AT_CMD_TYPE_READ_COMMAND:
		sprintf(rsp_buf, "\r\n#XUDPCLI: %d,%d,%d\r\n", proxy.sock, udp_datamode,proxy.family);
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	case AT_CMD_TYPE_TEST_COMMAND:
		sprintf(rsp_buf, "\r\n#XUDPCLI: (%d,%d,%d,%d,%d),<url>,<port>,<sec_tag>\r\n",
			CLIENT_DISCONNECT, CLIENT_CONNECT, CLIENT_CONNECT_WITH_DATAMODE,
			CLIENT_CONNECT6, CLIENT_CONNECT6_WITH_DATAMODE);
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	default:
		break;
	}

	return err;
}

/**@brief handle AT#XUDPSEND commands
 *  AT#XUDPSEND=<datatype>,<data>
 *  AT#XUDPSEND? READ command not supported
 *  AT#XUDPSEND=? TEST command not supported
 */
int handle_at_udp_send(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;
	uint16_t datatype;
	char data[SLM_MAX_PAYLOAD + 1];
	int size = SLM_MAX_PAYLOAD ;

	if (proxy.family == AF_UNSPEC) {
		return err;
	}

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
				err = do_udp_send(data_hex, err);
			}
		} else {
			err = do_udp_send(data, size);
		}
		break;

	default:
		break;
	}

	return err;
}

/**@brief API to initialize UDP Proxy AT commands handler
 */
int slm_at_udp_proxy_init(void)
{
	proxy.sock = INVALID_SOCKET;
	udp_datamode = false;
	proxy.role = UDP_ROLD_UNSPEC;
	proxy.sec_tag  = INVALID_SEC_TAG;
	proxy.family = AF_UNSPEC;

	return 0;
}

/**@brief API to uninitialize UDP Proxy AT commands handler
 */
int slm_at_udp_proxy_uninit(void)
{
	int ret = 0;

	if (proxy.sock != INVALID_SOCKET) {
		k_thread_abort(udp_thread_id);
		ret = close(proxy.sock);
		if (ret < 0) {
			LOG_WRN("close() failed: %d", -errno);
			ret = -errno;
		}
		proxy.sock = INVALID_SOCKET;
	}

	return ret;
}
