#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_eth_driver.h"
#include "esp_netif.h"
#include "ptp_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PTP_NET_MAX_MSG 256

/** A received PTP message with its hardware RX timestamp. */
typedef struct {
    ptp_frame_info_t info;
    uint32_t rx_sec;        /*!< 0/0 if the frame had no timestamp */
    uint32_t rx_nsec;
    uint16_t len;
    uint8_t msg[PTP_NET_MAX_MSG];
} ptp_net_rx_t;

/**
 * Put the PTP classifier in front of lwIP. Call after esp_netif_attach().
 * PTP frames (Layer 2 and UDP 319/320) are consumed and queued; everything else
 * goes to lwIP unchanged.
 */
esp_err_t ptp_net_init(esp_eth_handle_t eth, esp_netif_t *netif);

/** Join 224.0.1.129 via IGMP (also installs the EMAC multicast filter). Call once the
 *  interface has an IPv4 address. */
esp_err_t ptp_net_join_multicast(void);

bool ptp_net_recv(ptp_net_rx_t *out, uint32_t timeout_ms);

/**
 * Send a PTP message over UDP/IPv4.
 * @param dst_ip   host order; PTP_MCAST_IP4 for multicast
 * @param dst_mac  required for unicast, ignored for multicast
 * @param port     PTP_PORT_EVENT or PTP_PORT_GENERAL
 * @param tx_sec/tx_nsec  if non-NULL, request a hardware TX timestamp (blocks until known)
 */
esp_err_t ptp_net_send_udp4(uint32_t dst_ip, const uint8_t dst_mac[6], uint16_t port,
                            const uint8_t *msg, size_t len, uint32_t *tx_sec, uint32_t *tx_nsec);

#ifdef __cplusplus
}
#endif
