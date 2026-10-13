/* M0 bring-up: Ethernet + PTP clock + PPS out, GNSS NMEA, PPS phase capture, and a
 * UDP/IPv4 PTP RX/TX timestamp probe. See PLAN-A.md, milestone M0. */

#include <string.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "ethernet_init.h"
#include "gm_clock.h"
#include "phase.h"
#include "gnss.h"
#include "ptp_net.h"

static const char *TAG = "main";

#define GOT_IP_BIT BIT0
static EventGroupHandle_t s_events;

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *ev = data;
    ESP_LOGI(TAG, "IP " IPSTR, IP2STR(&ev->ip_info.ip));
    xEventGroupSetBits(s_events, GOT_IP_BIT);
}

static esp_eth_handle_t start_ethernet(esp_netif_t **out_netif)
{
    esp_eth_handle_t *handles;
    uint8_t count;
    ESP_ERROR_CHECK(ethernet_init_all(&handles, &count));
    ESP_ERROR_CHECK(count == 1 ? ESP_OK : ESP_ERR_INVALID_STATE);

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&cfg);
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(handles[0])));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL));

    // Must come after esp_netif_attach(): it replaces the glue's input path.
    ESP_ERROR_CHECK(ptp_net_init(handles[0], netif));
    ESP_ERROR_CHECK(esp_eth_start(handles[0]));
    *out_netif = netif;
    return handles[0];
}

static const char *msg_type_name(uint8_t t)
{
    static const char *names[16] = {
        "Sync", "Delay_Req", "Pdelay_Req", "Pdelay_Resp", "?4", "?5", "?6", "?7",
        "Follow_Up", "Delay_Resp", "Pdelay_Resp_FU", "Announce", "Signaling", "Management", "?E", "?F",
    };
    return names[t & 0x0f];
}

static void ptp_rx_log_task(void *arg)
{
    ptp_net_rx_t rx;
    for (;;) {
        if (!ptp_net_recv(&rx, portMAX_DELAY)) {
            continue;
        }
        uint8_t type = rx.msg[0] & 0x0f;
        uint8_t domain = rx.msg[4];
        uint16_t seq = (uint16_t)(rx.msg[30] << 8 | rx.msg[31]);
        if (rx.info.transport == PTP_TRANSPORT_UDP4) {
            uint32_t s = rx.info.src_ip;
            ESP_LOGI(TAG, "RX udp4 %u.%u.%u.%u:%u %-12s dom %3u seq %5u ts %" PRIu32 ".%09" PRIu32,
                     (unsigned)(s >> 24), (unsigned)(s >> 16 & 0xff), (unsigned)(s >> 8 & 0xff), (unsigned)(s & 0xff),
                     rx.info.dst_port, msg_type_name(type), domain, seq, rx.rx_sec, rx.rx_nsec);
        } else {
            ESP_LOGI(TAG, "RX l2   %02x:%02x:%02x:%02x:%02x:%02x %-12s dom %3u seq %5u ts %" PRIu32 ".%09" PRIu32,
                     rx.info.src_mac[0], rx.info.src_mac[1], rx.info.src_mac[2], rx.info.src_mac[3],
                     rx.info.src_mac[4], rx.info.src_mac[5], msg_type_name(type), domain, seq, rx.rx_sec, rx.rx_nsec);
        }
    }
}

#if CONFIG_GM_M0_TX_PROBE
/* Signaling message (IEEE 1588-2008 13.12), domain 99, no TLVs. Real PTP devices on
 * other domains ignore it; we only want the hardware TX timestamp. */
static void ptp_tx_probe_task(void *arg)
{
    uint8_t msg[44] = { 0 };
    msg[0] = 0x0c;                 // messageType Signaling
    msg[1] = 0x02;                 // versionPTP 2
    msg[2] = 0;
    msg[3] = sizeof(msg);          // messageLength
    msg[4] = 99;                   // domainNumber
    msg[33] = 0x7f;                // logMessageInterval
    memset(msg + 34, 0xff, 10);    // targetPortIdentity: all ports
    uint16_t seq = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        msg[30] = seq >> 8;
        msg[31] = seq & 0xff;
        uint32_t sec = 0, nsec = 0;
        esp_err_t err = ptp_net_send_udp4(PTP_MCAST_IP4, NULL, PTP_PORT_GENERAL, msg, sizeof(msg), &sec, &nsec);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "TX probe seq %5u ts %" PRIu32 ".%09" PRIu32 "%s", seq, sec, nsec,
                     (sec | nsec) ? "" : " (no timestamp)");
        }
        seq++;
    }
}
#endif

static void phase_log_task(void *arg)
{
    phase_sample_t s;
    gnss_status_t g;
    for (;;) {
        if (!phase_get(&s, 2000)) {
            ESP_LOGW(TAG, "no PPS pair in 2 s (GNSS PPS or loopback wire missing?)");
            continue;
        }
        gnss_get_status(&g);
        gm_time_t now;
        gm_clock_get(&now);
        ESP_LOGI(TAG, "phase %+11" PRId64 " ns  interval %10" PRIu32 "  gnss %s utc %" PRId64 " sats %u  ptp %" PRIu64 ".%09" PRIu32,
                 s.error_ns, s.gnss_interval, g.valid ? "ok " : "---", g.utc_sec, g.sats, now.sec, now.nsec);
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_events = xEventGroupCreate();

    esp_netif_t *netif;
    esp_eth_handle_t eth = start_ethernet(&netif);

    gm_clock_config_t ccfg = {
        .pps_out_gpio = CONFIG_GM_PPS_OUT_GPIO,
        .use_pll_f80m = IS_ENABLED(CONFIG_GM_PTP_CLK_SRC_PLL_F80M),
        .required_accuracy_ns = CONFIG_GM_PTP_REQUIRED_ACCURACY_NS,
    };
    ESP_ERROR_CHECK(gm_clock_init(eth, &ccfg));

    gnss_config_t gcfg = {
        .uart_num = CONFIG_GM_GNSS_UART_NUM,
        .rx_gpio = CONFIG_GM_GNSS_RX_GPIO,
        .tx_gpio = CONFIG_GM_GNSS_TX_GPIO,
        .baud = CONFIG_GM_GNSS_UART_BAUD,
    };
    ESP_ERROR_CHECK(gnss_init(&gcfg));

    phase_config_t pcfg = {
        .gnss_pps_gpio = CONFIG_GM_GNSS_PPS_GPIO,
        .emac_pps_gpio = CONFIG_GM_PPS_LOOP_GPIO,
    };
    ESP_ERROR_CHECK(phase_init(&pcfg));
    xTaskCreate(phase_log_task, "phase_log", 4096, NULL, 5, NULL);

    xTaskCreate(ptp_rx_log_task, "ptp_rx_log", 4096, NULL, 6, NULL);
    xEventGroupWaitBits(s_events, GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_ERROR_CHECK(ptp_net_join_multicast());
#if CONFIG_GM_M0_TX_PROBE
    xTaskCreate(ptp_tx_probe_task, "ptp_tx_probe", 4096, NULL, 6, NULL);
#endif
}
