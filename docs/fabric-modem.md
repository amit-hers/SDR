# The fabric modem

A QPSK modem implemented in the Pluto+ programmable logic: `qpsk_mod` and
`qpsk_demod` (Vitis HLS) plus supporting RTL, spliced into the ADI reference
design by `fpga/bd/sdr_insert.tcl`.

This is a different path from the host-side daemon described in
[Architecture](architecture.md). The daemon modulates in software and streams IQ
over libiio; the fabric modem does it in the PL, and the host sees a **byte
stream** instead. That distinction is what makes real payload rates possible:
IQ at 17.28 MS/s is 4 bytes per sample — 69 MB/s, which the Pluto's
USB-ethernet cannot carry — while the same link as bytes is about 1.08 MB/s.

## Measured performance

Two boards over the air at 434 MHz, 1200-byte payloads, differential QPSK,
scrambler on, `PKT_BYTES` 32768, 16 s continuous:

```
frames on air   13210
wholly captured 12709 (96.2%)
decoded         12709   MISSING 0   PER 0.00%
CRC failures    0       payload mismatches 0
GOODPUT         7.85 Mbit/s     raw PHY 8.64, frame efficiency 94.5%
```

All four quarters of the run read 0.00%. A 121 s soak moved 131,052,014 bytes at
exactly 17.28/16 MB/s with `l_clk`, `ap`, RSSI and `mu_clamped` unchanged end to
end, and no reset.

| sample rate | `PKT_BYTES` | delivery | goodput | PER |
|---|---|---|---|---|
| 14.40 MS/s | 8192 | 84.9% | 5.78 Mbit/s | 0.00% |
| 17.28 MS/s | 8192 | 84.7% | 6.92 Mbit/s | 0.01% |
| **17.28 MS/s** | **32768** | **96.2%** | **7.85 Mbit/s** | **0.00%** |

Cold-start acquisition: 30/30 trials, median 900 symbols (0.47 ms), worst 2408.
Re-acquisition after a DMA packet boundary: median 10 bytes (40 symbols,
0.009 ms), worst 52 bytes, over 384 boundaries.

### The ceiling, and where the losses actually are

`17.28 MS/s` is **98.7% of the fabric ceiling**, which is `modem_clk / II` =
35 MHz / 2 = 17.5 MS/s. Going faster needs a higher modem clock; the critical
path measured ~27.0 ns, so about 37 MHz (18.5 MS/s) is the limit and 40 MHz will
not close.

**14.4 MS/s cannot reach 7 Mbit/s whatever the link does.** Seventy bytes of
overhead on a 1270-byte frame caps goodput at 6.80 Mbit/s even at perfect
delivery.

**Delivery is `1 - wire_frame/PKT_BYTES` and nothing else.** The DMA is re-armed
after every transfer and the demodulator does not stop for it, so a frame that
straddles a packet boundary loses the re-arm gap and dies. The stream itself is
99.8% continuous — the gap is a median of 2–16 bytes — but that small hole kills
a whole frame. This, not error rate, is what limited throughput at every point in
the table above: PER among wholly-captured frames was already 0.00–0.01%.

## Wire format requirements the modem imposes

### Differential encoding is mandatory

A QPSK slicer is four sign tests, so the receiver recovers the constellation only
up to a 90° rotation and nothing in a continuous stream resolves which. Encoding
the *difference* between consecutive symbols removes the ambiguity outright: a
constant rotation cancels. Set `diff_mode = 1` on **both** ends
(`0x43C10020` on the modulator, `0x43C00028` on the demodulator) — a mismatch
decodes at chance and looks like an RF fault.

### Scrambling is mandatory

Differential QPSK maps a byte to a phase *increment*, so a run of identical bytes
is a run of identical symbols: the constellation stops moving and the Gardner
timing detector has nothing to measure. Measured over the air on two frames alike
in length, wire size and position, differing only in content:

```
200 B pseudorandom ... frame loss  0.11%
200 B of 0x00 ....... frame loss 25.87%
```

Reed-Solomon reaches the same failure from the other side by zero-padding a short
payload out to 223 bytes: 32 B + RS measured 34.14% against 0.12% for 200 B + RS.

`include/sdr/framing/Scrambler.hpp` is therefore applied unconditionally in
`Framer::encode`, after FEC and AES (so it covers RS padding) and before the CRC
(so the CRC verifies the bytes actually sent). It is an additive 15-bit LFSR,
x¹⁵+x¹⁴+1, seeded from the sequence number — additive rather than
self-synchronising so a wire error stays one error instead of being fed back
through the register and tripled. H.264 is full of zero runs, so this is not
optional for video.

### The receiver needs four deframers

The demodulator packs four 2-bit symbols per byte starting at an arbitrary
symbol, so its byte grid sits 0, 2, 4 or 6 bits off the transmitter's. A single
`Deframer` sees the sync word split across two bytes in three cases out of four.
Run four instances on bit-rotated copies of the stream and accept whichever
produces a valid CRC.

## Register map

| address | core | registers |
|---|---|---|
| `0x43C00000` | `qpsk_demod` | `0x10` enable, `0x18` lock_count (RO), `0x20` soft_reset, `0x28` diff_mode, `0x30` mu_clamped (RO) |
| `0x43C10000` | `qpsk_mod` | `0x10` enable, `0x18` bpsk_mode, `0x20` diff_mode |
| `0x43C20000` | TX IQ probe | `0x00` ctrl, `0x08` stat |
| `0x43C30000` | RX IQ probe | `0x00` ctrl, `0x08` stat |
| `0x43C40000` | DAC pin probe | `0x00` ctrl, `0x08` stat |
| `0x43C50000` | identity | `0x00` MAGIC, `0x04` FPGA_VERSION, `0x08` FPGA_ABI, `0x0C` REGISTER_MAP_VERSION, `0x10` BUILD_EPOCH, `0x14` GIT_SHA |

HLS places `s_axilite` ports on an 8-byte grid of its own choosing. Request the
offsets explicitly and **check the generated `*_hw.h` after any interface
change**: adding one scalar once pushed `bpsk_mode` from `0x18` to `0x24` and
silently broke every script writing the old address. See
[Deployment](DEPLOYMENT.md#version-and-abi-fields) for the identity block and the
ABI gate that exists because of exactly that failure.

## Bring-up

Order matters, and every step below was established by measurement.

```bash
# receive
fpga/scripts/rx_framed.sh 17280000

# transmit from a byte stream through the fabric modulator
fpga/scripts/tx_fabric.sh 17280000 434000000 1
fpga/scripts/tx_feed.sh /tmp/stream.bytes

# transmit pre-modulated IQ from a cyclic buffer instead
fpga/scripts/tx_cyclic.sh /tmp/tx.iq 1300480 17280000
```

* **Modem cores before ADC channels.** The other order latches the sticky
  overflow flag on the first sample and it never clears.
* **ADC channel format `0x51`, not `0x71`.** `dfmt_type` must be 0 on core
  10.03, or the demodulator input rails at 12-bit full scale regardless of RF —
  identical with the transmitter off, at every gain. Re-measure after any core
  version change rather than carrying the value forward.
* **DAC datarate (`0x7902404C`) must be 1.** It defaults to 0, which makes
  `dac_valid` fire every `l_clk` cycle and plays the waveform out at twice the
  intended rate; samples-per-symbol collapses 4→2. The stock driver does not set
  it because `sdr_insert.tcl` removed the TX FIR its calculation assumes.
* **Select the DMA source *after* opening the buffer.** Opening it re-points the
  channel at the internal DDS, so an earlier write is silently undone and the DAC
  emits nothing while every register still reads healthy.
* **Unmask the DMA IRQ.** Every PL reload re-masks it, after which transfers
  complete in hardware while userspace times out.
* **Relax the watchdog** (`fpga/scripts/watchdog_relax.sh`). Stock is a ten
  second timeout, outrun by a multi-megabyte transfer or a `devmem` burst; the
  board then resets and, because the rootfs is a ramdisk, comes back with the
  bitstream gone.

`fpga/scripts/fpga_abi_check.sh` should run before any of this and refuses on a
version mismatch.

## Measurement tools

`fpga/tools/framed_link_test.cpp` — build and modes:

```bash
g++ -O2 -std=c++17 -I include -o framed_link_test \
    fpga/tools/framed_link_test.cpp build/src/core/libsdr_core.a -lliquid -lcrypto

framed_link_test gen  <n> <out.iq> <out.bytes>          # numbered test frames
framed_link_test per  <cap> <ref> <txlen> [pkt]         # per-frame loss
framed_link_test ser  <cap> <ref> <txlen> [pkt]         # byte/symbol error rate
framed_link_test head <cap> <ref> <txlen> <pkt> <brate> # re-acquisition
framed_link_test acq  <pkt> <brate> <cap...>            # cold-start acquisition
framed_link_test genvid <file> <out.bytes>              # frame a real file
framed_link_test rxvid  <cap> <file> <pkt> <brate>      # reassemble it
```

**Pass the correct `pkt`.** The byte grid is continuous only *within* one DMA
transfer, so every alignment and every "was this frame wholly captured" decision
is scoped to it. Analysing a 1024-byte capture as if it were 8192 reports 22.37%
loss against a true 0.19% — a fabricated regression.

## Traps in the measurement itself

More than one conclusion in this project's history has been an artifact rather
than a fault. In rough order of how much time each cost:

* **`best_run` is not a link metric.** The longest exact run inside a capture
  says nothing about the rest of it; a "254 bytes exact, 100% lock" reading was
  3.1% of the stream at 65% overall SER.
* **A DMA capture is not continuous.** Reads return whole packets with gaps
  between them. Aligning one capture globally gave 65% SER; aligning per packet
  gave 1.19% on the same bytes.
* **`iq_probe_read.sh` re-arms the probe as its first act**, so calling it after
  a feed has stopped captures an idle core and reports all zeros. The probe fills
  in ~0.95 ms and a 200 KB feed drains in 0.185 s, so the feed must still be
  running at the instant of arming.
* **RSSI cannot see this signal.** The modulator's output is ~17 dB below a
  full-scale DDS tone and does not move RSSI at all. Confirm the link from
  demodulated bytes, never from RSSI.
* **A capture can begin with stale buffer content.** Two frames once passed CRC
  carrying payloads from the *previous* transmission. Do a discard read first.
* **`/dev/iio:device4` is single-open**, and a drain blocked inside the driver
  does not die on `SIGKILL` — dropping `buffer/enable` is what returns it. See
  `fpga/scripts/free_capture_dev.sh`.
* **PSD ripple and EVM need care.** "22.5 dB in-band ripple" was estimation
  variance from averaging three segments; at 124 segments it is 5.8 dB, i.e.
  flat. EVM read 19.4% without derotation and 14.6% with it.

## Known limitation

Streaming a real file through the **fabric modulator** does not yet decode: a
byte stream that gives PER 0.00% when modulated on the host produces no frames
when modulated on-chip, on the same demodulator and the same air. The TX chain
does carry signal — the TX IQ probe reads full-amplitude IQ when armed correctly
— so the fault is in the waveform or the byte path, not a dead transmitter. This
blocks live video, since host-modulated IQ cannot be streamed at 69 MB/s.
