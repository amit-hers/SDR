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

### The ceiling is in the demodulator, not the DMA

**Corrected.** This was first recorded as a capture/DMA limitation. It is not.
At 15.36 MS/s the receive path is clean by every available measure:

    DMA gap between consecutive packets: median 0 B, mean 0 B, p90 0 B, max 0 B
    capture duty cycle: 100.0% of the demodulated stream reached userspace
    PER 0.34%, CRC failures 1, payload mismatches 0

Nothing is lost in capture. And the sample interface is running at full rate --
`l_clk_mon` reads 5033 / 10066 / 20132 at 3.84 / 7.68 / 15.36 MS/s, exactly
doubling each time, a constant 1310.7 counts per MS/s. So the AD9363 is
delivering samples as configured.

What saturates is the demodulator's own output:

| fs | theoretical | measured | % of theory | behaves like |
|---|---|---|---|---|
| 3.84 MS/s | 1.92 Mbit/s | 1.902 | 99.1% | 3.80 MS/s |
| 7.68 MS/s | 3.84 Mbit/s | 3.823 | 99.6% | 7.65 MS/s |
| 15.36 MS/s | 7.68 Mbit/s | 5.520 | **71.9%** | **11.04 MS/s** |
| 17.28 MS/s | 8.64 Mbit/s | 3.725 | **43.1%** | 7.45 MS/s |

The core tracks the sample rate exactly up to 7.68 MS/s and then stops scaling,
flattening at roughly 5.5 Mbit/s of output -- as though it were fed about
11 MS/s. At 17.28 MS/s it is worse than at 7.68, which looks like intermittent
loss of lock rather than a clean throughput limit.

**This redirects the throughput work.** A smaller `PKT_BYTES` will not raise
throughput, because the packetizer is not the constraint; and raising the sample
rate past 7.68 MS/s buys 44% more at 15.36 and nothing at all at 17.28. The
5-10 Mbit/s target needs the demodulator core itself to sustain a higher symbol
rate. `docs/fabric-modem.md` puts the modem clock ceiling at 35 MHz / 2 =
17.5 MS/s, so the limit being hit here at an effective 11 MS/s is below that and
is worth understanding before any FPGA rebuild is attempted.

Note the project history records 7.85 Mbit/s at 17.28 MS/s. That is not
reproduced here and should not be relied on until it is: this run collapses at
that rate.

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
