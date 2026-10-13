#include "gnss.h"
#include "nmea.h"
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"

static const char *TAG = "gnss";

static gnss_config_t s_cfg;
static gnss_status_t s_status;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void handle_line(const char *line)
{
    if (!nmea_checksum_ok(line)) {
        return;
    }
    nmea_rmc_t rmc;
    uint8_t sats;
    if (nmea_parse_rmc(line, &rmc)) {
        int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&s_lock);
        s_status.valid = rmc.valid;
        s_status.utc_sec = rmc.utc_sec;
        s_status.utc_msec = rmc.utc_msec;
        s_status.rx_us = now;
        portEXIT_CRITICAL(&s_lock);
    } else if (nmea_parse_gga_sats(line, &sats)) {
        portENTER_CRITICAL(&s_lock);
        s_status.sats = sats;
        portEXIT_CRITICAL(&s_lock);
    }
}

static void gnss_task(void *arg)
{
    char line[128];
    size_t len = 0;
    uint8_t buf[256];
    for (;;) {
        int n = uart_read_bytes(s_cfg.uart_num, buf, sizeof(buf), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '$') {
                len = 0;
            }
            if (c == '\r' || c == '\n') {
                if (len > 0) {
                    line[len] = '\0';
                    handle_line(line);
                    len = 0;
                }
            } else if (len < sizeof(line) - 1) {
                line[len++] = c;
            } else {
                len = 0; // overlong: drop
            }
        }
    }
}

esp_err_t gnss_init(const gnss_config_t *cfg)
{
    s_cfg = *cfg;
    uart_config_t ucfg = {
        .baud_rate = cfg->baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(cfg->uart_num, 4096, 0, 0, NULL, 0), TAG, "uart install");
    ESP_RETURN_ON_ERROR(uart_param_config(cfg->uart_num, &ucfg), TAG, "uart config");
    ESP_RETURN_ON_ERROR(uart_set_pin(cfg->uart_num, cfg->tx_gpio, cfg->rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "uart pins");
    xTaskCreate(gnss_task, "gnss", 4096, NULL, 10, NULL);
    ESP_LOGI(TAG, "UART%d %d baud, RX GPIO %d, TX GPIO %d", cfg->uart_num, cfg->baud, cfg->rx_gpio, cfg->tx_gpio);
    return ESP_OK;
}

void gnss_get_status(gnss_status_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_lock);
}
