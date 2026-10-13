#pragma once

/* Frame classification and construction for PTP over Ethernet (IEEE 1588 Annex F)
 * and UDP/IPv4 (Annex D). Pure functions, no ESP-IDF dependencies, host-testable. */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PTP_ETHERTYPE        0x88F7
#define PTP_PORT_EVENT       319
#define PTP_PORT_GENERAL     320
#define PTP_MCAST_IP4        0xE0000181u  /* 224.0.1.129, host order */

#define PTP_FRAME_ETH_HDR    14
#define PTP_FRAME_UDP4_HDR   (14 + 20 + 8)

typedef enum {
    PTP_TRANSPORT_L2 = 0,
    PTP_TRANSPORT_UDP4 = 1,
} ptp_transport_t;

typedef struct {
    ptp_transport_t transport;
    uint8_t src_mac[6];
    uint32_t src_ip;     /*!< host order, 0 for L2 */
    uint32_t dst_ip;     /*!< host order, 0 for L2 */
    uint16_t dst_port;   /*!< 319/320, 0 for L2 */
    size_t msg_offset;   /*!< offset of the PTP message within the frame */
    size_t msg_len;
} ptp_frame_info_t;

/** Classify an Ethernet frame. Returns true if it carries a PTP message. */
bool ptp_frame_classify(const uint8_t *frame, size_t len, ptp_frame_info_t *info);

typedef struct {
    uint8_t src_mac[6];
    uint8_t dst_mac[6];
    uint32_t src_ip;     /*!< host order */
    uint32_t dst_ip;     /*!< host order */
    uint16_t dst_port;   /*!< also used as source port, per Annex D */
    uint8_t ttl;
    uint8_t dscp;
    uint16_t ip_id;
} ptp_udp4_hdr_params_t;

/** Write Ethernet + IPv4 + UDP headers for a payload of payload_len bytes.
 *  out must hold PTP_FRAME_UDP4_HDR bytes. UDP checksum is left 0 (allowed for IPv4). */
void ptp_frame_build_udp4_hdr(uint8_t *out, const ptp_udp4_hdr_params_t *p, size_t payload_len);

/** Multicast MAC for an IPv4 group address (host order). */
void ptp_frame_ip4_mcast_mac(uint32_t group, uint8_t mac[6]);

#ifdef __cplusplus
}
#endif
