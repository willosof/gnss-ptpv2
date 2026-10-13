# gnss-ptpv2 — a low-cost GNSS-disciplined PTPv2 grandmaster

Goal: a PTPv2 (IEEE 1588-2008) grandmaster that gets its time from GNSS and is good
enough for **AES67** and **SMPTE ST 2110 / ST 2059-2**, built from commodity parts
for roughly 1/20th of the cost of a commercial unit.

> Status: research and architecture (2026-10-13). Prices are approximate snapshots;
> anything marked **[verify]** has not been confirmed against a datasheet or on the bench yet.

---

## 1. What "good enough" means

| Requirement | Source | Our budget |
|---|---|---|
| Slaves within ~1 µs of the grandmaster | ST 2059-2 / AES67 practice | GM error should stay **< 100 ns** so the network has plenty of margin |
| Hardware timestamping of PTP event messages | Required for sub-µs accuracy | MAC or PHY timestamping. SPI Ethernet chips such as the W5500 cannot do this. |
| AES67 media profile | AES67 Annex A | Domain 0, E2E, sync 2^-3 (8/s), announce 2^1, UDP/IPv4 multicast |
| SMPTE ST 2059-2 profile | ST 2059-2 | Domain 127, sync 2^-3, announce 2^-2, announceReceiptTimeout 3, plus the SMPTE synchronization-metadata TLV |
| Correct dataset | 1588 | `timeSource=GPS (0x20)`, `clockClass 6` when locked, `7` in holdover, `248` free-running, `currentUtcOffset=37` with the valid flag, leap flags from GNSS |

Where the error comes from (target total < 100 ns):

| Contributor | Typical |
|---|---|
| GNSS PPS (timing receiver, survey-in, after qErr sawtooth correction) | 2–20 ns |
| Capturing the PPS edge (timer at 160 MHz) | ~6 ns quantization |
| PTP timestamp clock resolution (80 MHz PTP clock on the ESP32-P4) | 12.5 ns steps |
| 100BASE-TX PHY latency variation, RMII clock-domain crossing | 10–40 ns |
| Link asymmetry (static, can be calibrated out) | constant offset |

The expensive part of a commercial grandmaster is **holdover**: keeping time when GNSS is lost,
done with an OCXO or rubidium oscillator. Inside one facility the slaves follow the GM even if it
drifts from UTC, so holdover mainly matters for multi-site or GM failover. A good TCXO is enough to
start with, and the board leaves room for an oscillator upgrade.

---

## 2. Platform choice

### Option A — ESP32-P4 (recommended)

The ESP32-P4 is the first ESP32 whose Ethernet MAC has **IEEE 1588 hardware timestamping**. Classic
ESP32, S3 and C6 parts don't, so they're out.

- ESP-IDF exposes the PTP clock (`esp_eth_clock_gettime` / `esp_eth_clock_settime`). A PTP daemon
  (port of NuttX `ptpd`) exists as the `esp_ptp` component and can act as master (BTCA/"BTC").
- An **AES67/RAVENNA endpoint** component (`datanoisetv/aes67`) already exists for the P4, and Espressif
  published a blog post on it in June 2026. It's tested against Merging SIENNA and aes67-linux-daemon.
- ESP-IDF source checked:
  - The PTP clock source is `XTAL` (40 MHz) or `PLL_F80M`, so the PTP timebase is the board's 40 MHz
    crystal. Replacing it with a TCXO or DCTCXO improves the whole chip.
  - There is a **PPS output** (`EMAC_PTP_PPS_PAD_OUT_IDX`, routable through the GPIO matrix).
  - There's no auxiliary timestamp trigger *input* exposed. We work around that in hardware (below).
- Board: **Waveshare ESP32-P4-ETH**, about $20 (100 Mbit RJ45, PoE variant about $28).
- Limitations: one EMAC, **100 Mbit only** (fine for a GM: PTP traffic is tiny; connect to a switch),
  and the software is young. It has no published accuracy figures, so we'll measure them ourselves.

**Disciplining without a PPS-capture input.** The EMAC can output a PPS from the PTP clock.
1. Wire the EMAC PPS output pin back into an MCPWM capture channel.
2. Wire the GNSS PPS into a second capture channel on the **same** capture timer (160 MHz, so
   about 6 ns resolution **[verify clock]**).
3. Phase error = t(GNSS PPS) − t(EMAC PPS), measured entirely in hardware. Interrupt latency
   doesn't matter.
4. Subtract the GNSS-reported qErr (u-blox `UBX-TIM-TP`, or the equivalent message on other vendors).
5. A PI servo trims the PTP clock's addend (frequency) and steps the offset when unlocked.

Because we discipline the *actual pin edge*, the GPIO-matrix delay is inside the loop. It's a fixed
offset that can be calibrated once with a scope.

### Option B — Linux and an Intel i226/i210 NIC (lab reference only, not the product)

A grandmaster's job is a few hundred small packets per second, a once-per-second phase
measurement, and a control loop. That is microcontroller work. A multi-GB Linux SBC is overkill
and brings boot time, an SD card, and OS updates with it. We use this only as a known-good
measuring instrument.


- Raspberry Pi 5 plus the **TimeHAT** (Intel i226 with SDP PPS in/out on SMA, TCXO, optional M.2 GNSS),
  about $200 for the HAT. Software: `linuxptp` (`ts2phc` takes the GNSS PPS into the PHC, `ptp4l`
  serves PTP). Jeff Geerling's `time-pi` project documents a full build. Notes from that build: the
  i226 has DHCP issues and he couldn't get 2.5 Gbit working.
- Raspberry Pi CM4: its BCM54210PE PHY has PTP and a SYNC_IN pin usable as PPS input
  **[verify with the CM4 IO board]**. Whether the CM5 has an equivalent pin is unverified.
- Pros: mature software, gigabit, easy profile configs. Cons: costs 3–5× more, boots like Linux,
  and has a lot of moving parts.
- **Use: build one as the measurement reference** (a hardware-timestamping slave that logs offset)
  even if Option A becomes the product.

### Option C — Teensy 4.1 (NXP i.MX RT1062)

- About $32, with a 10/100 PHY on board. The ENET block has an IEEE 1588 timer **with hardware
  input-capture and output-compare channels**, so real hardware PPS timestamping is possible.
  QNEthernet exposes IEEE 1588 support **[verify channel/pin mapping]**.
- No ready-made PTP grandmaster stack. You'd write or port one (ptpd, or Zephyr's PTP stack on RT1060).
- A good fallback if the P4 route hits a wall.

### PHY-based alternative (any MCU with RMII)

- **TI DP83640**: 100 Mbit PHY with 1588 timestamping inside the PHY and 12 GPIOs for event
  capture/trigger. About €12, still active. It would let even a non-PTP MCU become a grandmaster with
  true hardware PPS capture, but the PTP stack must use PHY timestamps (status frames over MDIO),
  which `esp_ptp` doesn't do today.
- Microchip LAN8841/LAN8814 (gigabit, 1588 PHY, Linux driver supports PEROUT and EXTTS on LAN8814).

---

## 3. GNSS receiver (needs a timing receiver, not just "a GPS")

| Part | PPS spec | Bands | Approx. price | Notes |
|---|---|---|---|---|
| u-blox **NEO/LEA-M8T** | < 20 ns | L1 | ~$70–120 module | Classic timing part: survey-in, fixed-position mode, qErr. Lots of cheap boards, but counterfeits exist. |
| **Quectel LG290P** | **5 ns RMS** | quad-band | bare module much cheaper than the breakout; SparkFun breakout $190 | Best value. Needs checking for a qErr-equivalent message and timing mode **[verify]**. |
| u-blox **NEO-F10T** | "ns-level" | L1/L5 | $160 single (DigiKey) | Timing-specific, but pricey for what it is. |
| u-blox **ZED-F9T** | ~5 ns | L1/L2(/L5) | $200+ | Industry-standard timing reference. |

Antenna: multi-band (L1/L5 or L1/L2) active antenna with a good ground plane, about $30–70. For roof
installs, add a lightning arrester and use LMR-240/400 cable. Configure the receiver in **timing mode**
(survey-in, then fixed position), which noticeably lowers PPS jitter.

**Pick:** LG290P for the build. NEO-M8T is the budget fallback. Keep the footprint and UART protocol
abstracted so either fits.

---

## 4. Oscillator (holdover and wander)

| Tier | Part | Holdover feel | Cost |
|---|---|---|---|
| 0 | Stock 40 MHz crystal on the dev board | Poor; fine for proving software | $0 |
| 1 | 40 MHz TCXO ±0.5 ppm (clipped sine, 3225 package) | µs within minutes | ~$2 |
| 2 | **SiTime SiT5356** Super-TCXO (any frequency 1–60 MHz, so 40 MHz works; Stratum 3; I²C DCTCXO with 5 ppt pull resolution) | µs within roughly an hour | ~$30–60 **[verify price]** |
| 3 | OCXO (new, or surplus 10 MHz units such as the CTI OSC5A2B02) plus a PLL to 40 MHz | µs over several hours | ~$15–80 |

Because the servo trims the PTP addend digitally, oscillator **stability** matters more than
pullability. The SiT5356's I²C pull is a bonus: it lets you discipline the oscillator itself, which
also locks a 10 MHz output and the audio APLL to GNSS.

---

## 5. Architecture (Option A)

```
   GNSS antenna ── bias-T ── LG290P ──UART──┐
                               │ PPS        │
                               ▼            ▼
                     ┌──── ESP32-P4 ─────────────────────────┐
                     │ MCPWM capture ch0 ◄─ GNSS PPS         │
                     │ MCPWM capture ch1 ◄─┐                 │
                     │ EMAC PTP PPS out ───┴─► SMA "PPS OUT" │
                     │ EMAC (1588 HW ts) ── RMII ── PHY ── RJ45/PoE ──► PTP network
                     │ XTAL_P ◄── 40 MHz TCXO / SiT5356 (I²C)│
                     │ I²S/APLL ── 48 kHz word clock out (optional) │
                     └───────────────────────────────────────┘
```

Firmware tasks:
1. **gnss**: UBX/NMEA parser, time-of-next-pulse, qErr, leap seconds, fix and survey state.
2. **phase**: MCPWM capture ISR → phase error per second.
3. **servo**: state machine FREERUN → LOCKING (step) → LOCKED (PI on addend) → HOLDOVER (last frequency,
   plus a temperature model later).
4. **ptp**: `esp_ptp` in master-only mode, plus profile presets (AES67, ST 2059-2) and a dynamic
   `clockClass`/`clockAccuracy` driven by servo state, and the ST 2059-2 synchronization-metadata TLV.
5. **ui/telemetry**: web page, Prometheus `/metrics` (offset, frequency, satellites, state), and syslog.
6. **extras**: 48 kHz word clock or AES11 aligned to the PTP epoch (APLL), and 10 MHz output from the oscillator.

---

## 6. Plan

**Phase 0: bench kit (order now, about $100–250)**
- [ ] 2 × Waveshare ESP32-P4-ETH: one GM, one slave and test node
- [ ] GNSS: SparkFun LG290P breakout, or a NEO-M8T board for the budget version
- [ ] Multi-band active GNSS antenna and SMA/U.FL pigtails
- [ ] 40 MHz TCXO (a few, to bodge onto the dev board), and later a SiT5356 sample
- [ ] Reference slave: any PC or mini PC with an **Intel i210/i226** NIC running `ptp4l -s`, which logs offset
- [ ] Managed switch, ideally one with PTP transparent or boundary clock (for real-world tests)
- [ ] A scope (≥100 MHz, 2 channels) to compare GNSS PPS against P4 PPS out against the Linux slave's PPS out

**Phase 1: firmware**
- [ ] Get `esp_ptp` running as master and a Linux slave locking with hardware timestamps
- [ ] Wire the PPS loopback and MCPWM capture, and log the raw phase error
- [ ] Servo and GNSS lock; aim for PPS-out vs GNSS-PPS < 50 ns RMS
- [ ] Profile presets, clockClass state machine, ST 2059-2 TLV
- [ ] Interop: Dante in AES67 mode, a Merging/RAVENNA device, an ST 2110 receiver, and `ptp4l` as BC

**Phase 2: PCB**
- [ ] ESP32-P4 (chip or module) and RMII PHY, either as on the reference designs or the DP83640 variant
- [ ] Oscillator footprint taking either a 3225 TCXO or the SiT5356
- [ ] GNSS module footprint, SMA antenna with a current-limited bias-T (3.3/5 V selectable)
- [ ] SMA outputs: PPS, 10 MHz, word clock (BNC optional)
- [ ] PoE PD (802.3af module) plus USB-C power, a small OLED or status LEDs, and an enclosure

---

## 7. Open questions / risks

Detailed plan for Option A: [PLAN-A.md](PLAN-A.md).

- Existing P4 PTP stacks (`esp_ptp`, `aes67-esp32p4`) are **Layer-2 only**. AES67 and ST 2059-2 need
  UDP/IPv4, so we write our own UDP transport and a master-focused engine (see PLAN-A §0).
- The exact MCPWM capture clock and whether the GPIO-matrix input synchronizer adds a cycle of jitter.
- RMII clock source: have the P4 generate the 50 MHz REF_CLK from the disciplined oscillator, so
  everything runs on one timebase.
- ESP32-P4 silicon revision differences (check the board you get).
- LG290P timing features (timing mode, qErr-equivalent) **[verify]**.

## Sources

- Espressif blog: [AES67 audio over IP on the ESP32-P4](https://developer.espressif.com/blog/2026/06/aes67-audio-over-ip-on-the-esp32-p4/)
- [`esp_ptp` component](https://components.espressif.com/components/scrambletools/esp_ptp/versions/1.3.0/readme)
- [`datanoisetv/aes67` component](https://components.espressif.com/components/datanoisetv/aes67/versions/2.6.0/readme)
- ESP-IDF: `components/esp_hal_emac/esp32p4/include/hal/emac_ll.h`, `components/soc/esp32p4/include/soc/gpio_sig_map.h`
- [SparkFun LG290P breakout](https://www.sparkfun.com/sparkfun-quadband-gnss-rtk-breakout-lg290p-qwiic.html)
- [u-blox NEO-F10T](https://www.u-blox.com/en/product/neo-f10t-module), [u-blox NEO/LEA-M8T](https://www.u-blox.com/en/product/neolea-m8t-series)
- [SiTime SiT5356](https://www.sitime.com/products/super-tcxo/sit5356)
- [TI DP83640](https://ti.com/product/DP83640/part-details/DP83640TVV/NOPB)
- [TimeHAT (Tindie)](https://tindie.com/products/timeappliances/timehat-2), [Jeff Geerling: DIY PTP grandmaster](https://www.jeffgeerling.com/blog/2025/diy-ptp-grandmaster-clock-raspberry-pi), [time-pi](https://github.com/geerlingguy/time-pi)
- [PJRC Teensy 4.1](https://www.pjrc.com/teensy-4-1-released/)
