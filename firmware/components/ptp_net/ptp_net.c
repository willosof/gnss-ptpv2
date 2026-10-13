#include "ptp_net.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_eth_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"

static const char *TAG = "ptp_net";

#define RX_QUEUE_LEN   16
#define PTP_TTL        1
#define PTP_DSCP       46   /* EF, as recommended by AES67 */

static esp_eth_handle_t s_eth;
static esp_netif_t *s_netif;
static QueueHandle_t s_rxq;
static uint8_t s_mac[6];
static uint16_t s_ip_id;
static uint32_t s_rx_dropped;

static esp_err_t eth_input(esp_eth_handle_t eth, uint8_t *buffer, uint32_t length, void *priv, void *info)
{
    ptp_frame_info_t fi;
    if (!ptp_frame_classify(buffer, length, &fi)) {
        return esp_netif_receive(s_netif, buffer, length, NULL);
    }
    // Runs in the EMAC RX task: copy what we need and release the buffer right away.
    ptp_net_rx_t *rx = malloc(sizeof(*rx));
    if (rx) {
        const eth_mac_time_t *ts = info;
        rx->info = fi;
        rx->rx_sec = ts ? ts->seconds : 0;
        rx->rx_nsec = ts ? ts->nanoseconds : 0;
        rx->len = fi.msg_len > PTP_NET_MAX_MSG ? PTP_NET_MAX_MSG : (uint16_t)fi.msg_len;
        memcpy(rx->msg, buffer + fi.msg_offset, rx->len);
        if (xQueueSend(s_rxq, &rx, 0) != pdTRUE) {
            free(rx);
            s_rx_dropped++;
        }
    } else {
        s_rx_dropped++;
    }
    free(buffer);
    return ESP_OK;
}

esp_err_t ptp_net_init(esp_eth_handle_t eth, esp_netif_t *netif)
{
    s_eth = eth;
    s_netif = netif;
    s_rxq = xQueueCreate(RX_QUEUE_LEN, sizeof(ptp_net_rx_t *));
    ESP_RETURN_ON_FALSE(s_rxq, ESP_ERR_NO_MEM, TAG, "queue");
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(eth, ETH_CMD_G_MAC_ADDR, s_mac), TAG, "get MAC");
    // Replaces the input path installed by the esp_netif glue; we forward non-PTP frames.
    ESP_RETURN_ON_ERROR(esp_eth_update_input_path_info(eth, eth_input, NULL), TAG, "input path");
    return ESP_OK;
}

esp_err_t ptp_net_join_multicast(void)
{
    esp_netif_ip_info_t ip;
    ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(s_netif, &ip), TAG, "no IP");
    // A socket is the simplest portable way to drive IGMP membership in lwIP. PTP frames
    // never reach it: the classifier consumes them first.
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ESP_RETURN_ON_FALSE(sock >= 0, ESP_FAIL, TAG, "socket");
    struct ip_mreq mreq = {
        .imr_multiaddr.s_addr = htonl(PTP_MCAST_IP4),
        .imr_interface.s_addr = ip.ip.addr,
    };
    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        close(sock);
        ESP_LOGE(TAG, "IP_ADD_MEMBERSHIP failed: %d", errno);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "joined 224.0.1.129");
    return ESP_OK; // socket intentionally kept open to hold the membership
}

bool ptp_net_recv(ptp_net_rx_t *out, uint32_t timeout_ms)
{
    ptp_net_rx_t *rx;
    if (xQueueReceive(s_rxq, &rx, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }
    memcpy(out, rx, sizeof(*out));
    free(rx);
    return true;
}

esp_err_t ptp_net_send_udp4(uint32_t dst_ip, const uint8_t dst_mac[6], uint16_t port,
                            const uint8_t *msg, size_t len, uint32_t *tx_sec, uint32_t *tx_nsec)
{
    esp_netif_ip_info_t ip;
    ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(s_netif, &ip), TAG, "no IP");

    ptp_udp4_hdr_params_t p = {
        .src_ip = ntohl(ip.ip.addr),
        .dst_ip = dst_ip,
        .dst_port = port,
        .ttl = PTP_TTL,
        .dscp = PTP_DSCP,
        .ip_id = s_ip_id++,
    };
    memcpy(p.src_mac, s_mac, 6);
    if ((dst_ip & 0xf0000000u) == 0xe0000000u) {
        ptp_frame_ip4_mcast_mac(dst_ip, p.dst_mac);
    } else {
        ESP_RETURN_ON_FALSE(dst_mac, ESP_ERR_INVALID_ARG, TAG, "unicast needs dst MAC");
        memcpy(p.dst_mac, dst_mac, 6);
    }
    uint8_t hdr[PTP_FRAME_UDP4_HDR];
    ptp_frame_build_udp4_hdr(hdr, &p, len);

    // Minimum Ethernet payload is 46 bytes; short frames are padded by the MAC.
    eth_mac_time_t ts = { 0 };
    esp_err_t err = esp_eth_transmit_ctrl_vargs(s_eth, (tx_sec || tx_nsec) ? &ts : NULL, 4,
                                                hdr, (uint32_t)sizeof(hdr), msg, (uint32_t)len);
    ESP_RETURN_ON_ERROR(err, TAG, "transmit");
    if (tx_sec) {
        *tx_sec = ts.seconds;
    }
    if (tx_nsec) {
        *tx_nsec = ts.nanoseconds;
    }
    return ESP_OK;
}
