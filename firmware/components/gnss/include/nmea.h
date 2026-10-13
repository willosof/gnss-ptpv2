#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Fields of a parsed RMC sentence. */
typedef struct {
    bool valid;         /*!< Status 'A' */
    int64_t utc_sec;    /*!< UTC seconds since 1970-01-01 */
    uint32_t utc_msec;
} nmea_rmc_t;

/** True if the sentence has a correct "*hh" checksum. */
bool nmea_checksum_ok(const char *line);

/** Parse "$xxRMC,...". Returns false if not an RMC sentence or malformed. */
bool nmea_parse_rmc(const char *line, nmea_rmc_t *out);

/** Parse "$xxGGA,..." satellites-used field. Returns false if not GGA. */
bool nmea_parse_gga_sats(const char *line, uint8_t *sats);

/** Days-from-civil for the proleptic Gregorian calendar (no libc timezone involvement). */
int64_t nmea_days_from_civil(int y, unsigned m, unsigned d);

#ifdef __cplusplus
}
#endif
