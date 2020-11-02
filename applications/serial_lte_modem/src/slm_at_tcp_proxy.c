/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-BSD-5-Clause-Nordic
 */
#include <logging/log.h>
#include <zephyr.h>
#include <stdio.h>
#include <string.h>
#include <net/socket.h>
#include <modem/modem_info.h>
#include <sys/ring_buffer.h>
#include "slm_util.h"
#include "slm_native_tls.h"
#include "slm_at_host.h"
#include "slm_at_tcp_proxy.h"
#if defined(CONFIG_SLM_UI)
#include "slm_ui.h"
#endif

LOG_MODULE_REGISTER(tcp_proxy, CONFIG_SLM_LOG_LEVEL);

#define THREAD_STACK_SIZE	(KB(3) + NET_IPV4_MTU)
#define THREAD_PRIORITY		K_LOWEST_APPLICATION_THREAD_PRIO
#define DATA_HEX_MAX_SIZE	(2 * NET_IPV4_MTU)

#define MAX_POLL_FD			2

/**@brief Proxy operations. */
enum slm_tcp_proxy_operation {
	AT_SERVER_STOP,
	AT_CLIENT_DISCONNECT = AT_SERVER_STOP,
	AT_WHITELIST_CLEAR =  AT_SERVER_STOP,
	AT_SERVER_START,
	AT_CLIENT_CONNECT = AT_SERVER_START,
	AT_WHITELIST_SET =  AT_SERVER_START,
	AT_SERVER_START_WITH_DATAMODE,
	AT_CLIENT_CONNECT_WITH_DATAMODE = AT_SERVER_START_WITH_DATAMODE
};

/**@brief Proxy roles. */
enum slm_tcp_proxy_role {
	AT_TCP_ROLE_CLIENT,
	AT_TCP_ROLE_SERVER
};

/**@brief List of supported AT commands. */
enum slm_tcp_proxy_at_cmd_type {
	AT_TCP_WHITELIST,
	AT_TCP_SERVER,
	AT_TCP_CLIENT,
	AT_TCP_SEND,
	AT_TCP_RECV,
	AT_TCP_PROXY_MAX
};

/**@brief Action towards client out of whitelist. */
enum slm_tcp_whitelist_action {
	AT_TCP_ACTION_NONE,
	AT_TCP_ACTION_DISCONNECT,
	AT_TCP_ACTION_DROPDATA
};

/** forward declaration of cmd handlers **/
static int handle_at_tcp_whitelist(enum at_cmd_type cmd_type);
static int handle_at_tcp_server(enum at_cmd_type cmd_type);
static int handle_at_tcp_client(enum at_cmd_type cmd_type);
static int handle_at_tcp_send(enum at_cmd_type cmd_type);
static int handle_at_tcp_recv(enum at_cmd_type cmd_type);

/**@brief SLM AT Command list type. */
static slm_at_cmd_list_t tcp_proxy_at_list[AT_TCP_PROXY_MAX] = {
	{AT_TCP_WHITELIST, "AT#XTCPWHTLST", handle_at_tcp_whitelist},
	{AT_TCP_SERVER, "AT#XTCPSVR", handle_at_tcp_server},
	{AT_TCP_CLIENT, "AT#XTCPCLI", handle_at_tcp_client},
	{AT_TCP_SEND, "AT#XTCPSEND", handle_at_tcp_send},
	{AT_TCP_RECV, "AT#XTCPRECV", handle_at_tcp_recv},
};

RING_BUF_DECLARE(data_buf, CONFIG_AT_CMD_RESPONSE_MAX_LEN / 2);
static uint8_t data_hex[DATA_HEX_MAX_SIZE];
static char ip_whitelist[CONFIG_SLM_WHITELIST_SIZE][INET_ADDRSTRLEN];
static int whitelist_action;
static struct k_thread tcp_thread;
static K_THREAD_STACK_DEFINE(tcp_thread_stack, THREAD_STACK_SIZE);
K_TIMER_DEFINE(conn_timer, NULL, NULL);

static struct sockaddr_in remote;
static struct tcp_proxy_t {
	int sock; /* Socket descriptor. */
	sec_tag_t sec_tag; /* Security tag of the credential */
	int sock_peer; /* Socket descriptor for peer. */
	bool whitelisted; /* peer in whitelist */
	int role; /* Client or Server proxy */
	bool datamode; /* Data mode flag*/
	uint16_t timeout; /* Peer connection timeout */
} proxy;

/* global functions defined in different files */
void rsp_send(const uint8_t *str, size_t len);

/* global variable defined in different files */
extern struct at_param_list at_param_list;
extern struct modem_param_info modem_param;
extern char rsp_buf[CONFIG_AT_CMD_RESPONSE_MAX_LEN];

/** forward declaration of thread function **/
static void tcpcli_thread_func(void *p1, void *p2, void *p3);
static void tcpsvr_thread_func(void *p1, void *p2, void *p3);

static int do_tcp_server_start(uint16_t port, int sec_tag)
{
	int ret = 0;
	struct sockaddr_in local;
	int addr_len, addr_reuse = 1;

#if defined(CONFIG_SLM_NATIVE_TLS)
	if (sec_tag != INVALID_SEC_TAG) {
		ret = slm_tls_loadcrdl(sec_tag);
		if (ret < 0) {
			LOG_ERR("Fail to load credential: %d", ret);
			return ret;
		}
	}
#endif
	/* Open socket */
	if (sec_tag == INVALID_SEC_TAG) {
		proxy.sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	} else {
		proxy.sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);
	}
	if (proxy.sock < 0) {
		LOG_ERR("socket() failed: %d", -errno);
		sprintf(rsp_buf, "#XTCPSVR: %d\r\n", -errno);
		rsp_send(rsp_buf, strlen(rsp_buf));
		return -errno;
	}

	if (sec_tag != INVALID_SEC_TAG) {
		sec_tag_t sec_tag_list[1] = { sec_tag };

		ret = setsockopt(proxy.sock, SOL_TLS, TLS_SEC_TAG_LIST,
				sec_tag_list, sizeof(sec_tag_t));
		if (ret) {
			LOG_ERR("set tag list failed: %d", -errno);
			sprintf(rsp_buf, "#XTCPSVR: %d\r\n", -errno);
			rsp_send(rsp_buf, strlen(rsp_buf));
			close(proxy.sock);
			return -errno;
		}
	}

	/* Allow reuse of local addresses */
	ret = setsockopt(proxy.sock, SOL_SOCKET, SO_REUSEADDR,
			 &addr_reuse, sizeof(addr_reuse));
	if (ret != 0) {
		LOG_ERR("set reuse addr failed: %d", -errno);
		sprintf(rsp_buf, "#XTCPSVR: %d\r\n", -errno);
		rsp_send(rsp_buf, strlen(rsp_buf));
		close(proxy.sock);
		return -errno;
	}

	/* Bind to local port */
	local.sin_family = AF_INET;
	local.sin_port = htons(port);
	ret = modem_info_params_get(&modem_param);
	if (ret) {
		LOG_ERR("Unable to obtain modem parameters (%d)", ret);
		close(proxy.sock);
		return ret;
	}
	addr_len = strlen(modem_param.network.ip_address.value_string);
	if (addr_len == 0) {
		LOG_ERR("LTE not connected yet");
		close(proxy.sock);
		return -EINVAL;
	}
	if (!check_for_ipv4(modem_param.network.ip_address.value_string,
			addr_len)) {
		LOG_ERR("Invalid local address");
		close(proxy.sock);
		return -EINVAL;
	}
	if (inet_pton(AF_INET, modem_param.network.ip_address.value_string,
		&local.sin_addr) != 1) {
		LOG_ERR("Parse local IP address failed: %d", -errno);
		close(proxy.sock);
		return -EINVAL;
	}

	ret = bind(proxy.sock, (struct sockaddr *)&local,
		 sizeof(struct sockaddr_in));
	if (ret) {
		LOG_ERR("bind() failed: %d", -errno);
		sprintf(rsp_buf, "#XTCPSVR: %d\r\n", -errno);
		rsp_send(rsp_buf, strlen(rsp_buf));
		close(proxy.sock);
		return -errno;
	}

	/* Enable listen */
	ret = listen(proxy.sock, 1);
	if (ret < 0) {
		LOG_ERR("listen() failed: %d", -errno);
		sprintf(rsp_buf, "#XTCPSVR: %d\r\n", -errno);
		rsp_send(rsp_buf, strlen(rsp_buf));
		close(proxy.sock);
		return -errno;
	}

	proxy.role = AT_TCP_ROLE_SERVER;
	k_thread_create(&tcp_thread, tcp_thread_stack,
			K_THREAD_STACK_SIZEOF(tcp_thread_stack),
			tcpsvr_thread_func, NULL, NULL, NULL,
			THREAD_PRIORITY, K_USER, K_NO_WAIT);
	sprintf(rsp_buf, "#XTCPSVR: %d started\r\n", proxy.sock);
	rsp_send(rsp_buf, strlen(rsp_buf));

	return ret;
}

static int do_tcp_server_stop(int error)
{
	int ret = 0;

	if (proxy.sock > 0) {
		k_timer_stop(&conn_timer);
		if (proxy.sock_peer != INVALID_SOCKET) {
			close(proxy.sock_peer);
		}
		ret = close(proxy.sock);
		if (ret < 0) {
			LOG_WRN("close() failed: %d", -errno);
			ret = -errno;
		}
#if defined(CONFIG_SLM_NATIVE_TLS)
		if (proxy.sec_tag != INVALID_SEC_TAG) {
			ret = slm_tls_unloadcrdl(proxy.sec_tag);
			if (ret < 0) {
				LOG_ERR("Fail to unload credential: %d", ret);
			}
		}
#endif
		if (error) {
			sprintf(rsp_buf, "#XTCPSVR: %d stopped\r\n", error);
		} else {
			sprintf(rsp_buf, "#XTCPSVR: stopped\r\n");
		}
		rsp_send(rsp_buf, strlen(rsp_buf));
	}

	return ret;
}

static int do_tcp_client_connect(const char *url, uint16_t port, int sec_tag)
{
	int ret;

	/* Open socket */
	if (sec_tag == INVALID_SEC_TAG) {
		proxy.sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	} else {
		proxy.sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TLS_1_2);

	}
	if (proxy.sock < 0) {
		LOG_ERR("socket() failed: %d", -errno);
		sprintf(rsp_buf, "#XTCPCLI: %d\r\n", -errno);
		rsp_send(rsp_buf, strlen(rsp_buf));
		return -errno;
	}
	if (sec_tag != INVALID_SEC_TAG) {
		sec_tag_t sec_tag_list[1] = { sec_tag };

		ret = setsockopt(proxy.sock, SOL_TLS, TLS_SEC_TAG_LIST,
				sec_tag_list, sizeof(sec_tag_t));
		if (ret) {
			LOG_ERR("set tag list failed: %d", -errno);
			sprintf(rsp_buf, "#XTCPCLI: %d\r\n", -errno);
			rsp_send(rsp_buf, strlen(rsp_buf));
			close(proxy.sock);
			return -errno;
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
			close(proxy.sock);
			return -EINVAL;
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
			close(proxy.sock);
			return -EINVAL;
		}

		remote.sin_family = AF_INET;
		remote.sin_port = htons(port);
		remote.sin_addr.s_addr =
		((struct sockaddr_in *)result->ai_addr)->sin_addr.s_addr;
		/* Free the address. */
		freeaddrinfo(result);
	}

	ret = connect(proxy.sock, (struct sockaddr *)&remote,
		sizeof(struct sockaddr_in));
	if (ret < 0) {
		LOG_ERR("connect() failed: %d", -errno);
		sprintf(rsp_buf, "#XTCPCLI: %d\r\n", -errno);
		rsp_send(rsp_buf, strlen(rsp_buf));
		close(proxy.sock);
		return -errno;
	}

	proxy.role = AT_TCP_ROLE_CLIENT;
	k_thread_create(&tcp_thread, tcp_thread_stack,
			K_THREAD_STACK_SIZEOF(tcp_thread_stack),
			tcpcli_thread_func, NULL, NULL, NULL,
			THREAD_PRIORITY, K_USER, K_NO_WAIT);
	sprintf(rsp_buf, "#XTCPCLI: %d connected\r\n", proxy.sock);
	rsp_send(rsp_buf, strlen(rsp_buf));

	return ret;
}

static int do_tcp_client_disconnect(int error)
{
	int ret = 0;

	if (proxy.sock > 0) {
		ret = close(proxy.sock);
		if (ret < 0) {
			LOG_WRN("close() failed: %d", -errno);
			ret = -errno;
		}
		(void)slm_at_tcp_proxy_init();
		if (error) {
			sprintf(rsp_buf, "#XTCPCLI: %d disconnected\r\n",
				error);
		} else {
			sprintf(rsp_buf, "#XTCPCLI: disconnected\r\n");
		}
		rsp_send(rsp_buf, strlen(rsp_buf));
	}

	return ret;
}

static int do_tcp_send(const uint8_t *data, int datalen)
{
	int ret = 0;
	uint32_t offset = 0;
	int sock;

	if (proxy.role == AT_TCP_ROLE_CLIENT &&
	    proxy.sock != INVALID_SOCKET) {
		sock = proxy.sock;
	} else if (proxy.role == AT_TCP_ROLE_SERVER &&
		   proxy.sock_peer != INVALID_SOCKET) {
		sock = proxy.sock_peer;
		k_timer_stop(&conn_timer);
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
					do_tcp_client_disconnect(-errno);
				} else {
					do_tcp_server_stop(-errno);
				}
			} else {
				sprintf(rsp_buf, "#XTCPSEND: %d\r\n", -errno);
				rsp_send(rsp_buf, strlen(rsp_buf));
			}
			ret = -errno;
			break;
		}
		offset += ret;
	}

	sprintf(rsp_buf, "#XTCPSEND: %d\r\n", offset);
	rsp_send(rsp_buf, strlen(rsp_buf));

	/* restart activity timer */
	if (proxy.role == AT_TCP_ROLE_SERVER) {
		LOG_ERR("do_tcp_send: Restart timer:%d", proxy.timeout);
		k_timer_start(&conn_timer, K_SECONDS(proxy.timeout),
			K_NO_WAIT);
	}

	if (ret >= 0) {
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

	if (proxy.role == AT_TCP_ROLE_CLIENT &&
	    proxy.sock != INVALID_SOCKET) {
		sock = proxy.sock;
	} else if (proxy.role == AT_TCP_ROLE_SERVER &&
		   proxy.sock_peer != INVALID_SOCKET) {
		sock = proxy.sock_peer;
		k_timer_stop(&conn_timer);
	} else {
		LOG_ERR("Not connected yet");
		return -EINVAL;
	}

	while (offset < datalen) {
		ret = send(sock, data + offset, datalen - offset, 0);
		if (ret < 0) {
			LOG_ERR("send() failed: %d", -errno);
			ret = -errno;
			break;
		}
		offset += ret;
	}

	/* restart activity timer */
	if (proxy.role == AT_TCP_ROLE_SERVER) {
		LOG_ERR("do_tcp_send_datamode: Restart timer:%d", proxy.timeout);
		k_timer_start(&conn_timer, K_SECONDS(proxy.timeout),
			K_NO_WAIT);
	}

	return offset;
}

static int tcp_data_save(uint8_t *data, uint32_t length)
{
	if (ring_buf_space_get(&data_buf) < length) {
		return -1; /* RX overrun */
	}

	return ring_buf_put(&data_buf, data, length);
}

/* TCP server thread */
static void tcpsvr_thread_func(void *p1, void *p2, void *p3)
{
	int ret, nfds = 0, current_size = 0;
	struct pollfd fds[MAX_POLL_FD];

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	fds[nfds].fd = proxy.sock;
	fds[nfds].events = POLLIN;
	nfds++;
	ring_buf_reset(&data_buf);
	while (true) {
		if (proxy.role == AT_TCP_ROLE_SERVER &&
			proxy.timeout > 0 &&
			k_timer_status_get(&conn_timer) > 0) {
			k_timer_stop(&conn_timer);
			LOG_INF("Connecion timeout");
			sprintf(rsp_buf, "#XTCPSVR: timeout\r\n");
			rsp_send(rsp_buf, strlen(rsp_buf));
			close(proxy.sock_peer);
			proxy.sock_peer = INVALID_SOCKET;
		}
		ret = poll(fds, nfds, MSEC_PER_SEC * CONFIG_SLM_TCP_POLL_TIME);
		if (ret < 0) {  /* IO error */
			LOG_WRN("poll() error: %d", ret);
			return;
		}
		if (ret == 0) {  /* timeout */
			LOG_DBG("poll() timeout");
			continue;
		}
		current_size = nfds;
		for (int i = 0; i < current_size; i++) {
			LOG_DBG("Poll events 0x%08x", fds[i].revents);
			if ((fds[i].revents & POLLERR) == POLLERR) {
				LOG_ERR("POLLERR:%d", i);
				return;
			}
			if ((fds[i].revents & POLLHUP) == POLLHUP) {
				LOG_DBG("Peer disconnect:%d", fds[i].fd);
				sprintf(rsp_buf, "#XTCPSVR: disconnected\r\n");
				rsp_send(rsp_buf, strlen(rsp_buf));
				close(fds[i].fd);
				proxy.sock_peer = INVALID_SOCKET;
				fds[i].fd = INVALID_SOCKET;
				nfds--;
				k_timer_stop(&conn_timer);
				continue;
			}
			if ((fds[i].revents & POLLNVAL) == POLLNVAL) {
				if (fds[i].fd == proxy.sock) {
					LOG_ERR("TCP server closed.");
					proxy.sock = INVALID_SOCKET;
					return;
				} else {
					LOG_INF("TCP server peer closed.");
					nfds--;
				}
			}
			if ((fds[i].revents & POLLIN) == POLLIN) {
				if (fds[i].fd == proxy.sock) {
					socklen_t len;
					char peer_addr[INET_ADDRSTRLEN];

					len = sizeof(struct sockaddr_in);
					if (nfds >= MAX_POLL_FD) {
						LOG_WRN("Full. Can not accept"
						" connection.");
						continue;
					}
					/* Accept incoming connection */;
					LOG_DBG("Accept connection...");
					proxy.sock_peer = INVALID_SOCKET;
					proxy.whitelisted = false;
					ret = accept(proxy.sock,
						     (struct sockaddr *)&remote, &len);
					if (ret < 0) {
						LOG_ERR("accept() failed: %d", -errno);
						do_tcp_server_stop(-errno);
						return;
					} else {
						LOG_DBG("accept(): %d", ret);
					}
					/* Whitelist filtering */
					if (inet_ntop(AF_INET, &remote.sin_addr, peer_addr,
						INET_ADDRSTRLEN) == NULL) {
						LOG_ERR("inet_ntop() failed: %d", -errno);
						do_tcp_server_stop(-errno);
						return;
					}
					for (int i = 0; i < CONFIG_SLM_WHITELIST_SIZE; i++) {
						if (strcmp(ip_whitelist[i], peer_addr) == 0) {
							proxy.whitelisted = true;
							break;
						}
					}
					if (!proxy.whitelisted &&
			    		    whitelist_action == AT_TCP_ACTION_DISCONNECT) {
						LOG_INF("Connection from %s filtered", peer_addr);
						sprintf(rsp_buf, "#XTCPSVR: %s filtered\r\n",
							peer_addr);
						rsp_send(rsp_buf, strlen(rsp_buf));
						close(ret);
						continue;
					}
					sprintf(rsp_buf, "#XTCPSVR: %s connected\r\n",
						peer_addr);
					rsp_send(rsp_buf, strlen(rsp_buf));
					proxy.sock_peer = ret;
					LOG_DBG("New connection - %d",
						proxy.sock_peer);
					fds[nfds].fd = proxy.sock_peer;
					fds[nfds].events = POLLIN;
					nfds++;
					/* Start a one-shot timer to close the connection */
					k_timer_start(&conn_timer, K_SECONDS(proxy.timeout), K_NO_WAIT);
					break;
				} else {
					char data[NET_IPV4_MTU];

					ret = recv(fds[i].fd, data, NET_IPV4_MTU, 0);
					if (ret < 0) {
						LOG_WRN("recv() error: %d", -errno);
						continue;
					}
					if (ret == 0) {
						continue;
					}
#if defined(CONFIG_SLM_UI)
					if (ret < NET_IPV4_MTU/3) {
						ui_led_set_state(LED_ID_DATA, UI_DATA_SLOW);
					} else if (ret < 2*NET_IPV4_MTU/3) {
						ui_led_set_state(LED_ID_DATA, UI_DATA_NORMAL);
					} else {
						ui_led_set_state(LED_ID_DATA, UI_DATA_FAST);
					}
#endif
					if (proxy.datamode) {
						rsp_send(data, ret);
					} else if (slm_util_hex_check(data, ret)) {
						ret = slm_util_htoa(data, ret, data_hex,
							DATA_HEX_MAX_SIZE);
						if (ret < 0) {
							LOG_ERR("hex convert error: %d", ret);
							continue;
						}
						if (tcp_data_save(data_hex, ret) < 0) {
							sprintf(rsp_buf,
								"#XTCPDATA: overrun\r\n");
						} else {
							sprintf(rsp_buf,
								"#XTCPDATA: %d, %d\r\n",
								DATATYPE_HEXADECIMAL,
								ret);
						}
						rsp_send(rsp_buf, strlen(rsp_buf));
					} else {
						if (tcp_data_save(data, ret) < 0) {
							sprintf(rsp_buf,
								"#XTCPDATA: overrun\r\n");
						} else {
							sprintf(rsp_buf,
								"#XTCPDATA: %d, %d\r\n",
								DATATYPE_PLAINTEXT, ret);
						}
						rsp_send(rsp_buf, strlen(rsp_buf));
					}
					/* Restart conn timer */
					k_timer_stop(&conn_timer);
					LOG_ERR("restart timer: POLLIN");
					k_timer_start(
						&conn_timer,
						K_SECONDS(proxy.timeout),
						K_NO_WAIT);
				}
			}
		}
	}
}

/* TCP client thread */
static void tcpcli_thread_func(void *p1, void *p2, void *p3)
{
	int ret;
	int sock;
	struct pollfd fds;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	sock = proxy.sock;
	fds.fd = sock;
	fds.events = POLLIN;
	ring_buf_reset(&data_buf);
	while (true) {
		ret = poll(&fds, 1, MSEC_PER_SEC * CONFIG_SLM_TCP_POLL_TIME);
		if (ret < 0) {  /* IO error */
			LOG_WRN("poll() error: %d", ret);
			continue;
		}
		if (ret == 0) {  /* timeout */
			continue;
		}
		LOG_DBG("Poll events 0x%08x", fds.revents);
		if ((fds.revents & POLLNVAL) == POLLNVAL) {
			LOG_INF("TCP client closed.");
			return;
		}
		if ((fds.revents & POLLIN) == POLLIN) {
			char data[NET_IPV4_MTU];

			ret = recv(sock, data, NET_IPV4_MTU, 0);
			if (ret < 0) {
				LOG_WRN("recv() error: %d", -errno);
				continue;
			}
			if (ret == 0) {
				continue;
			}
#if defined(CONFIG_SLM_UI)
			if (ret < NET_IPV4_MTU/3) {
				ui_led_set_state(LED_ID_DATA, UI_DATA_SLOW);
			} else if (ret < 2*NET_IPV4_MTU/3) {
				ui_led_set_state(LED_ID_DATA, UI_DATA_NORMAL);
			} else {
				ui_led_set_state(LED_ID_DATA, UI_DATA_FAST);
			}
#endif
			if (proxy.datamode) {
				rsp_send(data, ret);
			} else if (slm_util_hex_check(data, ret)) {
				ret = slm_util_htoa(data, ret, data_hex,
					DATA_HEX_MAX_SIZE);
				if (ret < 0) {
					LOG_ERR("hex convert error: %d", ret);
					continue;
				}
				if (tcp_data_save(data_hex, ret) < 0) {
					sprintf(rsp_buf,
						"#XTCPDATA: overrun\r\n");
				} else {
					sprintf(rsp_buf,
						"#XTCPDATA: %d, %d\r\n",
						DATATYPE_HEXADECIMAL,
						ret);
				}
				rsp_send(rsp_buf, strlen(rsp_buf));
			} else {
				if (tcp_data_save(data, ret) < 0) {
					sprintf(rsp_buf,
						"#XTCPDATA: overrun\r\n");
				} else {
					sprintf(rsp_buf,
						"#XTCPDATA: %d, %d\r\n",
						DATATYPE_PLAINTEXT, ret);
				}
				rsp_send(rsp_buf, strlen(rsp_buf));
			}
		}
	}
}

/**@brief handle AT#XTCPWHITELIST commands
 *  AT#XTCPWHTLST=<op>[,<action>,<IP_ADDR#1>[,<IP_ADDR#2>[,...]]]
 *  AT#XTCPWHTLST?
 *  AT#XTCPWHTLST=?
 */
static int handle_at_tcp_whitelist(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;
	uint16_t op;
	int param_count = at_params_valid_count_get(&at_param_list);

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
		err = at_params_short_get(&at_param_list, 1, &op);
		if (err) {
			return err;
		}
		if (op == AT_WHITELIST_SET) {
			char address[INET_ADDRSTRLEN];
			int size;

			if (param_count > (CONFIG_SLM_WHITELIST_SIZE + 3)){
				return -EINVAL;
			}
			err = at_params_int_get(&at_param_list, 2,
					&whitelist_action);
			if (err) {
				return err;
			}
			if (whitelist_action != AT_TCP_ACTION_DISCONNECT &&
			    whitelist_action != AT_TCP_ACTION_DROPDATA) {
				return -EINVAL;
			}
			for (int i = 0; i < CONFIG_SLM_WHITELIST_SIZE; i++) {
				memset(ip_whitelist[i], 0x00, INET_ADDRSTRLEN);
			}
			for (int i = 3; i < param_count; i++) {
				size = INET_ADDRSTRLEN;
				err = at_params_string_get(&at_param_list, i,
					address, &size);
				if (err) {
					return err;
				} else if (!check_for_ipv4(address, size)) {
					return -EINVAL;
				} else {
					memcpy(ip_whitelist[i-3], address,
						size);
				}
			}
			err = 0;
		} else if (op == AT_WHITELIST_CLEAR) {
			for (int i = 0; i < CONFIG_SLM_WHITELIST_SIZE; i++) {
				memset(ip_whitelist[i], 0x00, INET_ADDRSTRLEN);
			}
			whitelist_action = AT_TCP_ACTION_NONE;
			err = 0;
		} break;

	case AT_CMD_TYPE_READ_COMMAND:
		sprintf(rsp_buf, "#XTCPWHTLST: %d", whitelist_action);
		for (int i = 0; i < CONFIG_SLM_WHITELIST_SIZE; i++) {
			if (strlen(ip_whitelist[i]) > 0) {
				strcat(rsp_buf, ",\"");
				strcat(rsp_buf, ip_whitelist[i]);
				strcat(rsp_buf, "\"");
			}
		}
		strcat(rsp_buf, "\r\n");
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	case AT_CMD_TYPE_TEST_COMMAND:
		sprintf(rsp_buf, "#XTCPWHTLST: (%d, %d),(%d, %d)",
			AT_WHITELIST_CLEAR, AT_WHITELIST_SET,
			AT_TCP_ACTION_DISCONNECT, AT_TCP_ACTION_DROPDATA);
		strcat(rsp_buf, ",<IP_ADDR#1>[,<IP_ADDR#2>[,...]]\r\n");
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	default:
		break;
	}

	return err;
}

/**@brief handle AT#XTCPSVR commands
 *  AT#XTCPSVR=<op>[,<port>,<timeout>,[sec_tag]]
 *  AT#XTCPSVR?
 *  AT#XTCPSVR=?
 */
static int handle_at_tcp_server(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;
	uint16_t op;
	int param_count = at_params_valid_count_get(&at_param_list);

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
		if (param_count < 2) {
			return -EINVAL;
		}
		err = at_params_short_get(&at_param_list, 1, &op);
		if (err) {
			return err;
		}
		if (op == AT_SERVER_START ||
		    op == AT_SERVER_START_WITH_DATAMODE) {
			uint16_t port;

			if (param_count < 3) {
				return -EINVAL;
			}
			if (proxy.sock != INVALID_SOCKET) {
				LOG_ERR("Server is already running.");
				return -EINVAL;
			}
			slm_at_tcp_proxy_init();
			err = at_params_short_get(&at_param_list, 2, &port);
			if (err) {
				return err;
			}
			err = at_params_short_get(&at_param_list, 3,
						  &proxy.timeout);
			if (err) {
				return err;
			}
			if (param_count > 4) {
				at_params_int_get(&at_param_list, 4,
						  &proxy.sec_tag);
			}
			err = do_tcp_server_start(port, proxy.sec_tag);
			if (err == 0 && op == AT_SERVER_START_WITH_DATAMODE) {
				proxy.datamode = true;
			}
		} else if (op == AT_SERVER_STOP) {
			if (proxy.sock == INVALID_SOCKET) {
				LOG_WRN("Server is not running");
				return -EINVAL;
			}
			err = do_tcp_server_stop(0);
		} break;

	case AT_CMD_TYPE_READ_COMMAND:
		if (proxy.sock != INVALID_SOCKET &&
		    proxy.role == AT_TCP_ROLE_SERVER) {
			sprintf(rsp_buf, "#XTCPSVR: %d, %d, %d, %d\r\n",
				proxy.sock, proxy.sock_peer, proxy.timeout, proxy.datamode);
		} else {
			sprintf(rsp_buf, "#XTCPSVR: %d, %d\r\n",
				INVALID_SOCKET, INVALID_SOCKET);
		}
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	case AT_CMD_TYPE_TEST_COMMAND:
		sprintf(rsp_buf, "#XTCPSVR: (%d, %d, %d),<port>,<timeout>,<sec_tag>\r\n",
			AT_SERVER_STOP, AT_SERVER_START,
			AT_SERVER_START_WITH_DATAMODE);
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	default:
		break;
	}

	return err;
}

/**@brief handle AT#XTCPCLI commands
 *  AT#XTCPCLI=<op>[,<url>,<port>[,[sec_tag]]
 *  AT#XTCPCLI?
 *  AT#XTCPCLI=?
 */
static int handle_at_tcp_client(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;
	uint16_t op;
	int param_count = at_params_valid_count_get(&at_param_list);

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
		if (param_count < 2) {
			return -EINVAL;
		}
		err = at_params_short_get(&at_param_list, 1, &op);
		if (err) {
			return err;
		}
		if (op == AT_CLIENT_CONNECT ||
		    op == AT_CLIENT_CONNECT_WITH_DATAMODE) {
			uint16_t port;
			char url[TCPIP_MAX_URL];
			int size = TCPIP_MAX_URL;
			proxy.sec_tag = INVALID_SEC_TAG;

			if (param_count < 4) {
				return -EINVAL;
			}
			err = at_params_string_get(&at_param_list,
						2, url, &size);
			if (err) {
				return err;
			}
			url[size] = '\0';
			err = at_params_short_get(&at_param_list, 3, &port);
			if (err) {
				return err;
			}
			if (param_count > 4) {
				at_params_int_get(&at_param_list,
						  4, &proxy.sec_tag);
			}
			err = do_tcp_client_connect(url, port, proxy.sec_tag);
			if (err == 0 &&
			    op == AT_CLIENT_CONNECT_WITH_DATAMODE) {
				proxy.datamode = true;
			}
		} else if (op == AT_CLIENT_DISCONNECT) {
			if (proxy.sock < 0) {
				LOG_WRN("Client is not connected");
				return -EINVAL;
			}
			err = do_tcp_client_disconnect(0);
		} break;

	case AT_CMD_TYPE_READ_COMMAND:
		if (proxy.sock != INVALID_SOCKET &&
		    proxy.role == AT_TCP_ROLE_CLIENT) {
			sprintf(rsp_buf, "#XTCPCLI: %d, %d\r\n",
				proxy.sock, proxy.datamode);
		} else {
			sprintf(rsp_buf, "#XTCPCLI: %d\r\n", INVALID_SOCKET);
		}
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
		break;

	case AT_CMD_TYPE_TEST_COMMAND:
		sprintf(rsp_buf,
			"#XTCPCLI: (%d, %d, %d),<url>,<port>,<sec_tag>\r\n",
			AT_CLIENT_DISCONNECT, AT_CLIENT_CONNECT,
			AT_CLIENT_CONNECT_WITH_DATAMODE);
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
static int handle_at_tcp_send(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;
	uint16_t datatype;
	char data[NET_IPV4_MTU];
	int size = NET_IPV4_MTU;

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
		if (at_params_valid_count_get(&at_param_list) < 3) {
			return -EINVAL;
		}
		err = at_params_short_get(&at_param_list, 1, &datatype);
		if (err) {
			return err;
		}
		err = at_params_string_get(&at_param_list, 2, data, &size);
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
static int handle_at_tcp_recv(enum at_cmd_type cmd_type)
{
	int err = -EINVAL;
	uint16_t length = 0;

	switch (cmd_type) {
	case AT_CMD_TYPE_SET_COMMAND:
	{
		uint32_t sz_send = 0;

		if (at_params_valid_count_get(&at_param_list) > 1) {
			err = at_params_short_get(&at_param_list, 1, &length);
			if (err) {
				return err;
			}
		}
		if (ring_buf_is_empty(&data_buf) == 0) {
			sz_send = ring_buf_get(&data_buf, rsp_buf,
					sizeof(rsp_buf));
			if (length > 0 && sz_send > length) {
				sz_send = length;
			}
			rsp_send(rsp_buf, sz_send);
			rsp_send("\r\n", 2);
		}
		sprintf(rsp_buf, "#XTCPRECV: %d\r\n", sz_send);
		rsp_send(rsp_buf, strlen(rsp_buf));
		err = 0;
	} break;

	default:
		break;
	}

	return err;
}

/**@brief API to handle TCP proxy AT commands
 */
int slm_at_tcp_proxy_parse(const char *at_cmd, uint16_t length)
{
	int ret = -ENOENT;
	enum at_cmd_type type;

	for (int i = 0; i < AT_TCP_PROXY_MAX; i++) {
		if (slm_util_cmd_casecmp(at_cmd,
			tcp_proxy_at_list[i].string)) {
			ret = at_parser_params_from_str(at_cmd, NULL,
						&at_param_list);
			if (ret) {
				LOG_ERR("Failed to parse AT command %d", ret);
				return -EINVAL;
			}
			type = at_parser_cmd_type_get(at_cmd);
			ret = tcp_proxy_at_list[i].handler(type);
			break;
		}
	}

	/* handle sending in data mode */
	if (ret == -ENOENT && proxy.datamode) {
		ret = do_tcp_send_datamode(at_cmd, length);
		if (ret > 0) {
#if defined(CONFIG_SLM_UI)
			if (ret < NET_IPV4_MTU/3) {
				ui_led_set_state(LED_ID_DATA, UI_DATA_SLOW);
			} else if (ret < 2*NET_IPV4_MTU/3) {
				ui_led_set_state(LED_ID_DATA, UI_DATA_NORMAL);
			} else {
				ui_led_set_state(LED_ID_DATA, UI_DATA_FAST);
			}
#endif
		}
	}

	return ret;
}

/**@brief API to list TCP proxy AT commands
 */
void slm_at_tcp_proxy_clac(void)
{
	for (int i = 0; i < AT_TCP_PROXY_MAX; i++) {
		sprintf(rsp_buf, "%s\r\n", tcp_proxy_at_list[i].string);
		rsp_send(rsp_buf, strlen(rsp_buf));
	}
}

/**@brief API to initialize TCP proxy AT commands handler
 */
int slm_at_tcp_proxy_init(void)
{
	proxy.sock = INVALID_SOCKET;
	proxy.sock_peer = INVALID_SOCKET;
	proxy.role = INVALID_ROLE;
	proxy.datamode = false;
	proxy.timeout = 0;
	proxy.sec_tag = INVALID_SEC_TAG;
	for (int i = 0; i < CONFIG_SLM_WHITELIST_SIZE; i++) {
		memset(ip_whitelist[i], 0x00, INET_ADDRSTRLEN);
	}
	whitelist_action = AT_TCP_ACTION_NONE;

	return 0;
}

/**@brief API to uninitialize TCP proxy AT commands handler
 */
int slm_at_tcp_proxy_uninit(void)
{
	if (proxy.role == AT_TCP_ROLE_CLIENT) {
		return do_tcp_client_disconnect(0);
	}
	if (proxy.role == AT_TCP_ROLE_SERVER) {
		return do_tcp_server_stop(0);
	}

	return 0;
}
