/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef __OT_COAP_UTILS_H__
#define __OT_COAP_UTILS_H__

#include <openthread/coap.h>
#include <coap_server_client_interface.h>

#define MAX_HOPS 32

/**@brief Type definition of the function used to handle light resource change.
 */
typedef void (*light_request_callback_t)(uint8_t cmd);
typedef void (*provisioning_request_callback_t)();
typedef otCoapCode (*traceroute_request_callback_t)(uint16_t src_rloc16, uint16_t dst_rloc16, uint8_t hops, uint8_t *path);

int ot_coap_init(provisioning_request_callback_t on_provisioning_request,
		 light_request_callback_t on_light_request,
		 traceroute_request_callback_t on_traceroute_request);

void ot_coap_activate_provisioning(void);

void ot_coap_deactivate_provisioning(void);

bool ot_coap_is_provisioning_active(void);

void ot_coap_activate_traceroute_source(void);

int traceroute(uint16_t src_rloc16, uint16_t dst_rloc16, uint8_t hops, uint8_t *path);

#endif
