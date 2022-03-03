/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <functional>
#include <zephyr.h>
#include <sys/ring_buffer.h>

#define ZB_SHELL_MSG_PROMPT "uart:~$"
#define ZB_SHELL_MSG_CMD_DONE "Done"
#define ZB_SHELL_MSG_CMD_ERROR "Error"
#define ZB_SHELL_MSG_DEVICE_REJOIN "rejoined (short: 0x"
#define ZB_SHELL_MSG_JOIN_NETWORK "Joined network successfully"
#define ZB_SHELL_MSG_REJOIN "on reboot signal"

#define UNPARSED_BUF_LEN 1024
#define MAX_ZIGBEE_CMD_LEN 128
#define UART_BUF_SIZE	256
#define UART_RX_BUF_NUM	2
#define ZB_ZCL_MAX_ATTR_SIZE 40
#define EXT_PAN_ID_SIZE 16

class ZigbeeShell
{
public:
	enum Event_t
	{
		kEvent_NetworkRejoin,
		kEvent_NetworkSteering,
		kEvent_DeviceAnnounceRsp,
		kEvent_ActiveEpRsp,
		kEvent_SimpleDescRsp,
		kEvent_ZclAttrRead
	};
	enum Cluster_t : uint16_t
	{
		kCluster_OnOff = 0x0006,
		kCluster_LevelControl = 0x0008
	};
	/* On/Off cluster attribute identifiers */
	enum OnOffAttr_t : uint16_t
	{
		kOnOffAttr_OnOff = 0x0000
	};
	enum ZclAttrType_t : uint8_t
	{
		kZclAttrType_BOOL = 0x10
	};

	struct BdbEvent {
		char ext_pan_id[EXT_PAN_ID_SIZE + 1];
		uint16_t pan_id;
	};
	struct ZdoEvent {
		uint16_t addr;
		uint8_t ep;
		uint16_t dev_id;
	};
	struct ZclEvent {
		uint16_t addr;
		uint8_t ep;
		uint16_t cluster_id;
		uint16_t attr_id;
		uint8_t type;
		char value[ZB_ZCL_MAX_ATTR_SIZE + 1];
		size_t len;
	};
	union {
		struct BdbEvent Bdb;
		struct ZdoEvent Zdo;
		struct ZclEvent Zcl;
	} mEvent;

	typedef void (*zigbee_event_handler_t)(ZigbeeShell *, Event_t);

	ZigbeeShell();
	int BdbStart();
	int NetworkSteering();
	int ZdoActiveEpReq(uint16_t addr);
	int ZdoSimpleDescReq(uint16_t addr, uint8_t ep);
	int ZclCmd(uint16_t addr, uint8_t ep, uint16_t cluster, uint16_t cmd_id);
	int ZclAttrRead(uint16_t addr,
			uint8_t ep,
			uint16_t profile_id,
			enum Cluster_t cluster_id,
			uint16_t attr_id);
	int ZdoMatchDesc(uint16_t dst_addr,
			 uint16_t req_addr,
			 uint16_t profile_id,
			 uint8_t in_cluster_cnt,
			 uint16_t *in_clusters,
			 uint8_t out_cluster_cnt,
			 uint16_t *out_clusters);
	void SetEventCallback(zigbee_event_handler_t zigbee_event_handler);

private:
	typedef size_t (*ZigbeeResponseHandler)(ZigbeeShell *shell, const char *data, size_t len);

	struct k_sem mCmdSem;
	struct ZigbeeCmd {
		char command[MAX_ZIGBEE_CMD_LEN + 1];
		ZigbeeResponseHandler handler;
		int result;
	};
	struct ZigbeeCmd mZigbeeCmd;
	const struct device *mUartDev;
	uint8_t *mNextUartBuf;
	uint8_t mUartRxBuf[UART_RX_BUF_NUM][UART_BUF_SIZE];
	struct ring_buf mShellRspRb;
	static void UartCallback(const struct device *dev, struct uart_event *evt, void *user_data);
	uint8_t mShellRspBuffer[UNPARSED_BUF_LEN];
	char mParserBuffer[UNPARSED_BUF_LEN];
	static void UartWorkHandler(struct k_work *work);
	struct k_work mUartWork;

	size_t ParseShellMessage(const char * szMsg);
	int WriteCmd(const char *cmd, ZigbeeResponseHandler cmd_handler);
	zigbee_event_handler_t mEvent_CB;

	static size_t ShellRspHandler(ZigbeeShell *shell, const char *data, size_t len);
	static size_t GeneralRspHandler(ZigbeeShell *shell, const char *data, size_t len);
	static size_t ZdoActiveEpRspHandler(ZigbeeShell *shell, const char *data, size_t len);
	static size_t ZdoSimpleDescRspHandler(ZigbeeShell *shell, const char *data, size_t len);
	static size_t ZclAttrReadRspHandler(ZigbeeShell *shell,const char *data, size_t len);
};