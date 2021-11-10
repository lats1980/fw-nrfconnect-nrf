.. _SLM_IPV6_changelog:

IPV6 changes
############

.. contents::
   :local:
   :depth: 1

The following lists changes about IPV6.

Support more Socket options
***************************

Support socket options from nrf_modem 1.2.0
Set() options

- SO_BINDTODEVICE

- SO_REUSEADDR

- SO_SNDTIMEO

- SO_SILENCE_ALL, SO_IP_ECHO_REPLY, SO_IPV6_ECHO_REPLY

- SO_TCP_SRV_SESSTIMEO:

- SO_RAI_LAST, SO_RAI_NO_DATA, SO_RAI_ONE_RESP, SO_RAI_ONGOING

- SO_RAI_WAIT_MORE

Get() options

- SO_ERROR (currently pseudo-implementation)

- SO_SNDTIMEO

- SO_SILENCE_ALL, SO_IP_ECHO_REPLY, SO_IPV6_ECHO_REPLY

* - SO_TCP_SRV_SESSTIMEO

Request MFWv1.3.0 and nrf_modem 1.3.1, or later version.

Add IPv6 support to ICMP Ping
*****************************

Reworked Ping implementation to support IPv6 target, by hostname or IPv6 address.

Affect two SLM AT command:

  AT#XPING

  AT#XGETADDRINFO

Rework of socket service
************************

Rename SLM TCPIP service to Socket service.

Add secure socket AT command AT#XSSOCKET.

Add secure socket option AT command AT#XSSOCKETOPT.

Add IPv6 support.

Add timeout for blocking APIs.

No closing of socket in send/recv error.

Adjust socket TX/RX buffer size.

Add socket-specific document page.

Related changes in other service modules.

Rework of TCPIP proxy service
*****************************

Remove two customer project feature, AT#XTCPFILTER and AT#XTCPRECV

Add IPv6 support

Do not disconnect connection when send/receive fails

Enter datamode only in #XTCPSEND/#XUDPSEND, align in all modules

Do not disconnect TCP client when quitting datamode

Add new AT#XTCPHANGUP to disconnect incoming connection

Fix leakage in ICMP and socket service (static code analysis)

Add IPv6 support to HTTP client
*******************************

Reworked the #XHTTPCCON implementation to add IPv6 support

Piggyback: code optimization in at_host and TCPIP proxy

Add IPv6 support to MQTT client
*******************************

Rework of #XMQTTCON implementation:

- Add IPv6 support to MQTT broker connection

- Start MQTT thread dynamically so as to support re-connection

- Use no native TLS, but TLS on modem side only

Unifying the way of waiting for thread quit

- Align the implementation in TCPIP proxy, MQTT


ftp_client: Add IPv6 support to FTP client
******************************************

Try IPv6 parsing first, if fails try IPv4 parsing next.

Adjust control and data socket based on address family.

Update QUIT command as some server does not reply.

Update KEEPALIVE to avoid unnecessary NOOP.

Add IPv6 support to FTP client
******************************

Adding IPv6 to lib_ftp_client.

agps: refactor SUPL socket handling
***********************************

Refactored SUPL socked handling and added support for IPv6.

Add util function for DNS lookup
********************************

Add util_resolve_host() to resolve remote host by name or address.

Prepared for future multiple PDN support.
