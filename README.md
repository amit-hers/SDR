# SDR Datalink

A professional C++20 transparent wireless bridge built for the [ADALM-PlutoSDR](https://wiki.analog.com/university/tools/pluto) (Zynq-7010 + AD9363). Two PlutoSDR devices, one per PC, create a full-duplex RF link — any traffic (TCP, UDP, video, RTP, multicast) flows between the two PCs as if they were on the same LAN.

---

## Hardware

| Item | Detail |
|------|--------|
| SDR | ADALM-PlutoSDR Rev.C (default IP `192.168.2.1`) |
| FPGA | Zynq-7010 (Cortex-A9 + PL) |
| RF chip | AD9363A |
| Frequency range | 325 MHz – 3.8 GHz |
| Max sample rate | 20 MSPS |

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
iperf3 -c 192.168.1.20      # on PC-A → ~25–90 Mbps depending on SNR
```

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
  "pluto_ip":         "192.168.2.1",

  "freq_tx_mhz":      434.0,
  "freq_rx_mhz":      439.0,
  "bw_mhz":           10,
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

**Bandwidth budget at 10 MHz:**

| Scheme | Usable throughput |
|--------|------------------|
| QPSK | ~25 Mbps |
| 16QAM | ~50 Mbps |
| 64QAM | ~75 Mbps |

1080p H.264 requires ~8–15 Mbps — comfortable within QPSK. Use `bw_mhz: 20` to double all rates.

---

## Reed-Solomon FEC

Set `"fec": true` to enable RS(255,223) per-frame error correction via liquid-dsp (`LIQUID_FEC_RS_M8`):

- 14.3% overhead per frame
- Corrects up to 16 byte errors per 255-byte block
- Negligible CPU impact on the Cortex-A9

---

## Frame Format (v3)

```
[SYNC 4B 0xC0FFEE77] [VER 1B=0x03] [FLAGS 1B] [MOD 1B] [BW 1B]
[NODE_ID 4B BE] [SEQ 4B BE] [LEN 2B BE]
[PAYLOAD N bytes  (FEC-encoded if FL_FEC; AES-encrypted if FL_ENCRYPT)]
[CRC32 4B LE]
Total overhead: 22 bytes   MAX_PAYLOAD: 1400 bytes
```

Flags: `FL_ENCRYPT=0x01  FL_FEC=0x02  FL_ACK=0x04  FL_CTRL=0x08`

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
│   ├── framing/Frame.hpp  Framer.hpp  Deframer.hpp
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
- Firmware tested: `v0.37-dirty` on `xc7z010clg225-1`
