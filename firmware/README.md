# Firmware (ESP32-P4, ESP-IDF v6.1)

Current stage: **M0 bring-up**. See [PLAN-A.md](../PLAN-A.md).

Requires ESP32-P4 silicon **v3.x**: the EMAC PPS output isn't available on older revisions.

## Components

| Component | Role |
|---|---|
| `gm_clock` | EMAC PTP clock: PLL_F80M source, timestamps on all frames, PPS0 output, frequency adjust |
| `phase` | MCPWM dual capture at 160 MHz: GNSS PPS vs looped-back EMAC PPS |
| `gnss` | UART reader. Parses NMEA RMC (time, validity) and GGA (satellites). UBX/qErr comes in M1. |
| `ptp_net` | Frame classifier in front of lwIP. PTP over L2 and UDP/IPv4 (319/320) is consumed with its HW RX timestamp. UDP/IPv4 TX with HW TX timestamp, plus IGMP join of 224.0.1.129. |

`gnss/nmea.c` and `ptp_net/ptp_frame.c` have no ESP-IDF dependencies so they can be unit-tested on a host.

## M0 wiring (defaults, change in `idf.py menuconfig` → GNSS PTP grandmaster → Pins)

| Signal | GPIO |
|---|---|
| GNSS TXD → P4 RX | 21 |
| P4 TX → GNSS RXD | 20 |
| GNSS PPS → capture ch0 | 22 |
| EMAC PPS out | 23 |
| PPS loopback (wire GPIO23 → GPIO32) → capture ch1 | 32 |

Check these against the board's header silkscreen before wiring.

## Build

```sh
. ~/esp/esp-idf-v6.1/export.sh
idf.py set-target esp32p4
idf.py build flash monitor
```

## What M0 should print

- `gm_clock`: clock source and timestamp resolution
- `phase ... ns`: GNSS PPS vs EMAC PPS phase once per second, with the GNSS UTC time and satellite count
- `RX udp4 ...`: any PTP traffic on the LAN (e.g. a `ptp4l` master), with **hardware RX timestamps**
- `TX probe ...`: a domain-99 Signaling message once per second with its **hardware TX timestamp**

M0 is done when all four show sane, non-zero values.
