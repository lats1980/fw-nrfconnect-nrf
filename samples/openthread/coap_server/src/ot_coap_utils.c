/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/logging/log.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/net_l2.h>
#include <zephyr/net/openthread.h>
#include <openthread/ip6.h>
#include <openthread/message.h>
#include <openthread/thread.h>

#include "ot_coap_utils.h"

#ifdef CONFIG_OPENTHREAD_FTD
#include <openthread/thread_ftd.h>
#endif

LOG_MODULE_REGISTER(ot_coap_utils, CONFIG_OT_COAP_UTILS_LOG_LEVEL);

struct traceroute_ctx {
	struct k_work traceroute_work;
	uint16_t src_rloc16;
	uint16_t dst_rloc16;
	uint8_t hops;
	uint8_t path[MAX_HOPS]; // Store up to 32 hops
	bool in_progress;
};
static struct traceroute_ctx traceroute_process;

struct server_context {
	struct otInstance *ot;
	bool provisioning_enabled;
	bool traceroute_source_enabled;
	light_request_callback_t on_light_request;
	provisioning_request_callback_t on_provisioning_request;
	traceroute_request_callback_t on_traceroute_request;
};

static struct server_context srv_context = {
	.ot = NULL,
	.provisioning_enabled = false,
	.on_light_request = NULL,
	.on_provisioning_request = NULL,
	.on_traceroute_request = NULL,
};

/**@brief Definition of CoAP resources for provisioning. */
static otCoapResource provisioning_resource = {
	.mUriPath = PROVISIONING_URI_PATH,
	.mHandler = NULL,
	.mContext = NULL,
	.mNext = NULL,
};

/**@brief Definition of CoAP resources for light. */
static otCoapResource light_resource = {
	.mUriPath = LIGHT_URI_PATH,
	.mHandler = NULL,
	.mContext = NULL,
	.mNext = NULL,
};

/**@brief Definition of CoAP resources for traceroute. */
static otCoapResource traceroute_resource = {
	.mUriPath = TRACEROUTE_URI_PATH,
	.mHandler = NULL,
	.mContext = NULL,
	.mNext = NULL,
};

static otError coap_utils_send_response(otMessage *request_message,
					const otMessageInfo *message_info,
					otCoapCode code)
{
	otError error = OT_ERROR_NO_BUFS;
	otMessage *response;

	response = otCoapNewMessage(srv_context.ot, NULL);
	if (response == NULL) {
		goto end;
	}

	error = otCoapMessageInitResponse(response, request_message,
					  OT_COAP_TYPE_ACKNOWLEDGMENT, code);
	if (error != OT_ERROR_NONE) {
		goto end;
	}

	error = otCoapSendResponse(srv_context.ot, response, message_info);

end:
	if (error != OT_ERROR_NONE && response != NULL) {
		otMessageFree(response);
	}

	return error;
}

static void handle_traceroute_response(void *context, otMessage *message, const otMessageInfo *message_info, otError error)
{
	ARG_UNUSED(context);
	ARG_UNUSED(message_info);

	if (error != OT_ERROR_NONE) {
		LOG_ERR("Failed to send traceroute request: %d", error);
		goto exit;
	}

	if (otCoapMessageGetCode(message) == OT_COAP_CODE_CHANGED) {
		LOG_INF("traceroute response received");
	} else {
		LOG_ERR("Unexpected CoAP code in traceroute response: %d",
			otCoapMessageGetCode(message));
	}
exit:
	traceroute_process.in_progress = false;
}

static void traceroute_request_send(struct k_work *item)
{
	ARG_UNUSED(item);
	struct traceroute_ctx *traceroute_process = CONTAINER_OF((struct k_work *)item, struct traceroute_ctx, traceroute_work);

	otError error = OT_ERROR_NO_BUFS;
	otMessage *message;
	char uri[] = "traceroute";
	otMessageInfo message_info;
	otIp6Address peer_addr;
	uint16_t next_hop_rloc16;

	message = otCoapNewMessage(srv_context.ot, NULL);
	if (message == NULL) {
		goto end;
	}

	otCoapMessageInit(message, OT_COAP_TYPE_CONFIRMABLE, OT_COAP_CODE_POST);
	otCoapMessageGenerateToken(message, OT_COAP_DEFAULT_TOKEN_LENGTH);
	error = otCoapMessageAppendUriPathOptions(message, uri);
	if (error != OT_ERROR_NONE) {
		goto end;
	}

	error = otCoapMessageSetPayloadMarker(message);
	if (error != OT_ERROR_NONE) {
		goto end;
	}
	error = otMessageAppend(message, &traceroute_process->src_rloc16, sizeof(traceroute_process->src_rloc16));
	if (error != OT_ERROR_NONE) {
		goto end;
	}
	error = otMessageAppend(message, &traceroute_process->dst_rloc16, sizeof(traceroute_process->dst_rloc16));
	if (error != OT_ERROR_NONE) {
		goto end;
	}
	error = otMessageAppend(message, &traceroute_process->hops, sizeof(traceroute_process->hops));
	if (error != OT_ERROR_NONE) {
		goto end;
	}
	if (traceroute_process->hops > 0) {
		// Append the path if hops > 0
		if (traceroute_process->hops > sizeof(traceroute_process->path)) {
			LOG_ERR("Path size exceeds maximum allowed size");
			goto end;
		}
		error = otMessageAppend(message, &traceroute_process->path, traceroute_process->hops);
		if (error != OT_ERROR_NONE) {
			goto end;
		}
	}
	memset(&message_info, 0, sizeof(message_info));
	memset(&peer_addr, 0, sizeof(peer_addr));
	message_info.mPeerPort = COAP_PORT;

	if (traceroute_process->src_rloc16 == TRACEROUTE_INIT_ADDR) {
		// Send multicast traceroute request to all nodes (ff03::1, Realm-Local All Nodes)
		peer_addr.mFields.m8[0] = 0xff; // Multicast
		peer_addr.mFields.m8[1] = 0x03; // Scope: Realm-Local
		peer_addr.mFields.m8[15] = 0x01; // Group ID: All Nodes
		message_info.mHopLimit = 5;
		message_info.mAllowZeroHopLimit = false;
		message_info.mMulticastLoop = false; // Allow multicast loop
		message_info.mPeerAddr = peer_addr;
		error = otCoapSendRequest(srv_context.ot, message, &message_info, NULL, NULL);
		traceroute_process->in_progress = false;
	} else {
#ifdef CONFIG_OPENTHREAD_FTD
		uint8_t path_cost;
		// Get next hop RLOC16 and path cost
		otThreadGetNextHopAndPathCost(srv_context.ot, traceroute_process->dst_rloc16, &next_hop_rloc16, &path_cost);
#else
		// Get parent RLOC16 for the next hop
		otRouterInfo parent_info;
		error = otThreadGetParentInfo(srv_context.ot, &parent_info);
		if (error != OT_ERROR_NONE) {
			LOG_ERR("Failed to get parent info: %d", error);
			goto end;
		}
		next_hop_rloc16 = parent_info.mRloc16;
#endif
		// Get mesh prefix
		const otIp6NetworkPrefix *prefix = otThreadGetMeshLocalPrefix(srv_context.ot);

		// Set prefix (first 8 bytes)
		memcpy(peer_addr.mFields.m8, prefix->m8, OT_IP6_PREFIX_SIZE);

		// Set Interface Identifier (IID) from next_hop_rloc16
		// IID: 0000:00ff:fe00:xxxx, where xxxx is next_hop_rloc16
		peer_addr.mFields.m8[8]  = 0x00;
		peer_addr.mFields.m8[9]  = 0x00;
		peer_addr.mFields.m8[10] = 0x00;
		peer_addr.mFields.m8[11] = 0xff;
		peer_addr.mFields.m8[12] = 0xfe;
		peer_addr.mFields.m8[13] = 0x00;
		peer_addr.mFields.m8[14] = (uint8_t)(next_hop_rloc16 >> 8);
		peer_addr.mFields.m8[15] = (uint8_t)(next_hop_rloc16 & 0xff);
		message_info.mPeerAddr = peer_addr;
		error = otCoapSendRequest(srv_context.ot, message, &message_info, &handle_traceroute_response, NULL);
	}

end:
	if (error != OT_ERROR_NONE && message != NULL) {
		LOG_ERR("Failed to send modem state: %d", error);
		otMessageFree(message);
	}

	return;
}

static otError provisioning_response_send(otMessage *request_message,
					  const otMessageInfo *message_info)
{
	otError error = OT_ERROR_NO_BUFS;
	otMessage *response;
	const void *payload;
	uint16_t payload_size;

	response = otCoapNewMessage(srv_context.ot, NULL);
	if (response == NULL) {
		goto end;
	}

	otCoapMessageInit(response, OT_COAP_TYPE_NON_CONFIRMABLE,
			  OT_COAP_CODE_CONTENT);

	error = otCoapMessageSetToken(
		response, otCoapMessageGetToken(request_message),
		otCoapMessageGetTokenLength(request_message));
	if (error != OT_ERROR_NONE) {
		goto end;
	}

	error = otCoapMessageSetPayloadMarker(response);
	if (error != OT_ERROR_NONE) {
		goto end;
	}

	payload = otThreadGetMeshLocalEid(srv_context.ot);
	payload_size = sizeof(otIp6Address);

	error = otMessageAppend(response, payload, payload_size);
	if (error != OT_ERROR_NONE) {
		goto end;
	}

	error = otCoapSendResponse(srv_context.ot, response, message_info);

	LOG_HEXDUMP_INF(payload, payload_size, "Sent provisioning response:");

end:
	if (error != OT_ERROR_NONE && response != NULL) {
		otMessageFree(response);
	}

	return error;
}

static void provisioning_request_handler(void *context, otMessage *message,
					 const otMessageInfo *message_info)
{
	otError error;
	otMessageInfo msg_info;

	ARG_UNUSED(context);

	if (!srv_context.provisioning_enabled) {
		LOG_WRN("Received provisioning request but provisioning "
			"is disabled");
		return;
	}

	LOG_INF("Received provisioning request");

	if ((otCoapMessageGetType(message) == OT_COAP_TYPE_NON_CONFIRMABLE) &&
	    (otCoapMessageGetCode(message) == OT_COAP_CODE_GET)) {
		msg_info = *message_info;
		memset(&msg_info.mSockAddr, 0, sizeof(msg_info.mSockAddr));

		error = provisioning_response_send(message, &msg_info);
		if (error == OT_ERROR_NONE) {
			srv_context.on_provisioning_request();
			srv_context.provisioning_enabled = false;
		}
	}
}

static void light_request_handler(void *context, otMessage *message,
				  const otMessageInfo *message_info)
{
	uint8_t command;

	ARG_UNUSED(context);

	if (otCoapMessageGetType(message) != OT_COAP_TYPE_NON_CONFIRMABLE) {
		LOG_ERR("Light handler - Unexpected type of message");
		goto end;
	}

	if (otCoapMessageGetCode(message) != OT_COAP_CODE_PUT) {
		LOG_ERR("Light handler - Unexpected CoAP code");
		goto end;
	}

	if (otMessageRead(message, otMessageGetOffset(message), &command, 1) !=
	    1) {
		LOG_ERR("Light handler - Missing light command");
		goto end;
	}

	LOG_INF("Received light request: %c", command);

	srv_context.on_light_request(command);

end:
	return;
}

static void traceroute_request_handler(void *context, otMessage *message,
				  const otMessageInfo *message_info)
{
	static uint16_t message_id;
	uint16_t src_rloc16, dst_rloc16;
	uint8_t hops;
	uint8_t path[MAX_HOPS];
	otError err;
	otCoapCode code;

	ARG_UNUSED(context);

	if (otCoapMessageGetMessageId(message) == message_id) {
		LOG_WRN("Received the same message id");
		return;
	}
	message_id = otCoapMessageGetMessageId(message);

	if (otCoapMessageGetType(message) != OT_COAP_TYPE_CONFIRMABLE) {
		LOG_ERR("traceroute handler - Unexpected type of message");
		goto end;
	}

	if (otCoapMessageGetCode(message) != OT_COAP_CODE_POST) {
		LOG_ERR("traceroute handler - Unexpected CoAP code");
		goto end;
	}

	if (otMessageRead(message, otMessageGetOffset(message), &src_rloc16, sizeof(src_rloc16)) !=
	    sizeof(src_rloc16)) {
		LOG_ERR("traceroute handler - Missing src rloc16 address");
		goto end;
	}

	if (otMessageRead(message, otMessageGetOffset(message) + sizeof(src_rloc16), &dst_rloc16, sizeof(dst_rloc16)) !=
	    sizeof(dst_rloc16)) {
		LOG_ERR("traceroute handler - Missing dst rloc16 address");
		goto end;
	}

	if (otMessageRead(message, otMessageGetOffset(message) + sizeof(src_rloc16) + sizeof(dst_rloc16), &hops, sizeof(hops)) !=
	    sizeof(hops)) {
		LOG_ERR("traceroute handler - Missing hops count");
		goto end;
	}

	LOG_INF("Received traceroute request for dst rloc16: 0x%04x, src rloc16: 0x%04x, hops: %d",
		dst_rloc16, src_rloc16, hops);

	memset(path, 0, sizeof(path));
	if (hops > 0) {
		if (otMessageRead(message, otMessageGetOffset(message) + sizeof(src_rloc16) + sizeof(dst_rloc16) + sizeof(hops), path, hops) !=
		    hops) {
			LOG_ERR("traceroute handler - Missing path");
			goto end;
		}
		LOG_HEXDUMP_INF(path, hops, "Current path: ");
	}
	if (src_rloc16 == TRACEROUTE_INIT_ADDR) {
		if (srv_context.traceroute_source_enabled) {
			// Multicast traceroute request, replace the source RLOC16 with the local RLOC16
			src_rloc16 = otThreadGetRloc16(srv_context.ot);
			LOG_INF("Multicast traceroute request, using local RLOC16: 0x%04x", src_rloc16);
			srv_context.traceroute_source_enabled = false;
		} else {
			// Ignore traceroute request from multicast source if traceroute source is not enabled
			goto end;
		}
	} else {
#ifdef CONFIG_OPENTHREAD_FTD
		uint16_t local_rloc16 = otThreadGetRloc16(openthread_get_default_context()->instance);
		if (hops >= MAX_HOPS) {
			LOG_ERR("Hops count exceeds maximum allowed size: %d", hops);
			return;
		}
		path[hops] = (local_rloc16 >> 10) & 0xFF;
		hops++;
#endif
	}
	code = srv_context.on_traceroute_request(src_rloc16, dst_rloc16, hops, path);
	if (code == OT_COAP_CODE_EMPTY) {
		LOG_ERR("traceroute handler - No response code returned");
		goto end;
	}
	err = coap_utils_send_response(message, message_info, code);
	if (err != OT_ERROR_NONE) {
		LOG_ERR("Failed to send traceroute response: %d", err);
	}

end:
	return;
}

static void coap_default_handler(void *context, otMessage *message,
				 const otMessageInfo *message_info)
{
	ARG_UNUSED(context);
	ARG_UNUSED(message);
	ARG_UNUSED(message_info);

	LOG_INF("Received CoAP message that does not match any request "
		"or resource");
}

void ot_coap_activate_provisioning(void)
{
	srv_context.provisioning_enabled = true;
}

void ot_coap_deactivate_provisioning(void)
{
	srv_context.provisioning_enabled = false;
}

bool ot_coap_is_provisioning_active(void)
{
	return srv_context.provisioning_enabled;
}

void ot_coap_activate_traceroute_source(void)
{
	srv_context.traceroute_source_enabled = true;
}

int ot_coap_init(provisioning_request_callback_t on_provisioning_request,
		 light_request_callback_t on_light_request,
		 traceroute_request_callback_t on_traceroute_request)
{
	otError error;

	srv_context.provisioning_enabled = false;
	srv_context.on_provisioning_request = on_provisioning_request;
	srv_context.on_light_request = on_light_request;
	srv_context.on_traceroute_request = on_traceroute_request;

	srv_context.ot = openthread_get_default_instance();
	if (!srv_context.ot) {
		LOG_ERR("There is no valid OpenThread instance");
		error = OT_ERROR_FAILED;
		goto end;
	}

	provisioning_resource.mContext = srv_context.ot;
	provisioning_resource.mHandler = provisioning_request_handler;

	light_resource.mContext = srv_context.ot;
	light_resource.mHandler = light_request_handler;

	traceroute_resource.mContext = srv_context.ot;
	traceroute_resource.mHandler = traceroute_request_handler;

	otCoapSetDefaultHandler(srv_context.ot, coap_default_handler, NULL);
	otCoapAddResource(srv_context.ot, &light_resource);
	otCoapAddResource(srv_context.ot, &provisioning_resource);
	otCoapAddResource(srv_context.ot, &traceroute_resource);

	error = otCoapStart(srv_context.ot, COAP_PORT);
	if (error != OT_ERROR_NONE) {
		LOG_ERR("Failed to start OT CoAP. Error: %d", error);
		goto end;
	}

	k_work_init(&traceroute_process.traceroute_work, traceroute_request_send);

end:
	return error == OT_ERROR_NONE ? 0 : 1;
}

int traceroute(uint16_t src_rloc16, uint16_t dst_rloc16, uint8_t hops, uint8_t *path)
{
	if (traceroute_process.in_progress) {
		LOG_WRN("traceroute is already active");
		return -EALREADY;
	}
	if (hops > MAX_HOPS) {
		LOG_ERR("Maximum hops exceeded: %d", hops);
		return -EINVAL;
	}
	if (hops > 0 && path == NULL) {
		LOG_ERR("Path must be provided if hops is greater than 0");
		return -EINVAL;
	}
	traceroute_process.src_rloc16 = src_rloc16;
	traceroute_process.dst_rloc16 = dst_rloc16;
	traceroute_process.in_progress = true;
	traceroute_process.hops = hops;
	memset(traceroute_process.path, 0, sizeof(traceroute_process.path));
	if (hops > 0 && path != NULL) {
		memcpy(traceroute_process.path, path, hops);
	}
	k_work_submit(&traceroute_process.traceroute_work);
	return 0;
}
