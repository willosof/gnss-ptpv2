#include "gm_clock.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_eth_mac_esp.h"

static const char *TAG = "gm_clock";

static esp_eth_mac_t *s_mac;

esp_err_t gm_clock_init(esp_eth_handle_t eth, const gm_clock_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(eth && cfg, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_ERROR(esp_eth_get_mac_instance(eth, &s_mac), TAG, "no MAC instance");

    eth_mac_ptp_config_t ptp_cfg = ETH_MAC_ESP_PTP_DEFAULT_CONFIG();
    if (cfg->use_pll_f80m) {
        ptp_cfg.clk_src = EMAC_PTP_CLK_SRC_PLL_F80M;
        ptp_cfg.clk_src_period_ns = 12.5f;
    }
    ptp_cfg.required_accuracy_ns = cfg->required_accuracy_ns;
    ESP_RETURN_ON_ERROR(esp_eth_mac_ptp_enable(s_mac, &ptp_cfg), TAG, "PTP enable failed");

    // The EMAC's PTP frame filter only matches Layer-2 PTP. Timestamp every frame so
    // UDP/IPv4 PTP (AES67, ST 2059-2) gets hardware RX timestamps too.
    ESP_RETURN_ON_ERROR(esp_eth_mac_enable_ts4all(s_mac, true), TAG, "ts4all failed");

    if (cfg->pps_out_gpio >= 0) {
        ESP_RETURN_ON_ERROR(esp_eth_mac_set_pps_out_gpio(s_mac, cfg->pps_out_gpio), TAG, "PPS GPIO failed");
        ESP_RETURN_ON_ERROR(esp_eth_mac_set_pps_out_freq(s_mac, 0), TAG, "PPS freq failed"); // 0 = 1PPS
    }

    ESP_LOGI(TAG, "PTP clock on %s, resolution %" PRIu32 " ns, PPS out GPIO %d",
             cfg->use_pll_f80m ? "PLL_F80M" : "XTAL", gm_clock_resolution_ns(), cfg->pps_out_gpio);
    return ESP_OK;
}

esp_err_t gm_clock_get(gm_time_t *t)
{
    eth_mac_time_t mt;
    ESP_RETURN_ON_ERROR(esp_eth_mac_get_ptp_time(s_mac, &mt), TAG, "get time failed");
    t->sec = mt.seconds;
    t->nsec = mt.nanoseconds;
    return ESP_OK;
}

esp_err_t gm_clock_set(const gm_time_t *t)
{
    // The EMAC seconds register is 32 bits; it wraps in 2106.
    eth_mac_time_t mt = { .seconds = (uint32_t)t->sec, .nanoseconds = t->nsec };
    return esp_eth_mac_set_ptp_time(s_mac, &mt);
}

esp_err_t gm_clock_adj_freq_ppb(int32_t ppb)
{
    return esp_eth_mac_adj_ptp_time(s_mac, ppb);
}

uint32_t gm_clock_resolution_ns(void)
{
    return esp_eth_mac_get_ts_resolution(s_mac);
}
