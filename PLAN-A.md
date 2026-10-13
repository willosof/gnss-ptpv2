# Plan A — ESP32-P4 GNSS-disciplined PTPv2 grandmaster

Status: plan, 2026-10-13. Nothing is built yet. **[verify]** marks things to confirm on the bench
or in a datasheet before we rely on them.

## 0. Findings that shape the plan

Checked against ESP-IDF `master`, `scrambletools/esp_ptp` 1.6.0 and `DatanoiseTV/aes67-esp32p4`:

| Finding | Consequence |
|---|---|
| The P4 EMAC has a 1588 clock with a fine-correction addend, add/sub time steps, a target-time interrupt and a PPS0 output. The public API includes `esp_eth_mac_adj_ptp_freq_ppb()`, `esp_eth_mac_set_ptp_time()`, PPS0 on a GPIO, and `esp_eth_mac_get_ptp_resolution()`. | Everything needed to discipline the clock is exposed. No register hacking required. |
| PTP clock source: `XTAL` (40 MHz) or `PLL_F80M`, both derived from the 40 MHz crystal. | The oscillator upgrade means replacing the 40 MHz crystal. |
| **No auxiliary-snapshot (event capture) input** is exposed for the EMAC. | GNSS PPS capture is done with the **MCPWM capture timer** (PLL_F160M = 6.25 ns resolution, confirmed in `clk_tree_defs.h`). EMAC PPS out and GNSS PPS feed two channels of the same timer. |
| **Both existing P4 PTP stacks (`esp_ptp`, the ptpd vendored in `aes67-esp32p4`) are Layer-2 only** (ethertype 0x88F7 through L2TAP). | **AES67 and ST 2059-2 require UDP/IPv4** (224.0.1.129, ports 319/320). We have to add a UDP transport. |
| `esp_eth_update_input_path_info()` delivers each RX frame **with its info (timestamp)**. `esp_eth_transmit_ctrl_bufs()` sends a frame with a control block (TX timestamp request). | The UDP transport can be a thin frame classifier in front of lwIP, with full hardware timestamps. |
| `esp_ptp` has server support, but clockClass, accuracy and timeSource are **static Kconfig values**. It's a 5k-line single file built around gPTP, AVB and Wi-Fi. | **Decision: write a small master-focused engine** (`ptp_gm`), borrowing codecs and ideas from `esp_ptp` (Apache-2.0) where useful. |

## 1. Target spec (v1)

- PTPv2 (IEEE 1588-2008 wire format), **E2E, two-step, UDP/IPv4 multicast**, with unicast Delay_Resp
  when the request was unicast. L2 transport is optional.
- Profiles: **AES67 media** (domain 0, Sync 2^-3, Announce 2^1), **SMPTE ST 2059-2** (domain 127,
  Sync 2^-3, Announce 2^-2, receipt timeout 3, SM TLV), and IEEE default. All parameters can be
  overridden.
- Dataset: `timeSource=0x20 GPS`, `clockClass` 6 / 7 / 52 / 248 driven by servo state,
  `clockAccuracy` from measured error, `currentUtcOffset` and leap flags from GNSS, PTP timescale (TAI).
- BMCA: if a better master shows up on the domain, go PASSIVE. Configurable "master-only" override.
- Accuracy target: **GNSS PPS vs. P4 PPS-out < 50 ns RMS** when locked; a Linux i210/i226 slave on a
  direct cable < 100 ns peak-to-peak relative to GNSS.
- Holdover (TCXO): clockClass 7 while estimated error < 1 µs, then 52.
- Ops: web status page, JSON API, Prometheus `/metrics`, config in NVS, OTA, syslog.
- Outputs: PPS on SMA. 10 MHz and 48 kHz word clock are stretch goals.

## 2. Phase 0 — bench kit

| Qty | Item | Why | ~Price |
|---|---|---|---|
| 2 | Waveshare **ESP32-P4-ETH** (one PoE variant), **chip revision v3.x** | DUT plus a spare/second node. The EMAC PPS output only exists on P4 v3+ silicon, so confirm with the seller | $20–28 each |
| 1 | **SparkFun LG290P breakout** (GPS-26620) | Timing receiver, 5 ns RMS PPS | $190 |
| 1 | *(fallback)* u-blox NEO/LEA-M8T board | Well-documented qErr (`UBX-TIM-TP`) if the LG290P timing features fall short | $70–120 |
| 1 | Multi-band active GNSS antenna and cable | — | $40–80 |
| 5 | 40 MHz TCXO, 3225, clipped sine, ±0.5 ppm | Oscillator bodge on the dev board **[verify P4 accepts external clock on XTAL_P]** | $2 each |
| 1 | PC or mini-PC with **Intel i210 or i226 NIC** (the i210 needs SDP pins accessible for PPS out) | Reference slave: `ptp4l`/`phc2sys` logs, PHC PPS to the scope | $30 NIC |
| 1 | Managed switch with IGMP snooping (PTP TC/BC optional, e.g. an AV-line switch) | Real-world topology tests | have/$ |
| — | Scope ≥ 100 MHz, 2–4 channels; SMA tees, 50 Ω terminators | Measuring PPS alignment | have |

**Before ordering:** read the Waveshare schematic to pick free GPIOs, find the Ethernet PHY and see
which way REF_CLK goes, and check the crystal footprint.

### Bench wiring (GPIO numbers to be chosen from the schematic)

```
LG290P TXD ─────────► GPIO_GNSS_RX (UART1)
LG290P RXD ◄───────── GPIO_GNSS_TX
LG290P PPS ─────────► GPIO_CAP0   (MCPWM capture ch0)
P4 EMAC PPS0 ───────► GPIO_PPS_OUT ──┬──► scope / SMA
                                     └──► GPIO_CAP1 (MCPWM capture ch1)
                     (or the same pad with input enabled via GPIO matrix [verify])
GNSS PPS ─► scope ch1, P4 PPS ─► scope ch2, i210 SDP PPS ─► scope ch3
```

## 3. Firmware architecture

ESP-IDF 5.5+ (whichever release has the EMAC PTP API above; pin it in `idf_component.yml`).

```
firmware/
  main/                     app wiring, config load, task start-up
  components/
    gm_clock/               wrapper on the EMAC PTP clock: get/set/step/adj_ppb, PPS0 config
    gnss/                   receiver interface + backends: ubx (M8T/F9T), quectel (LG290P)
    phase/                  MCPWM dual-capture → per-second phase error, edge pairing, outlier reject
    servo/                  state machine + PI loop, holdover estimator   (host-testable)
    ptp_net/                frame classifier on esp_eth input path; Eth/IPv4/UDP framing; TX ts
    ptp_gm/                 PTP engine: Announce/Sync/Follow_Up/Delay_Resp, BMCA, profiles, TLVs
                            (codec + BMCA host-testable)
    ops/                    HTTP UI, JSON API, /metrics, NVS config, OTA, syslog, mDNS
  test/host/                Unity tests for servo, codec, BMCA, GNSS parsers (IDF linux target)
tools/                      log/scope analysis scripts (Python), fake-slave load generator (scapy)
```

### 3.1 `ptp_net`: UDP transport with hardware timestamps
- Register `esp_eth_update_input_path_info()` with a classifier that does two things. Frames that are
  IPv4/UDP to port 319/320 (to 224.0.1.129 or our unicast IP), or ethertype 0x88F7, go to the PTP
  queue together with the RX timestamp. Everything else goes to `esp_netif_receive()` for lwIP.
- TX: build Ethernet + IPv4 + UDP headers ourselves (IPv4 UDP checksum may be 0, IP checksum
  computed), MAC `01:00:5e:00:01:81`, TTL 1 (configurable), DSCP 46 (EF, as AES67 recommends), and
  send with `esp_eth_transmit_ctrl_bufs()` requesting a TX timestamp for Sync.
- Ask lwIP to join 224.0.1.129 via IGMP so snooping switches forward Delay_Req to us.
- ARP for unicast replies: read lwIP's ARP table or learn the sender MAC from the request frame
  (simpler and always correct).

### 3.2 `phase` and `servo`
- Every second: `e = t_cap(GNSS PPS) − t_cap(EMAC PPS) − qErr_gnss + cal_offset` (ns).
- The second number comes from GNSS time-of-next-pulse. The PTP clock runs TAI
  (UTC + leap seconds, currently 37 s).
- States:
  1. **FREERUN**: no valid GNSS. clockClass 248.
  2. **ACQUIRE**: GNSS valid. Set the PTP seconds field, then step the sub-second phase to |e| < 1 µs. Only state that steps.
  3. **LOCKING**: PI with τ ≈ 10 s, until |e| < 100 ns for 60 s.
  4. **LOCKED**: PI with τ ≈ 60 s for a TCXO, 300–1000 s for an OCXO/Super-TCXO. Slew only, never step. clockClass 6.
  5. **HOLDOVER**: GNSS lost or invalid. Freeze at the averaged frequency (later: temperature-compensated). Error estimate grows with an oscillator model. clockClass 7, then 52 once past 1 µs.
- Frequency resolution: addend is 32-bit, so about 0.23 ppb steps. Timestamp resolution: about
  12.5–25 ns depending on clock and update mode. Report the real value via
  `esp_eth_mac_get_ptp_resolution()`.
- Calibration: `cal_offset` covers GPIO/pad/cable delays and is measured once with the scope.

### 3.3 `ptp_gm` engine
- Port states: INITIALIZING → LISTENING → MASTER or PASSIVE.
- Sync and Follow_Up: the TX timestamp of Sync goes into Follow_Up.
- Delay_Req → Delay_Resp: same transport mode as the request (multicast or unicast).
- Announce carries the dataset from servo state. SMPTE synchronization-metadata TLV in ST 2059-2 mode.
- Management messages: respond to basic GET queries (e.g. `pmc` from linuxptp) for debuggability.
- Capacity target: 250 slaves at Delay_Req 8/s, which is about 2000 req/s. Measure CPU with the
  load generator.

## 4. Milestones

| # | Milestone | Done when |
|---|---|---|
| M0 | **Bring-up** (wk 1–2) | EMAC PTP running; PPS0 on a pin (pulse width measured **[verify narrow-pulse width]**); GNSS parsed; both PPS edges captured, raw `e` logged at 1 Hz |
| M1 | **Discipline** | Servo locks; PPS-out vs GNSS-PPS < 50 ns RMS for 1 h; clean recovery after antenna unplug/replug |
| M2 | **UDP PTP master** | `ptp4l -s` on an i210/i226 locks with HW timestamps over a direct cable; slave PHC PPS vs GNSS PPS < 100 ns p-p |
| M3 | **Profiles & datasets** | AES67 and ST 2059-2 presets, dynamic clockClass, SM TLV, BMCA PASSIVE tested against a second GM (`ptp4l` as master) |
| M4 | **Ops** | Web UI, `/metrics`, NVS config, OTA, syslog |
| M5 | **Interop & soak** | Dante (AES67 mode), RAVENNA/Merging, aes67-linux-daemon, one ST 2110 device; through a PTP-aware switch (BC/TC) and a dumb switch; 72 h soak with no steps or loss of lock |
| M6 | **Oscillator** (parallel from M1) | 40 MHz TCXO bodge, then SiT5356 eval; measure holdover drift over 1 h and 8 h |

Then Phase 2, PCB rev A. Inputs from M0–M6: GPIO map, PHY choice, oscillator choice, PPS output
stage (one-shot stretcher plus 50 Ω driver if PPS0 is too narrow), and PoE PD.

## 5. Test methods

- **PPS alignment:** scope with infinite persistence or a histogram of GNSS PPS vs P4 PPS-out; long
  term, log `e` from firmware and compare against the scope.
- **End-to-end:** the i210 slave's PHC PPS (SDP pin via `ts2phc`/`testptp -p`) on the scope against GNSS PPS.
  `ptp4l` `master offset` and `path delay` logs go to `tools/` scripts that produce MTIE/TDEV plots.
- **Load:** a scapy generator sending Delay_Req from N fake clock identities; check every response
  (sequenceId, requestingPortIdentity, timestamp sanity) and measure P4 CPU load.
- **Faults:** antenna unplug, link flap, a better master appearing, GNSS leap-second announcement
  (simulated via the receiver or a replayed log), power cycle (time to clockClass 6).

## 6. Risks and mitigations

| Risk | Mitigation |
|---|---|
| RX timestamp path through `input_path_info` is not what we assume | M0 spike: print RX timestamps of incoming 0x88F7 and UDP/319 frames before writing the engine |
| Narrow PPS0 pulse is not captured reliably | Use square-wave PPS mode or the target-time interrupt; on the PCB add a 74LVC1G123 stretcher |
| GPIO-matrix input synchronizer adds jitter | Average it out in the loop; measure in M1 |
| LG290P lacks a qErr equivalent | Accept ~5–10 ns extra jitter (the loop filters it) or fall back to M8T/F9T |
| 100 Mbit PHY latency variation | Characterize in M2; the 2nd-gen PCB could consider the DP83640 route |
| P4 silicon revision: **EMAC PPS out needs v3.x** (IDF v6.1 Kconfig) | Buy v3 boards; on v1.x, fall back to the target-time interrupt toggling a GPIO (more jitter) |

## 7. Open decisions

1. ~~GNSS for the dev kit~~ **Decided:** buy both the LG290P and an M8T board (2026-10-13).
2. Licence for our code: Apache-2.0 (compatible with the code we borrow) is the natural choice.
3. ~~Version control~~ **Done:** git repo initialised in `gnss-ptpv2/` (2026-10-13).
