# The two-unit modem link works: root cause and fix

**A->B PER 0.67%, B->A PER 0.00%, payload mismatches 0 in both directions.**
Reproduced with no manual intervention after the fix below.

## Root cause: the demodulator comes up stalled

`qpsk_demod` initialises into a stalled state and stays there. In that state:

- `mu_clamped` (`0x43C00030`) rails -- `0x7C` and `0xD0` observed
- the output symbol distribution looks *healthy*, within a few percent of 25%
  per symbol
- **not one frame decodes**, at any signal level

Every register reads correct, the settings match on both ends, and the signal
can be 24 dB above the noise floor. So it presents as an RF problem, or a
framing problem, or a modulator problem -- and is none of them. This single
fault produced every failure in this investigation: the single-board loopback,
both antenna tests, and both directions of the first two-unit test.

The fix is a `soft_reset` (`0x43C00020`) **with the demodulator's output being
drained**. A stalled core ignores the reset while its output is backpressured,
so something must be emptying the RX DMA at the moment the reset is pulsed.
After it, `mu_clamped` sits at `0x02`-`0x04`, `lock_count` restarts and advances,
and the link runs.

`rx_framed.sh` never issued that reset. It does now.

Use `iio_readdev` for the drain, not `dd`: a raw `read()` on `/dev/iio:*` does
not program the DMA on 6.12, so `dd` blocks forever and holds the device
single-open, wedging it for everything afterwards. And pass the device NAME --
neither the sysfs path nor `iio:deviceN` works, and the failure is a silent
0-byte capture.

## What the chain was proven to be, before the fix was found

Each stage was verified independently rather than inferred:

| stage | measurement |
|---|---|
| fabric modulator output | correlation **1.0000** against the host reference, on **both** units |
| RF path to the demodulator input | correlation **0.9936**, CFO +0.0 kHz (control: 0.155) |
| demodulator output | balanced symbols, zero sync words |

That sequence is what localised the fault. It also **corrects an earlier
conclusion in this project**: the fabric modulator was named the least-proven
element and the most likely culprit, and it is in fact bit-exact on both boards.

## Results

Settings written and read back from both boards and compared before each run,
including the transmitter's `mod_diff` against the receiver's `dem_diff`.
7.68 MS/s, 434 MHz, differential QPSK, 1200-byte payloads, TX at 0 dB, RSSI
81-85 dB at the far end.

| | A -> B | B -> A |
|---|---|---|
| packets anchored | 100% | 100% |
| capturable / decoded | 3597 / 3573 | 3598 / 3598 |
| PER | 0.67% | **0.00%** |
| CRC failures | 24 | 0 |
| **payload mismatches** | **0** | **0** |
| DMA gap median / max | 0 B / 1 B | 0 B / 0 B |
| capture duty cycle | 100.0% | 100.0% |

## Consequence for Phase 9

**The DMA gap is 0 B.** Median, mean and p90 are all zero; the maximum seen is
1 byte across a whole capture, and 100% of the demodulated stream reached
userspace. The structural loss that Phase 9 exists to eliminate is not present
in these runs.

This supersedes the earlier estimate of ~179 bytes lost per buffer refill in
`phase9-dma-loss.md`. That figure came from fitting capture *rate* against
buffer size, an indirect method; this is a direct measurement of the gap between
consecutive packets in a decoded stream, and it should be believed instead.
Phase 9 should be re-scoped against this measurement before any work is done on
buffer sizes or deframer state.
