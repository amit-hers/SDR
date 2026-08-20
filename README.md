# SDR Datalink

A professional C++20 transparent wireless bridge built for the [ADALM-PlutoSDR](https://wiki.analog.com/university/tools/pluto) (Zynq-7010 + AD9363). Two PlutoSDR devices, one per PC, create a full-duplex RF link — any traffic (TCP, UDP, video, RTP, multicast) flows between the two PCs as if they were on the same LAN.

---

## Hardware

| Item | Detail |
|------|--------|
| SDR | **HamGeek Pluto+** (AD9363 in AD9361 mode, default IP `192.168.2.1`) |
| FPGA | Zynq-7020 XC7z020clg400-2 (Cortex-A9 + PL, 1 GB DDR3) |
| RF chip | AD9363A (operating in AD9361 mode via F5OEO firmware) |
| Frequency range | 70 MHz – 6 GHz |
| Max sample rate | 30.72 MSPS on this board — the CMOS digital interface caps `DATA_CLK` at 61.44 MHz, i.e. half the LVDS-spec 61.44 MSPS. Requests above this are **rejected by the kernel driver**, not clamped. |
| Usable sample rate | ~4–8 MSPS (`bw_mhz` 1–2) for reliable reception — USB 2.0, not the radio, is the real limit. See [Bandwidth vs. USB throughput](#-bandwidth-vs-usb-throughput--read-before-raising-bw_mhz). |
| Oscillator | 0.5 ppm VCTCXO, with PPS / 10 MHz external-reference input |
| Ethernet | Gigabit (PS-side) |

---

## Network Topology (Bridge Mode)

```
PC-A (192.168.1.10)                        PC-B (192.168.1.20)
  eth0 ──┐                                    eth0 ──┐
         br0  ◄── same subnet ──►                   br0
  sdr0 ──┘  TX:434 MHz ───RF──► RX:434 MHz ──┘
            RX:439 MHz ◄──RF─── TX:439 MHz

PlutoSDR at 192.168.2.1                  PlutoSDR at 192.168.2.1
  (PC-A's USB port)                        (PC-B's USB port)
```

Any traffic from PC-A to PC-B — TCP, UDP, RTP, RTSP, multicast — flows transparently over RF. No application changes needed.

---

## Quick Start

### 1. Install dependencies

```bash
./scripts/install-deps.sh
```

Installs: `cmake`, `ninja`, `libiio-dev`, `libliquid-dev`, `libssl-dev`, `python3-pip`, `flask`, `flask-sock`.

### 2. Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### 3. Deploy to both PCs

```bash
# Basic deploy (Side A at 192.168.1.10, Side B at 192.168.1.20)
./scripts/deploy.sh --side-a 192.168.1.10 --side-b 192.168.1.20

# With FEC and AES-256 encryption
./scripts/deploy.sh --side-a 192.168.1.10 --side-b 192.168.1.20 \
    --fec --key 000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f

# Install as systemd services (auto-start on boot)
./scripts/deploy.sh --side-a 192.168.1.10 --side-b 192.168.1.20 --service
```

### 4. Verify

```bash
ping 192.168.1.20           # round-trip over RF from PC-A
iperf3 -s                   # on PC-B
iperf3 -c 192.168.1.20      # on PC-A
```

Throughput is bounded by `bw_mhz` (see
[Bandwidth vs. USB throughput](#-bandwidth-vs-usb-throughput--read-before-raising-bw_mhz)),
not by the modulation table alone: at the recommended `bw_mhz: 1` the symbol
rate is 1 Msym/s, so BPSK gives ~1 Mbps raw before framing overhead and loss.

### 5. Monitor

```bash
# Start live stats feeder (reads real IQ from PlutoSDR, no sudo needed)
./build/live_stats 192.168.2.1 434.0 10 &

# Start web dashboard
python3 src/monitor/server.py --port 8080
# Open http://<PC-IP>:8080
```

---

## Operating Modes

| Mode | Description |
|------|-------------|
| `bridge` | **Default.** Transparent L2 bridge — TAP + Linux bridge joining TAP and LAN NIC |
| `mesh` | IP routing via TUN interface — subnet mesh |
| `p2p-tx` | Unidirectional transmitter over UDP |
| `p2p-rx` | Unidirectional receiver over UDP |
| `scan` | Channel scan → `/tmp/sdr_scan.json` |

---

## Configuration (`config.json`)

```json
{
  "mode":             "bridge",
  "pluto_ip":         "192.168.2.1",   // or a URI: "usb:1.56.5"

  "freq_tx_mhz":      434.0,
  "freq_rx_mhz":      439.0,
  "bw_mhz":           1,               // 1-2; higher starves RX (see below)
  "tx_atten_db":      10,
  "gain_mode":        "fast_attack",
  "modulation":       "AUTO",

  "tap_iface":        "sdr0",
  "bridge_iface":     "br0",
  "lan_iface":        "eth0",
  "tap_mtu":          1386,

  "encrypt":          false,
  "fec":              false,
  "aes_key_hex":      "0000...0000",

  "stats_interval_ms": 1000,
  "monitor_port":     8080,
  "node_id":          "0x00000001",

  "scan_start_mhz":   430.0,
  "scan_step_mhz":    1.0,
  "scan_n":           20
}
```

Live tuning (no restart): POST to `/api/ctrl` via the monitor, or send `SIGUSR2` to the daemon after writing `/tmp/sdr_reload.json`.

---

## Adaptive Modulation

When `modulation` is `AUTO`, the daemon automatically switches based on measured SNR with 2 dB up / 1 dB down hysteresis:

| SNR | Scheme | Spectral efficiency |
|-----|--------|-------------------|
| < 9 dB | BPSK | 1 bps/sym |
| 9–15 dB | QPSK | 2 bps/sym |
| 15–24 dB | 16QAM | 4 bps/sym |
| ≥ 24 dB | 64QAM | 6 bps/sym |

---

## ⚠ Bandwidth vs. USB throughput — read before raising `bw_mhz`

The AD9363 will happily accept a high sample rate, but the IQ stream still has
to cross USB. Sample rate is `bw_mhz × 4` (`RRC_SPS`), and each sample is
4 bytes, so `bw_mhz: 5` means 80 MB/s — far beyond USB 2.0's ~35 MB/s
practical ceiling. When the link can't keep up, the receiver simply stops
listening for part of the time, and **bursts that arrive during a gap are
never seen at all.**

Measured RX duty cycle (fraction of wall time actually receiving):

| `bw_mhz` | Sample rate | Stream rate | USB backend | Network backend |
|---------:|------------:|------------:|------------:|----------------:|
| **1** | 4 MSPS | 16 MB/s | **99%** | **99%** |
| **2** | 8 MSPS | 32 MB/s | **95%** | 69% |
| 3 | 12 MSPS | 48 MB/s | 63% | 46% |
| 5 | 20 MSPS | 80 MB/s | 38% | 28% |

Raising `bw_mhz` past ~2 therefore delivers **fewer** frames, not more. Real
measurement over a cabled link, BPSK, everything else identical:

| Setting | Frames TX | Frames decoded | Rate |
|---------|----------:|---------------:|-----:|
| `bw_mhz: 5` | 59 | 6 | 10% |
| `bw_mhz: 1` | 130 | 76 | **58%** |

Default is `bw_mhz: 1`. The daemon prints a warning if you set it above 2.
(Higher rates are still fine for TX-only or `scan` mode — the limit is on
sustained reception.)

---

### Transport: prefer the USB backend

`pluto_ip` accepts either a plain IP (`192.168.2.1`) or an explicit libiio
backend URI (`usb:1.56.5`). The network backend runs TCP over the board's
USB-ethernet gadget and sustains a lower rate than talking to the USB
device directly, so `PlutoSDR::connect()` transparently upgrades a plain IP
to the USB backend when it can identify the same board unambiguously by
serial. It logs which transport it ended up on:

```
[sdr] 192.168.2.1: using transport usb:1.56.5
```

Boards ship with a blank serial, which makes them impossible to tell apart —
they then stay on the slower network path (and collide on IP/MAC, see
"Known Hardware Details"). Give each board a unique serial once:

```bash
ssh root@192.168.2.1                       # password: root
fw_setenv UniqueID MY-PLUTO-01
reboot
```

Then `iio_info -s` (or the daemon's own log line) will show a distinct
`usb:` URI per board, which you can also use directly as `pluto_ip` to skip
IP addressing entirely.

Set `"modulation"` to a fixed scheme (`BPSK`/`QPSK`/`16QAM`/`64QAM`) instead of
`AUTO` to disable auto-switching and lock the link to that scheme (BridgeMode
only). Useful for diagnosing a link that reports good SNR but won't decode
anything — high-order QAM has a much narrower carrier-frequency-offset
tolerance than BPSK, so two radios running off independent free-running
TCXOs (no shared reference clock) may only be able to hold lock at a lower
scheme, if at all.

---

## Reed-Solomon FEC

Set `"fec": true` to enable RS(255,223) per-frame error correction via liquid-dsp (`LIQUID_FEC_RS_M8`):

- 14.3% overhead per frame
- Corrects up to 16 byte errors per 255-byte block
- Negligible CPU impact on the Cortex-A9

---

## ARQ / Reliable Delivery

Set `"arq": true` (BridgeMode only, both sides) to add selective-repeat
retransmission on top of FEC — anything FEC can't correct gets resent instead
of just dropped. It's opt-in and complements, rather than replaces, FEC:

- Every accepted data frame triggers a tiny control-frame ACK
  (`flags = FL_CTRL | FL_ACK`, `seq` = the acknowledged sequence number, zero
  payload) sent back over the same link — no new wire format needed, it
  reuses the `FL_ACK`/`FL_CTRL` flags already in the frame header.
- The sender tracks up to `arq_window` (default 16) outstanding frames. An
  unacknowledged frame is retransmitted after `arq_timeout_ms` (default 80 ms,
  backing off ×1.5 per retry up to 500 ms), and dropped after
  `arq_max_retries` (default 5) attempts.
- If the window fills up, new packets from the TAP interface are held (not
  dropped) until space frees — this creates natural backpressure rather than
  silently discarding traffic.
- `arq_acked` / `arq_retransmits` / `arq_dropped` counters are exposed in the
  stats JSON (`/api/stats`, `/ws`) alongside the existing FEC counters.

**Scope:** currently BridgeMode-only and point-to-point — ACKs aren't
addressed to a specific peer, which is fine for today's 2-node link but will
need a `dest_id` once real multi-node mesh routing exists. Because TCP
traffic already self-retransmits, ARQ's main benefit is for UDP/RTP traffic
sharing the same bridge (e.g. a command/telemetry channel) — for bulk
loss-tolerant video you may prefer FEC alone to avoid the extra latency of
waiting on retransmits.

```json
{
  "arq": true,
  "arq_window": 16,
  "arq_timeout_ms": 80,
  "arq_max_retries": 5
}
```

---

## Frame Format (v3)

```
[PREAMBLE 32B 0xAA] [SYNC 4B 0xC0FFEE77] [VER 1B=0x03] [FLAGS 1B] [MOD 1B] [BW 1B]
[NODE_ID 4B BE] [SEQ 4B BE] [LEN 2B BE]
[PAYLOAD N bytes  (FEC-encoded if FL_FEC; AES-encrypted if FL_ENCRYPT)]
[CRC32 4B LE] [POSTAMBLE 16B 0xAA]
Header+CRC overhead: 22 bytes   Preamble+postamble: 48 bytes   MAX_PAYLOAD: 1400 bytes
```

Flags: `FL_ENCRYPT=0x01  FL_FEC=0x02  FL_ACK=0x04  FL_CTRL=0x08`

The CRC covers the sync word through the payload — not the preamble or
postamble, which exist purely to give the receiver's control loops runway:

- **Preamble** — every frame is an isolated burst (there is no carrier between
  frames), so AGC / timing / carrier recovery all start cold. Their settling
  transient is longer than the 4-byte sync word, so without a preamble in
  front the sync word cannot survive.
- **Postamble** — the matched filter and decimator have group delay; without
  trailing padding the tail of the burst (including the CRC) gets clipped.

---

## RX pipeline (burst-gated)

A real transmission occupies a tiny fraction of an `rxPull` batch — the rest
is silence. Running AGC / `TimingSync` / `CostasLoop` continuously across the
whole batch lets them adapt to the silence rather than the signal, so
`BridgeMode::rxThread` instead:

1. **Detects bursts** (`BurstDetector`) by block energy vs. the median noise
   floor, and skips the DSP chain entirely for batches with nothing in them.
2. **Isolates each burst window**, applies blind coarse CFO correction
   (`CoarseFreqCorrect`, squaring method), and **resets** AGC / timing /
   carrier state so each burst starts clean.
3. **Retries demodulation at every sub-grid alignment** (`DECODE_OFFSETS`,
   = `RRC_TAPS`), taking the first that yields a frame.

Step 3 matters more than it looks. `symsync_crcf` cannot reliably acquire
symbol timing from an arbitrary start offset: whether it locks depends on
where the window begins relative to the RRC tap grid, and the dependence is
brittle rather than gradual — measured over identical signal content, a
precursor of 608 or 610 samples decoded 100% of the time while 600, 604, 612
and 616 decoded 0%. A detected burst starts at an essentially arbitrary
offset, so sweeping a full grid period is what makes reception reliable
(synthetic decode rate over realistic batches: ~3/30 → 30/30). Each attempt
uses a fresh `Deframer`, since a misaligned attempt emits garbage that would
otherwise poison its state machine.

---

## Project Structure

```
sdr-datalink/
├── .gitignore
├── CMakeLists.txt
├── cmake/
│   ├── FindLiquidDSP.cmake
│   └── FindLibIIO.cmake
├── include/sdr/
│   ├── hardware/PlutoSDR.hpp  FPGARegs.hpp
│   ├── modem/IModem.hpp  Modem.hpp  AdaptiveModem.hpp
│   ├── framing/Frame.hpp  Framer.hpp  Deframer.hpp  ArqWindow.hpp
│   ├── dsp/RRCFilter.hpp  AGC.hpp  TimingSync.hpp  CostasLoop.hpp  FFTSpectrum.hpp
│   ├── fec/ReedSolomon.hpp
│   ├── crypto/AESCipher.hpp
│   ├── transport/ITransport.hpp  TUNTAPDevice.hpp  UDPSocket.hpp  SPSCRing.hpp
│   └── stats/LinkStats.hpp
├── src/
│   ├── core/              → libsdr_core.a
│   ├── daemon/            → sdr-datalink executable
│   │   └── modes/         BridgeMode  MeshMode  P2PMode  ScanMode
│   ├── tools/
│   │   └── live_stats.cpp → live_stats  (hardware IQ reader, no sudo)
│   └── monitor/           Python Flask monitor server + Chart.js dashboard
├── fpga/
│   ├── hls/
│   │   ├── sync_detector/ → AXI4-Stream 0xC0FFEE77 frame correlator IP
│   │   ├── rssi_meter/    → AXI4-Stream I²+Q² power accumulator IP
│   │   ├── gain_block/    → AXI4-Lite digital gain + PA safety gate IP
│   │   └── qpsk_modem/    → AXI4-Stream QPSK/BPSK demod + mod IP
│   └── bd/
│       └── pluto_sdr_bd.tcl  Vivado block diagram (all IPs wired)
├── scripts/
│   ├── install-deps.sh
│   ├── deploy.sh
│   └── setup-service.sh
├── tests/                 Unit tests (framing, FEC, modem, crypto, DSP)
└── config.json
```

---

## Monitor Dashboard

Start the web server on either PC:

```bash
# 1. Feed live hardware data (reads real IQ from PlutoSDR, writes /tmp/sdr_stats.json)
./build/live_stats 192.168.2.1 434.0 10 &

# 2. Start web server
python3 src/monitor/server.py --port 8080 --config config.json
```

Open `http://<PC-IP>:8080`. Features:

- **Signal panel** — RSSI + SNR rolling line charts (60 s window), live from AD9363
- **Throughput panel** — TX/RX kbps live chart
- **Spectrum panel** — 256-bin FFT (DC-centred, updates ~5 Hz), real hardware IQ
- **Constellation panel** — IQ scatter showing current modulation
- **FEC panel** — corrected / uncorrectable counters + correction rate
- **Temperature** — on-chip AD9363 thermometer (°C), updates every 5 s
- **Channel scanner** — bar chart of scanned frequencies vs. power
- **Control panel** — freq TX/RX, BW, mod, atten, encrypt toggle, Start/Stop
- **Live-tune** — apply frequency/attenuation changes without restart

WebSocket endpoint `/ws` pushes stats JSON every 200 ms.

---

## Video Streaming

Once the bridge is running, use any streaming tool without modification:

```bash
# FFmpeg RTP sender (PC-A)
ffmpeg -re -i video.mp4 -f rtp rtp://192.168.1.20:5004

# VLC receiver (PC-B)
vlc rtp://@:5004

# iperf3 throughput test
iperf3 -s                        # PC-B
iperf3 -c 192.168.1.20 -t 30     # PC-A
```

---

## FPGA IP Cores (Zynq-7010 PL)

Four HLS IP cores offload work from the Cortex-A9 into the FPGA fabric:

### Data path

```
RX: AD9363 → [rssi_meter] → [gain_block] → [qpsk_demod] → [sync_detector] → AXI DMA → ARM
TX: ARM → AXI DMA → [gain_block] → [qpsk_mod] → AD9363
```

### IP summary

| IP | File | Function | Resources |
|----|------|----------|-----------|
| `rssi_meter` | `fpga/hls/rssi_meter/` | I²+Q² accumulator; exposes mean power + peak via AXI4-Lite | 2 DSP48, ~150 LUT |
| `gain_block` | `fpga/hls/gain_block/` | Q0.12 digital gain, PA safety gate (zeros TX if sample > threshold), clip/gate counters | 2 DSP48, ~120 LUT |
| `sync_detector` | `fpga/hls/sync_detector/` | Correlates `0xC0FFEE77`; suppresses inter-frame noise so DMA/CPU only fire on real frames | ~200 LUT |
| `qpsk_modem` | `fpga/hls/qpsk_modem/` | RRC + AGC + Costas + timing (RX); polyphase RRC + QPSK/BPSK symbols (TX); AXI4-Lite enable/lock | ~800 LUT, 2 DSP48 |

### AXI4-Lite register map

| Base address | IP | Key registers |
|---|---|---|
| `0x43C00000` | rssi_meter | `+0x18` mean power, `+0x28` peak, `+0x20` valid flag |
| `0x43C10000` | gain_block_rx | `+0x10` Q0.12 gain, `+0x18` enable, `+0x28` clip count |
| `0x43C20000` | gain_block_tx | `+0x10` Q0.12 gain, `+0x20` PA gate threshold, `+0x30` gate trips |
| `0x43C30000` | qpsk_demod | `+0x10` enable, `+0x18` lock count |
| `0x43C40000` | sync_detector | `+0x10` sync word, `+0x18` match count, `+0x20` drop count |

### Build FPGA (requires Vivado HLS 2019.1+ and Vivado 2019.1+)

```bash
# Synthesise each HLS IP (run on any Linux PC with Vivado installed)
cd fpga/hls/rssi_meter    && vivado_hls -f hls_build.tcl
cd fpga/hls/gain_block    && vivado_hls -f hls_build.tcl
cd fpga/hls/sync_detector && vivado_hls -f hls_build.tcl
cd fpga/hls/qpsk_modem    && vivado_hls -f hls_build.tcl

# Create block diagram in Vivado (after adding IPs to catalog)
vivado -source fpga/bd/pluto_sdr_bd.tcl

# Generate bitstream → flash to PlutoSDR
# scp boot.bin root@192.168.2.1:/mnt/jffs2/
# ssh root@192.168.2.1 reboot
```

### Access FPGA registers from the ARM side

```cpp
#include "sdr/hardware/FPGARegs.hpp"

sdr::FPGARegs regs;
regs.open();                          // mmap /dev/mem (requires root)

// Read hardware power measurement
float power_dbfs = regs.rssi_power_dbfs();

// Set PA safety gate — TX goes silent if any sample exceeds 28000 ADC counts
regs.set_pa_gate(28000);

// Digital gain (1.0 = unity, 0.5 = -6 dB)
regs.set_tx_gain(0.8f);
regs.set_rx_gain(1.0f);

// How many frames the correlator has detected
uint32_t frames = regs.sync_match_count();

// Noise suppression ratio
uint32_t noise_bytes = regs.sync_drop_count();
```

---

## Build & Test Reference

```bash
# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build (produces sdr-datalink, live_stats, sdr-tests)
cmake --build build -j$(nproc)

# Unit tests (no hardware required)
ctest --test-dir build --output-on-failure

# Live hardware stats (single PlutoSDR, no sudo)
./build/live_stats 192.168.2.1 434.0 10

# Deploy
./scripts/deploy.sh --side-a HOST_A --side-b HOST_B [--fec] [--key HEX] [--service]

# Monitor
python3 src/monitor/server.py --port 8080
```

---

## Dependencies

| Library | Purpose |
|---------|---------|
| `libiio` | IQ streaming to/from PlutoSDR over network context |
| `liquid-dsp` | Modems (BPSK/QPSK/16QAM/64QAM), RRC filter, AGC, timing sync, RS FEC |
| `openssl` | AES-256-CTR per-frame encryption |
| `flask` + `flask-sock` | Monitor web server + WebSocket |

---

## Known Hardware Details (PlutoSDR Rev.C)

- `altvoltage0` (RX_LO) and `altvoltage1` (TX_LO) are both **output** channels in the iio model
- Temperature is on channel `temp0`, attribute `input` (millidegrees → divide by 1000)
- Device names: `ad9361-phy`, `cf-ad9361-dds-core-lpc` (TX), `cf-ad9361-lpc` (RX)
- Firmware: F5OEO extended firmware (HamGeek ships with this pre-installed)
- FPGA part: `xc7z020clg400-2` (NOT xc7z010 — affects all HLS synthesis targets)
- Firmware tested: `v0.37-dirty` (F5OEO build) on `xc7z020clg400-2`
- **Sample rate ceiling is 30.72 MSPS, not 61.44.** The CMOS digital interface
  caps `DATA_CLK`; the driver logs
  `ad9361_validate_trx_clock_chain: Failed CMOS MODE DATA_CLK > 61.44MSPS`
  and **rejects** the write (`EINVAL`) rather than clamping it, so an
  unachievable request silently leaves the previous rate in place unless the
  return value is checked (`PlutoSDR::setSampleRate` now throws).
- **Two boards ship identical on the wire.** The host-visible MAC is derived
  from a hash of the u-boot `UniqueID` variable
  (see `/etc/init.d/S23udc`), which is *empty* out of the box — so every
  board hashes to the same MAC, and two of them on one host collide: only one
  is reachable, ARP behaves erratically under load, and libiio can't tell
  them apart. Fix once per board:
  ```bash
  ssh root@192.168.2.1     # password: root
  fw_setenv UniqueID MY-PLUTO-01
  reboot
  ```
  After this each board gets a distinct MAC, a distinct `enx…` interface, and
  a distinct `usb:` URI usable directly as `pluto_ip`.
- `hardwaregain` (TX attenuation) is a **fractional-dB** attribute (0.25 dB
  steps), despite some vendor docs describing it as millidB — verified by
  readback.
- The board's `rssi` attribute proved unreliable as a signal-presence
  indicator here: it barely moved between "peer transmitting at full power"
  and "peer off". Trust raw IQ power / decoded frames instead.
