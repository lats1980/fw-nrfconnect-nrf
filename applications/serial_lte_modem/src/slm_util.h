/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef SLM_UTIL_
#define SLM_UTIL_

/**@file slm_util.h
 *
 * @brief Utility functions for serial LTE modem
 * @{
 */

#include <zephyr/types.h>
#include <ctype.h>
#include <stdbool.h>
#include <net/socket.h>
#include <modem/at_cmd.h>
#include <modem/at_cmd_parser.h>

#define INVALID_SOCKET	-1
#define INVALID_SEC_TAG	-1
#define INVALID_ROLE	-1

/* Merged Protocol numbers of SLM */
enum slm_ip_protocol {
	SLM_IPPROTO_IP = 0,            /**< IP protocol (pseudo-val for setsockopt() */
	SLM_IPPROTO_ICMP = 1,          /**< ICMP protocol   */
	SLM_IPPROTO_TCP = 6,           /**< TCP protocol    */
	SLM_IPPROTO_UDP = 17,          /**< UDP protocol    */
	SLM_IPPROTO_IPV6 = 41,         /**< IPv6 protocol   */
	SLM_IPPROTO_ICMPV6 = 58,       /**< ICMPv6 protocol */
	SLM_IPPROTO_RAW = 255,         /**< RAW IP packets  */
	SLM_IPPROTO_TLS_1_0 = 256,     /**< TLS 1.0 protocol */
	SLM_IPPROTO_TLS_1_1 = 257,     /**< TLS 1.1 protocol */
	SLM_IPPROTO_TLS_1_2 = 258,     /**< TLS 1.2 protocol */
	SLM_IPPROTO_DTLS_1_0 = 272,    /**< DTLS 1.0 protocol */
	SLM_IPPROTO_DTLS_1_2 = 273,    /**< DTLS 1.2 protocol */
};
/**
 * @brief Compare string ignoring case
 *
 * @param str1 First string
 * @param str2 Second string
 *
 * @return true If two commands match, false if not.
 */
bool slm_util_casecmp(const char *str1, const char *str2);

/**
 * @brief Compare name of AT command ignoring case
 *
 * @param cmd Command string received from UART
 * @param slm_cmd Propreiatry command supported by SLM
 *
 * @return true If two commands match, false if not.
 */
bool slm_util_cmd_casecmp(const char *cmd, const char *slm_cmd);

/**
 * @brief Detect hexdecimal data type
 *
 * @param[in] data Hex arrary to be encoded
 * @param[in] data_len Length of hex array
 *
 * @return true if the input is hexdecimal array, otherwise false
 */
bool slm_util_hex_check(const uint8_t *data, uint16_t data_len);

/**
 * @brief Detect hexdecimal string data type
 *
 * @param[in] data Hexdecimal string arrary to be checked
 * @param[in] data_len Length of array
 *
 * @return true if the input is hexdecimal string array, otherwise false
 */
bool slm_util_hexstr_check(const uint8_t *data, uint16_t data_len);

/**
 * @brief Encode hex array to hexdecimal string (ASCII text)
 *
 * @param[in]  hex Hex arrary to be encoded
 * @param[in]  hex_len Length of hex array
 * @param[out] ascii encoded hexdecimal string
 * @param[in]  ascii_len reserved buffer size
 *
 * @return actual size of ascii string if the operation was successful.
 *           Otherwise, a (negative) error code is returned.
 */
int slm_util_htoa(const uint8_t *hex, uint16_t hex_len,
		char *ascii, uint16_t ascii_len);

/**
 * @brief Decode hexdecimal string (ASCII text) to hex array
 *
 * @param[in]  ascii encoded hexdecimal string
 * @param[in]  ascii_len size of hexdecimal string
 * @param[out] hex decoded hex arrary
 * @param[in]  hex_len reserved size of hex array
 *
 * @return actual size of hex array if the operation was successful.
 *           Otherwise, a (negative) error code is returned.
 */
int slm_util_atoh(const char *ascii, uint16_t ascii_len,
		uint8_t *hex, uint16_t hex_len);


/**@brief Check whether a string has valid IPv4 address or not
 *
 * @param address URL text string
 * @param length size of URL string
 *
 * @return true if text string is IPv4 address, false otherwise
 */
bool check_for_ipv4(const char *address, uint8_t length);

/**@brief Check whether a string has valid IPv4/IPv6 address format or not
 *
 * @param address URL text string
 * @param length size of URL string
 *
 * @return true if text string is IPv4 address, false otherwise
 */
bool check_for_ip_format(const char *address, uint8_t length);

/**
 * @brief Get string value from AT command with length check.
 *
 * @p len must be bigger than the string length, or an error is returned.
 * The copied string is null-terminated.
 *
 * @param[in]     list    Parameter list.
 * @param[in]     index   Parameter index in the list.
 * @param[out]    value   Pointer to the buffer where to copy the value.
 * @param[in,out] len     Available space in @p value, returns actual length
 *                        copied into string buffer in bytes, excluding the
 *                        terminating null character.
 *
 * @retval 0 If the operation was successful.
 *           Otherwise, a (negative) error code is returned.
 */
int util_string_get(const struct at_param_list *list, size_t index,
			 char *value, size_t *len);

/**
 * @brief use AT command to get IPv4 and IPv6 addresses
 *
 * @param[in] addr4 buffer to hold the IPv4 address, size NET_IPV4_ADDR_LEN
 * @param[in] addr6 buffer to hold the IPv6 address, size NET_IPV6_ADDR_LEN
 */
void util_get_ip_addr(char *addr4, char *addr6);

/**
 * @brief Resolve remote host by host name or IP address
 *
 * This function wraps up getaddrinfo() to return first resolved address.
 *
 * @param[in] cid PDP Context ID as defined in "+CGDCONT" command (0~10).
 * @param[in] host Name or IP address of remote host.
 * @param[in] port Service port of remote host.
 * @param[in] family Desired address family for the returned address.
 * @param[out] sa The returned address.
 *
 * @retval 0 If the operation was successful.
 *           Otherwise, EAI error code as defined by getaddrinfo().
 */
int util_resolve_host(int cid, const char *host, uint16_t port, int family, struct sockaddr *sa);

/** @} */

#endif /* SLM_UTIL_ */
