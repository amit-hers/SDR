# Against the 5-10 Mbit/s and 10 ms targets

Both are currently missed, for different reasons, and one change addresses both.

## Throughput: measured ceiling is ~3.5 Mbit/s, not 5-10

Same link, RSSI 68-69 dB, 20 s per point, 1200-byte payloads.

| rate (MS/s) | demod (Mbit/s) | theoretical | % of theory | PER | **goodput** |
|---|---|---|---|---|---|
| 2.40 | 1.193 | 1.20 | 99.4% | 0.00% | 1.083 |
| 3.84 | 1.902 | 1.92 | 99.1% | 0.00% | 1.725 |
| 7.68 | 3.823 | 3.84 | 99.6% | 0.03% | **3.462** |
| 15.36 | 5.637 | 7.68 | **73.4%** | 0.28% | 3.381 |
| 17.28 | 3.725 | 8.64 | **43.1%** | **96.00%** | 0.000 |

Up to 7.68 MS/s the chain runs at 99%+ of theoretical. Above it, it breaks
down: at 15.36 MS/s only 73% of the demodulated stream is captured and goodput
actually FALLS below the 7.68 figure; at 17.28 MS/s the link collapses
entirely at 96% PER.

So **3.46 Mbit/s is the honest current ceiling**, about a third of the way into
the 5-10 Mbit/s band.

### RETRACTED: "the demodulator saturates at ~11 MS/s"

That claim, and the capture/DMA claim before it, were both wrong. Both came from
the same measurement fault: **the transmit feed ran out partway through the
capture**, so the tail of each run recorded silence. The 20.3 MB feed lasts about
18.8 s at 17.28 MS/s, shorter than the captures using it.

This project has now recorded that same fault three times -- a capture outrunning
its feed, RSSI sampled after a feed ended, and this. The check is cheap: confirm
the feed is still alive AFTER the capture and sample RSSI DURING it.

### Corrected: the demodulator scales cleanly to 15.36 MS/s

Feed rebuilt to 71.1 MB (65 s at 17.28 MS/s), so it outlasts every capture.
Two independent 30 s runs at 15.36 MS/s, RSSI 69 dB:

| run | demod | frames | PER | goodput | duty |
|---|---|---|---|---|---|
| 1 | 7.660 Mbit/s | 21763 | **0.00%** | **6.960 Mbit/s** | 100.0% |
| 2 | 7.660 Mbit/s | 21764 | **0.00%** | **6.960 Mbit/s** | 100.0% |

| fs | theoretical | measured | % of theory | PER | goodput |
|---|---|---|---|---|---|
| 7.68 MS/s | 3.84 | 3.817 | 99.4% | 0.00% | 3.461 |
| **15.36 MS/s** | 7.68 | **7.660** | **99.7%** | **0.00%** | **6.960** |
| 17.28 MS/s | 8.64 | 3.888 | 45.0% | 94.00% | 0.003 |

**The 5-10 Mbit/s throughput target is MET at 15.36 MS/s: 6.96 Mbit/s of
payload at zero measured frame loss, repeatable.**

17.28 MS/s still fails, and that one is real rather than a measurement fault --
RSSI was 69.5 dB and the feed was confirmed alive throughout. It sits just below
the documented 17.5 MS/s modem-clock ceiling, so there is very little headroom
there; 15.36 MS/s is the rate to build on.

## Latency: the floor is structural, 30-137 ms

The deframer cannot run until a whole DMA transfer has arrived, and
`PKT_BYTES` is **32768**. So the receive path cannot deliver a frame sooner than
the time it takes to fill one packet, regardless of anything else:

| rate | demod rate | fill time at 32768 B |
|---|---|---|
| 3.84 MS/s | 1.92 Mbit/s | **136.5 ms** |
| 7.68 MS/s | 3.84 Mbit/s | **68.3 ms** |
| 15.36 MS/s | 7.68 Mbit/s | **34.1 ms** |
| 17.28 MS/s | 8.64 Mbit/s | **30.3 ms** |

Every value is far above 10 ms, and the appliance's 3.84 MS/s is the worst of
them. **No amount of tuning outside the FPGA will meet a 10 ms target at this
packet size.** `fpga/bd/sdr_insert.tcl` already states the trade: "The cost is
latency: a transfer does not complete until PKT_BYTES bytes have..."

For a 10 ms budget the packet must not exceed:

| rate | max PKT_BYTES for 10 ms |
|---|---|
| 3.84 MS/s | 2400 B |
| 7.68 MS/s | 4800 B |
| 15.36 MS/s | 9600 B |

## One change serves both targets

At **15.36 MS/s with PKT_BYTES = 8192**: 7.68 Mbit/s theoretical (inside the
5-10 band) and an 8.5 ms fill time (inside the 10 ms budget). At 7.68 MS/s with
4096 B the latency is also 8.5 ms, but throughput stays at 3.46 Mbit/s.

So the target combination is **a higher sample rate AND a smaller packet**, and
both halves currently fail: 15.36 MS/s does not sustain capture, and a 4 KiB
packetizer was reported to break acquisition.

## What has NOT been measured

**Actual end-to-end latency.** Everything above is the packetizer's floor,
computed from measured demodulated byte rates -- it is a lower bound, not a
measurement. The real figure adds bridge capture, framing, TX DMA buffering,
demodulator processing and injection.

A one-way measurement is not possible while both RJ45 ports share a switch:
UNIT-B receives every frame over copper as well as over RF, so the two arrivals
cannot be separated. Measuring it needs the same isolated topology Phase 8 does.
