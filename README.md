# SDR Data Link

A software-defined radio toolkit built around the [ADALM-PlutoSDR](https://wiki.analog.com/university/tools/pluto) (Zynq-7010 + AD9363). Provides an FM receiver, a bidirectional encrypted digital data link, a SOL8-compatible mesh daemon, an FPGA DSP core, and a browser-based management UI.

---

## Hardware

| Item | Detail |
|------|--------|
| SDR | ADALM-PlutoSDR (default IP `192.168.2.1`) |
| FPGA | Zynq-7010 (Cortex-A9 + PL) |
| RF chip | AD9363 |
| Frequency range | 325 MHz – 3.8 GHz |
| Max sample rate | 20 MSPS (USB ceiling) |

---

## Dependencies

```bash
make link-install
# Installs: libliquid-dev libssl-dev python3-flask
#           gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf
```

| Library | Purpose |
|---------|---------|
| `libiio` | IQ streaming to/from PlutoSDR |
| `liquid-dsp` | Modulation, RRC filter, frame sync |
| `openssl` | AES-256-CTR frame encryption |
| `Flask` | Web management UI |

---

## Project Structure

```
.
├── fm_radio.c          FM receiver + ASCII spectrum display
├── fm_scan.c           FM band scanner
├── rx_example.c        Basic IQ capture example
├── link_tx.cpp         Data link transmitter (multi-threaded)
├── link_rx.cpp         Data link receiver  (multi-threaded)
├── link_test.cpp       Software DSP loopback (no hardware)
├── sol8.cpp            SOL8-compatible mesh/bridge daemon
├── sol8_test.cpp       Unit tests for sol8 (no hardware)
├── pluto_eth_bridge.c  Ethernet bridge helper
├── config.json         Persistent link / radio configuration
├── Makefile
├── fpga/
│   ├── qpsk_demod_hls.cpp   Vivado HLS QPSK IP core
│   └── hls_build.tcl        HLS synthesis script
├── webui/
│   ├── server.py            Flask management server
│   └── templates/index.html Dashboard
└── logs/                    Captured audio / IQ logs
```

---

## Quick Start

### FM Radio

```bash
make fm FREQ=98.5          # build & play audio through speakers
make fm-scan               # scan the FM band and show signal levels
make fm-save FREQ=98.5     # record 30 s to logs/fm_*.wav
```

Audio is piped as raw 16-bit mono 48 kHz PCM to `aplay`.

### Digital Data Link (host / x86)

```bash
make link-all              # build link_tx, link_rx, link_test
make link-test             # software loopback — no hardware needed
make link-bench            # send 500 KB of random data over the air
make link-tx-run           # continuous TX from stdin (piped data)
make link-rx-run           # continuous RX to stdout (pipe to pv/file)
```

Options accepted by both `link_tx` and `link_rx`:

| Flag | Default | Description |
|------|---------|-------------|
| `--freq` | `434.0` | Center frequency MHz |
| `--rate` | `20` | Sample rate MSPS |
| `--mod` | `QPSK` | `BPSK` / `QPSK` / `QAM16` / `QAM64` |
| `--pluto` | `192.168.2.1` | PlutoSDR IP |
| `--encrypt` | off | AES-256-CTR per-frame encryption |
| `--key` | — | 32-byte key as 64 hex chars |
| `--source` / `--sink` | `stdin` / `stdout` | or `udp` |
| `--port` | `5005` / `5006` | UDP port |

### Web UI

```bash
make link-webui            # opens http://localhost:8080
```

The dashboard lets you start/stop TX, RX, or full-duplex, tune frequency and attenuation live (no restart), run loopback tests, and manage SOL8 modes.

---

## ARM Deployment (DSP runs on Zynq)

Running the link binaries **on the PlutoSDR** eliminates the IQ-over-Ethernet bottleneck (80 MB/s → 5 MB/s of decoded frames only).

```bash
# One-time: cross-compile liquid-dsp for ARM
git clone https://github.com/jgaeddert/liquid-dsp
cd liquid-dsp && ./bootstrap.sh
./configure --host=arm-linux-gnueabihf --prefix=/opt/arm-sysroot
make -j4 && make install

# Build & deploy
make link-arm              # cross-compile link_tx_arm, link_rx_arm
make link-deploy           # scp binaries to PlutoSDR

# Remote control via SSH
make link-start-rx         # start RX on device, stream frames back via UDP
make link-start-tx         # start TX on device, accept frames via UDP
make link-stop             # stop both
```

---

## SOL8 Daemon

`sol8` implements the core feature set of the DTC/Codan SOL8SDR2x1W radio:

| Mode | Description |
|------|-------------|
| `mesh` | IP MANET mesh via TUN interface (FDD) |
| `l2bridge` | Transparent Layer-2 bridge via TAP interface |
| `p2p-tx` | Unidirectional transmitter |
| `p2p-rx` | Unidirectional receiver |
| `scan` | Channel scan → `/tmp/sol8_scan.json` |
| `ranging` | RSSI-based distance estimation |

**Adaptive modulation** — automatically switches based on SNR:

| SNR | Modulation | Spectral efficiency |
|-----|-----------|-------------------|
| < 6 dB | BPSK | 1 bps/sym |
| < 13 dB | QPSK | 2 bps/sym |
| < 22 dB | 16QAM | 4 bps/sym |
| ≥ 22 dB | 64QAM | 6 bps/sym |

```bash
make sol8-test             # unit tests — no hardware needed
make sol8-mesh             # start FDD mesh (TX 434 MHz / RX 439 MHz)
make sol8-l2bridge         # transparent L2 bridge
make sol8-scan             # sweep 430–450 MHz in 1 MHz steps
make sol8-ranging          # print RSSI distance every second
```

SOL8 frame format (v2, 22-byte overhead):
```
[SYNC 4B 0xC0FFEE77] [VER 1B] [FLAGS 1B] [MOD 1B] [BW 1B]
[NODE_ID 4B] [SEQ 4B] [LEN 2B] [PAYLOAD N] [CRC32 4B]
```

---

## FPGA IP Core (optional)

Moves the DSP (RRC filter + QPSK demodulation) into the FPGA fabric, freeing the ARM entirely for framing.

```
[AD9363 LVDS] → [ADI AXI AD9361 IP] → [THIS IP] → [AXI DMA] → DDR
```

- Resources: ~800 LUTs, ~400 FFs, 2 DSP48 (Zynq-7010 has 80 — plenty)
- Throughput: 200 MSPS (10× margin over 20 MSPS requirement)

```bash
make link-fpga             # requires Vivado HLS 2019.1+
# Output IP at fpga/qpsk_modem/solution1/impl/ip/
# Add to Vivado IP catalog and wire to cf-ad9361-lpc M_AXIS_0
```

---

## Configuration

`config.json` is read by both the CLI tools and the web UI:

```json
{
  "radio":    { "freq_mhz": 434.0, "rate_msps": 20, "mod": "QPSK", "tx_atten_db": 10 },
  "link":     { "pluto_ip": "192.168.2.1", "source": "stdin", "sink": "stdout" },
  "security": { "encrypt": false, "key_hex": "0000...0000" },
  "sol8":     { "freq_tx": 434, "freq_rx": 439, "bw_mhz": 10, "mod": "AUTO" }
}
```

Live changes (no restart) are supported for TX attenuation, TX/RX frequency, and modulation via SIGUSR1 or through the web UI.

---

## Performance Summary

| Mode | Where DSP runs | Ethernet load |
|------|---------------|--------------|
| Host (default) | x86 PC | ~80 MB/s raw IQ |
| ARM (`--arm`) | Zynq Cortex-A9 | ~5 MB/s decoded frames |
| FPGA (future) | PL fabric | ARM framing only |

---

## Make Targets Reference

```
make build          Build rx_example (basic IQ test)
make fm             Build & run FM receiver
make fm-scan        FM band scan
make link-all       Build link_tx + link_rx + link_test (x86)
make link-test      Software DSP loopback
make link-arm       Cross-compile for Zynq ARM
make link-deploy    Build + copy ARM binaries to PlutoSDR
make link-webui     Start web UI at http://localhost:8080
make link-fpga      Synthesize FPGA QPSK IP (Vivado HLS)
make sol8           Build SOL8 daemon
make sol8-test      Run SOL8 unit tests (no hardware)
make clean          Remove all build artifacts
```
