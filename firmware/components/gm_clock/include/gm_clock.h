#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_eth_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/** PTP timestamp: seconds since the PTP epoch (TAI) plus nanoseconds. */
typedef struct {
    uint64_t sec;
    uint32_t nsec;
} gm_time_t;

typedef struct {
    int pps_out_gpio;            /*!< GPIO for EMAC PPS0 output, -1 to disable */
    bool use_pll_f80m;           /*!< true: PLL_F80M (12.5 ns), false: XTAL (25 ns) */
    uint32_t required_accuracy_ns;
} gm_clock_config_t;

/** Enable the EMAC PTP clock, timestamping of all frames and the PPS output. */
esp_err_t gm_clock_init(esp_eth_handle_t eth, const gm_clock_config_t *cfg);

esp_err_t gm_clock_get(gm_time_t *t);
esp_err_t gm_clock_set(const gm_time_t *t);

/** Frequency adjustment relative to the nominal rate, in parts per billion. */
esp_err_t gm_clock_adj_freq_ppb(int32_t ppb);

/** Timestamp resolution reported by the EMAC, in ns. */
uint32_t gm_clock_resolution_ns(void);

#ifdef __cplusplus
}
#endif
