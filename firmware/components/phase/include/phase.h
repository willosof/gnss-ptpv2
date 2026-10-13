#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Phase detector: GNSS PPS and the EMAC PPS (looped back from its output pin) are
 * captured by two channels of the same MCPWM capture timer, so both edges share one
 * timebase (PLL_F160M, 6.25 ns per count).
 */
typedef struct {
    int gnss_pps_gpio;
    int emac_pps_gpio;
} phase_config_t;

typedef struct {
    int64_t error_ns;       /*!< t(GNSS PPS) - t(EMAC PPS), wrapped into [-500 ms, +500 ms) */
    uint32_t gnss_count;    /*!< Raw capture counter value of the GNSS edge */
    uint32_t gnss_interval; /*!< Counts between this GNSS edge and the previous one */
} phase_sample_t;

esp_err_t phase_init(const phase_config_t *cfg);

/** Block until the next paired sample is available (one per second). */
bool phase_get(phase_sample_t *out, uint32_t timeout_ms);

/** Capture resolution in ns per count. */
float phase_resolution_ns(void);

#ifdef __cplusplus
}
#endif
