# Architecture

## Overview

The executable is a controller around a shared PlutoSDR device and one selected
operating mode. Core radio, modem, framing, DSP, security, transport, and
statistics code is built into the static `sdr_core` library.

```text
Linux packet source
  TAP / TUN / UDP
        |
        v
 framing -> optional AES/FEC -> split modulation -> RRC interpolation
        |
        v
     libiio -> PlutoSDR -> RF
        |
        v
 burst detection -> synchronization -> demodulation -> deframing
        |
        v
  TAP / TUN / UDP + JSON statistics
```

`src/daemon/main.cpp` loads a flat JSON configuration and creates `Controller`.
The controller connects to the radio, applies RF settings, chooses the mode,
starts statistics export, and handles shutdown and limited live reload.

## Components

| Area | Primary paths | Responsibility |
|---|---|---|
| Hardware | `include/sdr/hardware`, `src/core/hardware` | libiio discovery, configuration, RX/TX buffers |
| Framing | `include/sdr/framing`, `src/core/framing` | wire frames, CRC, aggregation, selective-repeat ARQ |
| Modem | `include/sdr/modem`, `src/core/modem` | BPSK/QPSK/QAM symbol mapping and split acquisition/payload modulation |
| DSP | `include/sdr/dsp`, `src/core/dsp` | RRC, AGC, burst detection, timing and carrier synchronization, spectrum |
| FEC/crypto | `include/sdr/fec`, `include/sdr/crypto` | Reed-Solomon and AES-256-CTR transforms |
| Transport | `include/sdr/transport`, `src/core/transport` | TAP/TUN, UDP, and single-producer/single-consumer rings |
| Daemon modes | `src/daemon/modes` | End-to-end packet and sample pipelines |
| Observability | `include/sdr/stats`, `src/daemon/StatsExporter.*`, `src/monitor` | Counters, JSON snapshots, web dashboard |

## Bridge-mode transmit path

1. Read Ethernet packets from the TAP interface.
2. Aggregate small packets into a payload when possible.
3. Add frame metadata, sequence number, flags, and CRC.
4. Apply encryption and/or FEC when configured.
5. Encode the acquisition portion in BPSK and the payload in the configured
   modulation.
6. Apply root-raised-cosine interpolation and convert to interleaved `int16`
   I/Q samples.
7. Observe carrier-sense and duty-cycle limits, then push samples through
   libiio.
8. When ARQ is enabled, retain outstanding frames and retransmit on timeout.

## Bridge-mode receive path

Bridge mode separates capture and DSP so processing does not stop libiio
reception. The capture thread fills a bounded queue. If DSP falls behind, the
oldest queued buffer is dropped and the `dropped` counter increases.

The DSP thread detects energy windows, estimates coarse frequency offset,
locates the known BPSK preamble, performs timing/carrier recovery, decodes the
header, then demodulates the payload according to the header's modulation
code. Valid aggregate records are written individually to TAP. ACK control
frames are handed back to the TX thread through an SPSC ring.

## Other modes

- `mesh` uses a TUN interface and transports IP packets. It does not implement
  peer discovery, route exchange, forwarding tables, or multi-hop routing.
- `p2p-tx` binds UDP port 5005 and converts received datagrams into QPSK radio
  frames. `p2p-rx` writes decoded datagrams through the UDP socket abstraction.
  Review addressing behavior in `UDPSocket` before integrating it into an
  application.
- `scan` retunes across `scan_n` frequencies, measures received power, writes
  `/tmp/sdr_scan.json`, and exits.

Bridge mode contains the newest synchronization, aggregation, carrier-sense,
and ARQ work. Do not assume the other modes have feature parity.

## Wire frame, version 3

```text
32 B preamble (0xAA)
4 B sync (0xC0FFEE77)
1 B version (0x03)
1 B flags
1 B modulation code
1 B bandwidth code
4 B node ID, big-endian
4 B sequence number, big-endian
2 B payload length, big-endian
N B encoded payload
4 B CRC32, little-endian
16 B postamble (0xAA)
```

The fixed header is 18 bytes and header-plus-CRC overhead is 22 bytes. With
preamble and postamble, fixed wire overhead is 70 bytes. Maximum payload is
1400 bytes; a TAP MTU of 1386 leaves room for the 14-byte Ethernet header.

Flags are `ENCRYPT=0x01`, `FEC=0x02`, `ACK=0x04`, `CTRL=0x08`, and
`AGGR=0x10`. Acquisition (preamble through header) is always BPSK. Payload and
CRC use the modulation declared in the header.

## Concurrency and shutdown

Bridge mode uses TX, capture, DSP, and statistics threads. Mesh uses TX/RX
threads; P2P uses one active direction; scan uses one worker. `SIGINT` and
`SIGTERM` request mode shutdown. `SIGUSR2` asks the controller to load
`/tmp/sdr_reload.json`; only TX attenuation and TX/RX frequencies are applied
live.
