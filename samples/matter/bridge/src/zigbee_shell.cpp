/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "zigbee_shell.h"
#include <logging/log.h>
#include <drivers/uart.h>

LOG_MODULE_DECLARE(zigbee_shell);

size_t ZigbeeShell::ShellRspHandler(ZigbeeShell *shell, const char *data, size_t len)
{
	char *p;

	p = strstr(data, ZB_SHELL_MSG_PROMPT);
	if (p != NULL) {
		LOG_DBG("Shell command finished");
		return p - data + strlen(ZB_SHELL_MSG_PROMPT);
	}

	return 0;
}

size_t ZigbeeShell::GeneralRspHandler(ZigbeeShell *shell, const char *data, size_t len)
{
	char *p;

	p = strstr((char *)data, ZB_SHELL_MSG_CMD_DONE);
	if (p != NULL) {
		LOG_DBG("General command finished - Done");
		shell->mZigbeeCmd.result = 0;
		return p - data + strlen(ZB_SHELL_MSG_CMD_DONE);
	}
	p = strstr((char *)data, ZB_SHELL_MSG_CMD_ERROR);
	if (p != NULL) {
		LOG_ERR("General command finished - Error");
		shell->mZigbeeCmd.result = -EINVAL;
		return p - data + strlen(ZB_SHELL_MSG_CMD_ERROR);
	}

	return 0;
}

size_t ZigbeeShell::ZclAttrReadRspHandler(ZigbeeShell *shell,const char *data, size_t len)
{
	char *p, *end;
	int attr_id;
	uint8_t type;
	size_t ret;

	p = strstr((char *)data, ZB_SHELL_MSG_CMD_ERROR);
	if (p != NULL) {
		LOG_ERR("Zcl attr read finished - Error");
		return p - data + strlen(ZB_SHELL_MSG_CMD_ERROR);
	}
	p = strstr((char *)data, ZB_SHELL_MSG_CMD_DONE);
	if (p == NULL) {
		LOG_DBG("Wait for more response");
		return 0;
	}
	ret = p - data + strlen(ZB_SHELL_MSG_CMD_DONE);
	p = strstr(data, "ID: ");
	if (p == NULL) {
		LOG_WRN("attr id missed");
		return ret;
	} else {
		p = p + strlen("ID: ");
		attr_id = strtol(p, &end, 10);
		if (p == end) {
			LOG_WRN("Can't get attr id");
			return ret;
		}
	}
	p = strstr(data, "Type: ");
	if (p == NULL) {
		LOG_WRN("attr type missed");
		return ret;
	} else {
		p = p + strlen("Type: ");
		type = strtol(p, &end, 16);
		if (p == end) {
			LOG_WRN("Can't get attr type");
			return ret;
		}
	}
	p = strstr(data, "Value: ");
	if (p == NULL) {
		LOG_WRN("Attr Value missed");
		return ret;
	}

	char *value_end = nullptr;

	p = p + strlen("Value: ");
	value_end = strstr(p, "\r\n");
	if (value_end == NULL) {
		LOG_WRN("Can't get attr value");
		return ret;
	}
	if ((value_end - p) > ZB_ZCL_MAX_ATTR_SIZE) {
		LOG_ERR("Fail to parse attr value");
		return 0;
	}
	memset(shell->mEvent.Zcl.value, 0, sizeof(shell->mEvent.Zcl.value));
	shell->mEvent.Zcl.len = value_end - p;
	shell->mEvent.Zcl.type = type;
	strncpy(shell->mEvent.Zcl.value, p, shell->mEvent.Zcl.len);
	LOG_INF("ID: %d Type: %x Value: %s", attr_id, type, shell->mEvent.Zcl.value);
	shell->mEvent_CB(shell, kEvent_ZclAttrRead);

	return ret;
}

size_t ZigbeeShell::ZdoActiveEpRspHandler(ZigbeeShell *shell, const char *data, size_t len)
{
	const char *p;
	uint16_t dev_addr;
	size_t ret;

	p = strstr((char *)data, ZB_SHELL_MSG_CMD_ERROR);
	if (p != NULL) {
		LOG_ERR("Zdo active endpoint request finished - Error");
		return p - data + strlen(ZB_SHELL_MSG_CMD_ERROR);
	}
	p = strstr((char *)data, ZB_SHELL_MSG_CMD_DONE);
	if (p == NULL) {
		LOG_DBG("Wait for more response");
		return 0;
	}
	ret = p - data + strlen(ZB_SHELL_MSG_CMD_DONE);
	p = data;
	for (;;) {
		p = strstr(p, "src_addr=");
		if (p == NULL) {
			break;
		} else {
			p = p + strlen("src_addr=");
			dev_addr = strtol(p, NULL, 16);
			p = strstr(p, "ep=");
			if (p == NULL) {
				break;
			} else {
				p = p + strlen("ep=");
				for (;;) {
					char *end;
					int ep;

					ep = strtol(p, &end, 10);
					if (p == end) {
						break;
					}
					shell->mEvent.Zdo.addr = dev_addr;
					shell->mEvent.Zdo.ep = ep;
					shell->mEvent_CB(shell, kEvent_ActiveEpRsp);
					if (*end == ',') {
						p = end + 1;
					} else {
						break;
					}
				}
			}
		}
	}
	return ret;
}

size_t ZigbeeShell::ZdoSimpleDescRspHandler(ZigbeeShell *shell, const char *data, size_t len)
{
	char *p;
	uint16_t dev_addr;
	size_t ret;

	p = strstr((char *)data, ZB_SHELL_MSG_CMD_ERROR);
	if (p != NULL) {
		LOG_ERR("Zdo simple descriptor request finished - Error");
		return p - data + strlen(ZB_SHELL_MSG_CMD_ERROR);
	}
	p = strstr((char *)data, ZB_SHELL_MSG_CMD_DONE);
	if (p == NULL) {
		LOG_DBG("Wait for more response");
		return 0;
	}
	ret = p - data + strlen(ZB_SHELL_MSG_CMD_DONE);
	p = strstr(data, "src_addr=");
	if (p == NULL) {
		return ret;
	} else {
		dev_addr = strtol(p + strlen("src_addr="), NULL, 16);
	}
	p = strstr(data, "ep=");
	if (p == NULL) {
		return ret;
	} else {
		char *end;
		int ep;
		int dev_id;

		p = p + strlen("ep=");
		ep = strtol(p, &end, 10);
		if (p != end) {
			p = strstr(data, "app_dev_id=");
			p = p + strlen("app_dev_id=");
			dev_id = strtol(p, &end, 16);
			if (p != end) {
				LOG_INF("addr: 0x%4hx, active ep: %d device id: %04hx",
					dev_addr, ep, dev_id);
				shell->mEvent.Zdo.addr = dev_addr;
				shell->mEvent.Zdo.ep = ep;
				shell->mEvent.Zdo.dev_id = dev_id;
				shell->mEvent_CB(shell, kEvent_SimpleDescRsp);
			}
		}
	}
	return ret;
}

void ZigbeeShell::UartWorkHandler(struct k_work *work)
{
	size_t ret = 0, parsed = 0, total_parsed = 0;

	ZigbeeShell *c = reinterpret_cast<ZigbeeShell*>((reinterpret_cast<char*>(work) - reinterpret_cast<int>((&(static_cast<ZigbeeShell*>(0)->mUartWork)))));
	if (c->mZigbeeCmd.handler == nullptr) {
		k_sem_give(&c->mCmdSem);
		return;
	}
	memset(c->mParserBuffer, 0, sizeof(c->mParserBuffer));
	ret = ring_buf_peek(&c->mShellRspRb, (uint8_t *)c->mParserBuffer, UNPARSED_BUF_LEN);
	if (ret == 0 || ret > UNPARSED_BUF_LEN) {
		LOG_ERR("Fail to peek ring buffer");
	}
	LOG_HEXDUMP_DBG(c->mParserBuffer, ret, "data to parse");
	parsed = c->mZigbeeCmd.handler(c, c->mParserBuffer, ret);
	if (parsed > 0) {
		k_sem_give(&c->mCmdSem);
	}
	total_parsed = (parsed > total_parsed) ? parsed : total_parsed;

	parsed = c->ParseShellMessage(c->mParserBuffer);
	total_parsed = (parsed > total_parsed) ? parsed : total_parsed;
	if (total_parsed > 0) {
		/* Remove parsed data from ring buffer */
		ret = ring_buf_get(&c->mShellRspRb, NULL, total_parsed);
		if(ret != total_parsed) {
			LOG_ERR("Fail to release from ring buffer");
		} else {
			LOG_INF("Release %d from buffer", ret);
		}
	}
}

int ZigbeeShell::WriteCmd(const char *cmd, ZigbeeResponseHandler rspHandler)
{
	int err = 0;
	size_t len;

	len = strlen(cmd);
	if (len + 2 > MAX_ZIGBEE_CMD_LEN) {
		LOG_INF("Not enough buffer to put Zigbee shell command");
		return -ENOMEM;
	}
	memset(mZigbeeCmd.command, 0, sizeof(mZigbeeCmd.command));
	memcpy(mZigbeeCmd.command, cmd, len);
	mZigbeeCmd.command[len] = '\r';
	mZigbeeCmd.command[len + 1] = '\n';
	mZigbeeCmd.handler = rspHandler;
	err = uart_tx(mUartDev, (const uint8_t *)mZigbeeCmd.command, strlen(mZigbeeCmd.command), 10);
	if (err) {
		LOG_ERR("uart_tx fail: %d", err);
	}
	k_sem_take(&mCmdSem, K_FOREVER);

	return mZigbeeCmd.result;
}

void ZigbeeShell::UartCallback(const struct device *dev, struct uart_event *evt, void *user_data)
{
	int err;
	static uint16_t pos;
	uint32_t ret;
	ZigbeeShell *shell = reinterpret_cast<ZigbeeShell *>(user_data);

	switch (evt->type) {
	case UART_TX_DONE:
		LOG_DBG("Tx sent %d bytes", evt->data.tx.len);
		break;

	case UART_TX_ABORTED:
		LOG_ERR("Tx aborted");
		break;

	case UART_RX_RDY:
		LOG_HEXDUMP_DBG(evt->data.rx.buf, evt->data.rx.len, "Uart RX");
		if (evt->data.rx.len > ring_buf_space_get(&shell->mShellRspRb)) {
			LOG_ERR("Not enough buffer to store data! Drop old data.");
			ret = ring_buf_get(&shell->mShellRspRb,
					   NULL,
					   evt->data.rx.len - ring_buf_space_get(&shell->mShellRspRb));
			LOG_ERR("Drop %d of ring buffer", ret);
		}
		/* Store received data to the ring buffer. */
		ret = ring_buf_put(&shell->mShellRspRb, evt->data.rx.buf + pos, evt->data.rx.len);
		if (ret != evt->data.rx.len) {
			LOG_ERR("Fail to put to ring buffer");
		}
		k_work_submit(&shell->mUartWork);
		pos += evt->data.rx.len;
		break;

	case UART_RX_BUF_REQUEST:
	{
		pos = 0;
		LOG_DBG("RX_REQUEST: %p", shell->mNextUartBuf);
		err = uart_rx_buf_rsp(shell->mUartDev,
				      shell->mNextUartBuf,
				      sizeof(shell->mUartRxBuf[0]));
		if (err) {
			LOG_WRN("UART RX buf rsp: %d", err);
		}
		break;
	}

	case UART_RX_BUF_RELEASED:
		LOG_DBG("RX_RELEASED: %p", evt->data.rx_buf.buf);
		shell->mNextUartBuf = evt->data.rx_buf.buf;
		break;

	case UART_RX_DISABLED:
		LOG_ERR("RX_DISABLED");
		break;

	case UART_RX_STOPPED:
		LOG_ERR("RX_STOPPED");
		break;
	}
}

size_t ZigbeeShell::ParseShellMessage(const char * szMsg)
{
	size_t parsed = 0, total_parsed = 0;
	char *p, *end;

	/* Parse network join message */
	p = strstr(szMsg, ZB_SHELL_MSG_JOIN_NETWORK);
	if (p != nullptr) {
		p = strstr(p, "Extended PAN ID: ");
		if (p != nullptr) {
			p = p + strlen("Extended PAN ID: ");
			memcpy(this->mEvent.Bdb.ext_pan_id, p, EXT_PAN_ID_SIZE);
			this->mEvent.Bdb.ext_pan_id[EXT_PAN_ID_SIZE] = 0;
			p = strstr(p, "PAN ID: ");
			if (p != nullptr) {
				p = p + strlen("PAN ID: ");
				this->mEvent.Bdb.pan_id = strtol(p, &end, 16);
				if (p != end) {
					LOG_INF("Joined network. Ext PAN ID: %s, PAN ID: 0x%04hx",
							this->mEvent.Bdb.ext_pan_id, this->mEvent.Bdb.pan_id);
					p = strstr(szMsg, ZB_SHELL_MSG_REJOIN);
					if (p != nullptr) {
						this->mEvent_CB(this, kEvent_NetworkRejoin);
					} else {
						this->mEvent_CB(this, kEvent_NetworkSteering);
					}
					parsed = end - szMsg;
				}
			}
		}
	}
	total_parsed = (parsed > total_parsed) ? parsed : total_parsed;
	/* Device annoucement message */
	p = strstr(szMsg, ZB_SHELL_MSG_DEVICE_REJOIN);
	if ((p != nullptr) && (p > szMsg)) {
		uint16_t dev_addr;

		parsed = p - mParserBuffer + strlen(ZB_SHELL_MSG_DEVICE_REJOIN) + strlen("xxxx)");
		dev_addr = strtol(p + strlen(ZB_SHELL_MSG_DEVICE_REJOIN), NULL, 16);
		LOG_INF("DEV announce: 0x%04hx", dev_addr);
		this->mEvent.Zdo.addr = dev_addr;
		this->mEvent_CB(this, kEvent_DeviceAnnounceRsp);
	}
	total_parsed = (parsed > total_parsed) ? parsed : total_parsed;
	LOG_DBG("%d bytes parsed", total_parsed);

	return total_parsed;
}

ZigbeeShell::ZigbeeShell(void)
{
	int err;

	k_work_init(&mUartWork, UartWorkHandler);
	k_sem_init(&mCmdSem, 0, 1);

	mUartDev = device_get_binding(CONFIG_ZIGBEE_SHELL_DEVICE_NAME);
	if (!mUartDev) {
		LOG_ERR("Failed to get the device");
	}
	err = uart_callback_set(mUartDev, UartCallback, this);
	if (err != 0) {
		LOG_ERR("Failed to set callback: %d", err);
	}
	mNextUartBuf = mUartRxBuf[1];
	err = uart_rx_enable(mUartDev, mUartRxBuf[0], UART_BUF_SIZE, 50000);
	if (err != 0) {
		LOG_ERR("Failed to enable RX: %d", err);
	}
	ring_buf_init(&mShellRspRb, sizeof(mShellRspBuffer), mShellRspBuffer);

	k_sleep(K_MSEC(10));

	err = WriteCmd("kernel reboot cold", nullptr);
	if (err) {
		LOG_ERR("Fail to reboot Zigbee shell");
	}

	k_sleep(K_MSEC(100));

	err = WriteCmd("shell colors off", ShellRspHandler);
	if (err) {
		LOG_ERR("Fail to shell color");
	}
	err = WriteCmd("shell echo off", ShellRspHandler);
	if (err) {
		LOG_ERR("Fail to set echo");
	}
}

int ZigbeeShell::BdbStart(void)
{
	int err = 0;

	err = WriteCmd("bdb role zc", GeneralRspHandler);
	if (err) {
		return err;
	}
	err = WriteCmd("bdb start", GeneralRspHandler);
	if (err) {
		return err;
	}
	err = WriteCmd("bdb legacy enable", GeneralRspHandler);
	if (err) {
		return err;
	}

	return err;
}

int ZigbeeShell::NetworkSteering(void)
{
	return WriteCmd("bdb start", GeneralRspHandler);
}

int ZigbeeShell::ZdoActiveEpReq(uint16_t addr)
{
	int err = 0;
	char cmd[MAX_ZIGBEE_CMD_LEN];

	LOG_INF("Request active endpoint of addr: 0x%04hx", addr);
	sprintf(cmd, "zdo active_ep 0x%04hx", addr);
	err = WriteCmd(cmd, ZdoActiveEpRspHandler);

	return err;
}

int ZigbeeShell::ZdoSimpleDescReq(uint16_t addr, uint8_t ep)
{
	int err = 0;
	char cmd[MAX_ZIGBEE_CMD_LEN];

	LOG_INF("Request simple descriptor of addr: 0x%04hx ep: %d ", addr, ep);
	sprintf(cmd, "zdo simple_desc_req 0x%04hx %d", addr, ep);
	err = WriteCmd(cmd, ZdoSimpleDescRspHandler);

	return err;
}

int ZigbeeShell::ZclCmd(uint16_t addr, uint8_t ep, uint16_t cluster, uint16_t cmd_id)
{
	int err = 0;
	char cmd[MAX_ZIGBEE_CMD_LEN];

	LOG_INF("Send ZCL cmd. addr: 0x%04hx ep: %d cluster: 0x%04hx cmd_id: 0x%04hx", addr, ep, cluster, cmd_id);
	sprintf(cmd, "zcl cmd -d 0x%04hx %d 0x%04hx 0x%04hx", addr, ep, cluster, cmd_id);
	err = WriteCmd(cmd, GeneralRspHandler);

	return err;
}

int ZigbeeShell::ZclAttrRead(uint16_t addr,
			     uint8_t ep,
			     uint16_t profile_id,
			     enum Cluster_t cluster_id,
			     uint16_t attr_id)
{
	int err = 0;
	char cmd[MAX_ZIGBEE_CMD_LEN];

	LOG_INF("Read ZCL attr addr: 0x%04hx ep: %d cluster: 0x%04hx attr_id: 0x%04hx", addr, ep, cluster_id, attr_id);
	sprintf(cmd, "zcl attr read 0x%04hx %d 0x%04hx 0x%04hx 0x%04hx", addr, ep, cluster_id, profile_id, attr_id);
	mEvent.Zcl.addr = addr;
	mEvent.Zcl.ep = ep;
	mEvent.Zcl.cluster_id = cluster_id;
	mEvent.Zcl.attr_id = attr_id;
	err = WriteCmd(cmd, ZclAttrReadRspHandler);

	return err;
}

int ZigbeeShell::ZdoMatchDesc(uint16_t dst_addr,
			      uint16_t req_addr,
			      uint16_t profile_id,
			      uint8_t in_cluster_cnt,
			      uint16_t *in_clusters,
			      uint8_t out_cluster_cnt,
			      uint16_t *out_clusters)
{
	int err = 0;
	char cmd[MAX_ZIGBEE_CMD_LEN], in_cluster_str[32], out_cluster_str[32];

	memset(cmd, 0, sizeof(cmd));
	memset(in_cluster_str, 0, sizeof(in_cluster_str));
	memset(out_cluster_str, 0, sizeof(out_cluster_str));
	for (auto i = 0; i < in_cluster_cnt; i++) {
		sprintf(in_cluster_str + 2*i, "%hx ", *(in_clusters + 2*i));
	}
	for (auto i = 0; i < out_cluster_cnt; i++) {
		sprintf(out_cluster_str + 2*i, "%hx ", *(out_clusters + 2*i));
	}
	sprintf(cmd, "zdo match_desc 0x%04hx 0x%04hx 0x%04hx %d %s %d %s -t 5",
		dst_addr, req_addr, profile_id, in_cluster_cnt, in_cluster_str, out_cluster_cnt, out_cluster_str);
	err = WriteCmd(cmd, ZdoActiveEpRspHandler);

	return err;
}

void ZigbeeShell::SetEventCallback(zigbee_event_handler_t zigbee_event_handler)
{
	mEvent_CB = zigbee_event_handler;
}
