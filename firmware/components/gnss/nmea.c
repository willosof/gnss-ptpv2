#include "nmea.h"
#include <stdlib.h>
#include <string.h>

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool nmea_checksum_ok(const char *line)
{
    if (line[0] != '$') {
        return false;
    }
    uint8_t sum = 0;
    const char *p = line + 1;
    while (*p && *p != '*') {
        sum ^= (uint8_t)*p++;
    }
    if (*p != '*' || hexval(p[1]) < 0 || hexval(p[2]) < 0) {
        return false;
    }
    return sum == (uint8_t)(hexval(p[1]) << 4 | hexval(p[2]));
}

/* Copy field n (0 = sentence id) into buf. Returns false if missing. */
static bool field(const char *line, int n, char *buf, size_t len)
{
    const char *p = line;
    for (int i = 0; i < n; i++) {
        p = strchr(p, ',');
        if (!p) {
            return false;
        }
        p++;
    }
    size_t k = strcspn(p, ",*");
    if (k >= len) {
        return false;
    }
    memcpy(buf, p, k);
    buf[k] = '\0';
    return true;
}

static bool is_type(const char *line, const char *type)
{
    // "$GPRMC", "$GNRMC", ... : talker is 2 chars after '$'
    return strlen(line) > 6 && strncmp(line + 3, type, 3) == 0 && line[6] == ',';
}

int64_t nmea_days_from_civil(int y, unsigned m, unsigned d)
{
    // Howard Hinnant's algorithm
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned mp = m > 2 ? m - 3 : m + 9; // March-based month
    const unsigned doy = (153 * mp + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

bool nmea_parse_rmc(const char *line, nmea_rmc_t *out)
{
    char t[16], st[4], d[8];
    if (!is_type(line, "RMC") || !field(line, 1, t, sizeof(t)) || !field(line, 2, st, sizeof(st)) ||
            !field(line, 9, d, sizeof(d)) || strlen(t) < 6 || strlen(d) != 6) {
        return false;
    }
    int hh = (t[0] - '0') * 10 + (t[1] - '0');
    int mm = (t[2] - '0') * 10 + (t[3] - '0');
    int ss = (t[4] - '0') * 10 + (t[5] - '0');
    uint32_t ms = 0;
    if (t[6] == '.') {
        // Fractional seconds, any number of digits, scaled to milliseconds
        int scale = 100;
        for (const char *p = t + 7; *p && scale > 0; p++, scale /= 10) {
            ms += (uint32_t)(*p - '0') * scale;
        }
    }
    int day = (d[0] - '0') * 10 + (d[1] - '0');
    int mon = (d[2] - '0') * 10 + (d[3] - '0');
    int yr = 2000 + (d[4] - '0') * 10 + (d[5] - '0');
    if (mon < 1 || mon > 12 || day < 1 || day > 31 || hh > 23 || mm > 59 || ss > 60) {
        return false;
    }
    out->valid = st[0] == 'A';
    out->utc_sec = nmea_days_from_civil(yr, (unsigned)mon, (unsigned)day) * 86400 + hh * 3600 + mm * 60 + ss;
    out->utc_msec = ms;
    return true;
}

bool nmea_parse_gga_sats(const char *line, uint8_t *sats)
{
    char n[8];
    if (!is_type(line, "GGA") || !field(line, 7, n, sizeof(n))) {
        return false;
    }
    *sats = (uint8_t)atoi(n);
    return true;
}
