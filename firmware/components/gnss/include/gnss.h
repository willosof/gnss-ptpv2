#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int uart_num;
    int rx_gpio;
    int tx_gpio;
    int baud;
} gnss_config_t;

typedef struct {
    bool valid;         /*!< Receiver reports a valid fix and time */
    int64_t utc_sec;    /*!< UTC seconds since 1970-01-01 of the latest time message */
    uint32_t utc_msec;  /*!< Milliseconds part of that time message */
    int64_t rx_us;      /*!< esp_timer time when the message was received */
    uint8_t sats;       /*!< Satellites used, 0 if unknown */
} gnss_status_t;

esp_err_t gnss_init(const gnss_config_t *cfg);
void gnss_get_status(gnss_status_t *out);

#ifdef __cplusplus
}
#endif
