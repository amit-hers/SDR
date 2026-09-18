# Phase 9: where the structural loss actually is

Two different structural losses were being conflated. They have different
causes, different sizes, and different fixes.

## Measured: the receive DMA is not gapless

Capture rate against libiio buffer size, 10.02 s per point, UNIT-B idle, no
other load. A gapless DMA's throughput cannot depend on buffer size, so the
trend is itself the result:

| libiio buffer | bytes captured | rate (B/s) |
|---|---|---|
| 16,384 B | 999,352 | 99,736 |
| 32,768 B (appliance default) | 1,002,086 | 100,009 |
| 65,536 B | 1,005,304 | 100,330 |
| 262,144 B | 1,007,974 | 100,596 |
| 1,048,576 B | 1,011,150 | 100,913 |

Fitting `1/R = (1/R_true)(1 + L/S)` -- the model for losing `L` bytes while the
buffer is re-armed -- gives **R_true = 100,715 B/s, L ~= 179 B per refill**,
R^2 = 0.887. The R^2 says the per-refill loss is not a clean constant, so treat
179 B as an estimate, not a design figure.

Loss to refills at each size: 1.08%, **0.54%**, 0.27%, 0.07%, 0.02%.

## The two losses

1. **Refill byte loss -- 0.54%** at the appliance's `-b 8192`. Purely a
   function of buffer size, and cheap to reduce.
2. **Frame straddle loss -- ~4%.** `OffsetDeframer` restarts at every
   32,768-byte DMA packet, so a frame crossing a boundary is lost. At ~25
   frames per packet that is ~1 frame per boundary.

The link between them is the useful part. Refills happen per **libiio buffer**;
the deframer resets per **DMA packet** (`PKT_BYTES`, RTL). Today those coincide
exactly -- buffer = 32,768 B = one packet -- so every packet boundary is also a
genuine discontinuity. That is *why* the deframer must reset, and why the 4% is
currently unavoidable.

## The hypothesis, and what would settle it

A 1 MB libiio buffer holds 32 DMA packets with no refill between them. **If**
those 32 are contiguous, the deframer can carry state across the 31 internal
boundaries and only 1 boundary in 32 breaks continuity: ~4% -> ~0.13%, with the
refill loss going 0.54% -> 0.02%. No FPGA change; `PKT_BYTES` stays at its
documented knee.

**This is not yet established.** The measurement above proves refills cost
bytes; it does not prove packets within one buffer are contiguous.

An attempt to settle it by AD9363 digital loopback on a single unit did not
work: three configurations were tried -- pre-modulated IQ with the fabric
modulator enabled, the same with it bypassed (`mod_en=0`), and the product path
feeding a byte stream with `mod_en=1` -- and the captures contain **zero frame
sync words at all four symbol phases**, so the demodulator recovered nothing.
That is a link-level failure in the loopback setup, not an analysis error, and
it is where this should resume. The earlier successful loopback run (632 frames,
0 mismatches) is the reference to reproduce first, before trusting any
contiguity result.

## Unrelated finding: the appliance transmits at 0 dB attenuation

`tx_fabric.sh` sets `out_voltage0_hardwaregain` to `0.000000 dB` -- full output.
That is the state a unit is in after autostart. Before connecting two units by
coax, confirm the inline attenuation suits full TX power into an AD9363 receive
input.
