#include "phase.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/mcpwm_cap.h"

static const char *TAG = "phase";

#define CAP_RESOLUTION_HZ 160000000u

enum { SRC_GNSS = 0, SRC_EMAC = 1 };

typedef struct {
    uint8_t src;
    uint32_t count;
} edge_t;

static QueueHandle_t s_edges;
static float s_ns_per_count;

static bool IRAM_ATTR on_capture(mcpwm_cap_channel_handle_t chan, const mcpwm_capture_event_data_t *edata, void *user)
{
    BaseType_t woken = pdFALSE;
    edge_t e = { .src = (uint8_t)(uintptr_t)user, .count = edata->cap_value };
    xQueueSendFromISR(s_edges, &e, &woken);
    return woken == pdTRUE;
}

static esp_err_t add_channel(mcpwm_cap_timer_handle_t timer, int gpio, uintptr_t src)
{
    mcpwm_capture_channel_config_t cfg = {
        .gpio_num = gpio,
        .prescale = 1,
        .flags.pos_edge = true,
    };
    mcpwm_cap_channel_handle_t chan;
    ESP_RETURN_ON_ERROR(mcpwm_new_capture_channel(timer, &cfg, &chan), TAG, "channel GPIO %d", gpio);
    mcpwm_capture_event_callbacks_t cbs = { .on_cap = on_capture };
    ESP_RETURN_ON_ERROR(mcpwm_capture_channel_register_event_callbacks(chan, &cbs, (void *)src), TAG, "callbacks");
    return mcpwm_capture_channel_enable(chan);
}

esp_err_t phase_init(const phase_config_t *cfg)
{
    s_edges = xQueueCreate(8, sizeof(edge_t));
    ESP_RETURN_ON_FALSE(s_edges, ESP_ERR_NO_MEM, TAG, "queue");

    mcpwm_capture_timer_config_t tcfg = {
        .group_id = 0,
        .clk_src = MCPWM_CAPTURE_CLK_SRC_PLL160M,
        .resolution_hz = CAP_RESOLUTION_HZ,
    };
    mcpwm_cap_timer_handle_t timer;
    ESP_RETURN_ON_ERROR(mcpwm_new_capture_timer(&tcfg, &timer), TAG, "timer");

    uint32_t res_hz = 0;
    mcpwm_capture_timer_get_resolution(timer, &res_hz);
    s_ns_per_count = 1e9f / (float)res_hz;

    ESP_RETURN_ON_ERROR(add_channel(timer, cfg->gnss_pps_gpio, SRC_GNSS), TAG, "GNSS channel");
    ESP_RETURN_ON_ERROR(add_channel(timer, cfg->emac_pps_gpio, SRC_EMAC), TAG, "EMAC channel");
    ESP_RETURN_ON_ERROR(mcpwm_capture_timer_enable(timer), TAG, "timer enable");
    ESP_RETURN_ON_ERROR(mcpwm_capture_timer_start(timer), TAG, "timer start");

    ESP_LOGI(TAG, "capture at %" PRIu32 " Hz (%.2f ns), GNSS PPS GPIO %d, EMAC PPS GPIO %d",
             res_hz, s_ns_per_count, cfg->gnss_pps_gpio, cfg->emac_pps_gpio);
    return ESP_OK;
}

float phase_resolution_ns(void)
{
    return s_ns_per_count;
}

bool phase_get(phase_sample_t *out, uint32_t timeout_ms)
{
    // Pair each GNSS edge with the most recent EMAC edge. Both arrive once per second,
    // so the nearest EMAC edge is within +-0.5 s of the GNSS edge once the clock is
    // roughly aligned; before that the wrapped error is still a valid phase reading.
    static bool have_emac, have_gnss;
    static uint32_t last_emac, last_gnss;

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    edge_t e;
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        if (now >= deadline || xQueueReceive(s_edges, &e, deadline - now) != pdTRUE) {
            return false;
        }
        if (e.src == SRC_EMAC) {
            last_emac = e.count;
            have_emac = true;
            continue;
        }
        uint32_t interval = have_gnss ? e.count - last_gnss : 0;
        last_gnss = e.count;
        have_gnss = true;
        if (!have_emac) {
            continue;
        }
        // Counter is 32 bits at 160 MHz (wraps every ~26.8 s); unsigned subtraction handles wrap.
        int32_t diff = (int32_t)(e.count - last_emac);
        int64_t ns = (int64_t)((double)diff * s_ns_per_count);
        ns %= 1000000000LL;
        if (ns >= 500000000LL) {
            ns -= 1000000000LL;
        } else if (ns < -500000000LL) {
            ns += 1000000000LL;
        }
        out->error_ns = ns;
        out->gnss_count = e.count;
        out->gnss_interval = interval;
        return true;
    }
}
