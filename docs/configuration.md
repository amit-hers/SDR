# Configuration reference

The daemon reads a flat JSON object. Unknown keys are ignored. Missing keys use
the defaults compiled into `src/daemon/Config.hpp`. Because the parser is
minimal, use ordinary JSON without comments, nested objects, or escaped quote
characters in values.

## Radio and mode

| Key | Default | Meaning and constraints |
|---|---:|---|
| `mode` | `bridge` | `bridge`, `mesh`, `p2p-tx`, `p2p-rx`, or `scan` |
| `pluto_ip` | `192.168.2.1` | Plain device IP or explicit libiio URI such as `usb:1.2.3` |
| `freq_tx_mhz` | `434.0` | TX center frequency in MHz |
| `freq_rx_mhz` | `439.0` | RX center frequency in MHz; must match the peer's TX frequency |
| `bw_mhz` | `1` | Integer symbol rate in MHz, clamped to 1–20; sample rate is 4x. Use 1–2 for sustained RX |
| `tx_atten_db` | `10.0` | TX attenuation, clamped to 0–89 dB; 0 is maximum transmit power |
| `gain_mode` | `fast_attack` | Passed to the AD936x RX gain-control attribute |
| `modulation` | `BPSK` | `BPSK`, `QPSK`, `16QAM`, or `64QAM`; `AUTO` warns and becomes BPSK |
| `rx_bw_factor` | `1.4` | Analog RX bandwidth / symbol-rate ratio, clamped to 1.0–3.0 |
| `cfo_method` | `fft` | `fft` selects the FFT estimator; any other value currently selects grid search |

The radio's two ends must cross frequencies:

```text
node A TX 434 MHz -> node B RX 434 MHz
node B TX 439 MHz -> node A RX 439 MHz
```

## Medium access and receive pipeline

| Key | Default | Meaning and constraints |
|---|---:|---|
| `tx_duty_max` | `0.65` | Maximum local airtime fraction; clamped to 0–1. Values 0 or 1 disable practical throttling |
| `carrier_sense` | `true` | Defer TX while RX energy indicates peer activity |
| `carrier_sense_hold_ms` | `25` | Continue deferring this long after activity |
| `carrier_sense_max_defer_ms` | `60` | Maximum defer before allowing TX |
| `rx_buffer_samples` | `262144` | Requested samples per capture, clamped to 4096–262144 |
| `rx_queue_depth` | `8` | Configured queue depth, clamped to 2–256. Note: bridge code currently has an internal queue cap of 8 |
| `burst_block` | `256` | Samples per detector power block |
| `burst_threshold` | `3.0` | Detection threshold as a multiple of estimated noise power |
| `burst_margin` | `512` | Context samples retained around a burst |
| `burst_merge_gap` | `512` | Nearby burst windows closer than this are merged |
| `burst_noise_q` | `0.20` | Power quantile used for the noise estimate |
| `spectrum_interval_ms` | `200` | Minimum FFT display interval; 0 disables spectrum calculation |

## Network interface

| Key | Default | Meaning |
|---|---:|---|
| `tap_iface` | `sdr0` | TAP name in bridge mode; TUN name in mesh mode |
| `bridge_iface` | `br0` | Reserved/configured bridge name; current mode code does not create or manage it |
| `lan_iface` | `eth0` | Reserved/configured LAN interface; current mode code does not attach it to a bridge |
| `tap_mtu` | `1386` | Interface MTU; default fits an Ethernet packet inside `MAX_PAYLOAD=1400` |

## Reliability and security

| Key | Default | Meaning and constraints |
|---|---:|---|
| `fec` | `false` | Reed-Solomon FEC; both peers must agree |
| `encrypt` | `false` | AES-256-CTR encryption; both peers must agree |
| `aes_key_hex` | empty | Exactly 64 hexadecimal characters when encryption is enabled |
| `arq` | `false` | Selective-repeat reliability; bridge mode only |
| `arq_window` | `16` | Outstanding-frame limit, minimum 1 |
| `arq_timeout_ms` | `80` | Initial retransmission timeout, minimum 1 ms |
| `arq_max_retries` | `5` | Retries before drop, minimum 0 |

AES-CTR provides confidentiality but, by itself, does not authenticate peers or
protect against deliberate modification. The frame CRC is an error detector,
not a cryptographic message-authentication code. Protect key files and use this
feature only with a threat model that accepts those limitations.

## Runtime and scan

| Key | Default | Meaning and constraints |
|---|---:|---|
| `node_id` | `0x00000001` | Parsed as a base-auto unsigned 32-bit number; invalid input silently becomes 1 |
| `stats_interval_ms` | `1000` | JSON statistics write interval |
| `stats_path` | `/tmp/sdr_stats.json` | Statistics output path |
| `monitor_port` | `8080` | Metadata used by scripts; the C++ daemon does not open an HTTP port |
| `rt_priority` | `0` | SCHED_FIFO priority, clamped to 0–90; 0 disables it |
| `pin_cores` | `false` | Pin bridge capture/DSP threads to CPU cores |
| `scan_start_mhz` | `430.0` | First scan frequency |
| `scan_step_mhz` | `1.0` | Frequency increment |
| `scan_n` | `20` | Number of scan points |

## Diagnostic environment variables

These apply primarily to bridge mode:

| Variable | Value |
|---|---|
| `SDR_PROFILE` | `1` enables per-stage CPU reporting at shutdown |
| `SDR_FRAME_LOG` | File path for decoded frame/header diagnostics |
| `SDR_RAW_LOG` | File path for raw demodulated bytes in hexadecimal |
| `SDR_IQ_DUMP` | File path for interleaved `int16` I/Q capture |
| `SDR_IQ_DUMP_MB` | Maximum I/Q dump size in MiB |
| `SDR_RX_CARRIER` | `ls+costas` (default), `ls`, `costas`, or `none` |
| `SDR_TSYNC` | Set to `fixed` to select experimental fixed-point timing recovery |

## Live reload

Write a complete valid configuration to `/tmp/sdr_reload.json`, then signal the
daemon:

```bash
sudo kill -USR2 "$(pidof sdr-datalink)"
```

Only `tx_atten_db`, `freq_tx_mhz`, and `freq_rx_mhz` are applied. All other
changes require a restart. This path is process-global, so it is unsuitable
for independently tuning two daemons on the same host.
