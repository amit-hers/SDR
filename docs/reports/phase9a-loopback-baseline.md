# Phase 9A: attempt to reproduce the single-board loopback baseline

**Result: NOT reproduced.** Four configurations, zero frame sync words in every
capture. Recorded here so the next attempt starts from what has been eliminated
rather than repeating it.

## The reference, recovered from history

It was **not** AD9363 digital loopback. Commit `7f77e83` describes single-board
**RF self-loopback** on UNIT-B at 7.68 MS/s -- its own fabric modulator on the
air, its own demodulator receiving:

    92 of 100 packets anchored
    2246 frames decoded of 2281 capturable    PER 1.53%
    CRC failures 18    payload mismatches 0

The same commit already warned this is not a test bed:

> "The loopback is a marginal channel... the clean run did not reproduce, later
> attempts giving 0 anchored packets but still 10 CRC failures... RSSI sits at
> 111-117 dB with RX gain pinned at maximum. Attenuating the transmitter kills
> it outright at both 10 and 20 dB, so the coupling is weak rather than
> saturating... Meeting 'CRC failures 0, payload mismatches 0' needs two boards
> with real antenna coupling."

`71d85ea` adds that 17.28 MS/s destabilises single-board loopback, so 7.68 MS/s
is the rate to use.

## What was tried

All at fs=7,680,000, LO 434 MHz, RX gain 73 dB (max), slow_attack, `mod_en=1`,
`dem_en=1`, reference stream 50,800 B (40 numbered frames, 1200 B payload).

| # | loopback | TX atten | feed path | sync words | captured |
|---|---|---|---|---|---|
| 1 | digital | -50 dB | raw `iio_writedev` | 0 | 5.14 MB |
| 2 | digital | -50 dB | raw, `mod_en=0` | 0 | 5.09 MB |
| 3 | digital | 0 dB | raw `iio_writedev` | 0 | 4.10 MB |
| 4 | RF | 0 dB | raw `iio_writedev` | 0 | 3.84 MB |
| 5 | RF | 0 dB | **`tx_feed.sh`** | 0 | 4.04 MB |
| 6 | digital | 0 dB | **`tx_feed.sh`** | 0 | 4.02 MB |

Sync words were counted directly in the raw capture at all four symbol phases
(0/2/4/6 bits) and inverted, rather than trusting the analysis tool -- whose
`PER 0.00%` was meaningless because `capturable` was 0. **A `capturable` of 0 is
NO SIGNAL, not success.**

## Two of my own errors, both now fixed

**Attenuating the transmitter.** Runs 1, 2 and 4 used -50 dB "for safety". The
record is explicit that 10 dB and 20 dB each killed this loopback outright. This
invalidated the first three attempts.

**Feeding with raw `iio_writedev`.** Runs 1-4 bypassed `tx_feed.sh`, which
documents the trap plainly: opening the buffer re-points the DAC at its internal
DDS, so a source selection made earlier is silently undone and "the DAC emits
nothing while every register still reads healthy". With `tx_feed.sh` the log
confirms `src=0x00000002` and 5,080,000 B delivered in 10 s -- the correct byte
rate for this sample rate.

## What is eliminated

- **Transmit path.** DAC source correct (`src=0x2`), feed delivers at the right
  rate, and RSSI responds to it: 114.25 dB idle -> 107.5 dB feeding -> 114.75 dB
  after, repeatably.
- **ADC data format.** `0x51` on both channels, ADC core `0x000A0300` (10.03).
  `rx_framed.sh` sets it and the comment names the trap. Not `0x71`.
- **Sample rate.** 7.68 MS/s, below the rate that destabilises loopback.
- **Analysis parameters.** Sync words absent from the raw bytes at every phase,
  so this is not a scoping or `txlen` error.

## Second session: instrumenting the demodulator's input

The RX IQ probe (`0x43C30000`, the demodulator's own input) was used to see
whether the modulated signal reaches the demodulator at all. Three cases, each
with a transmit feed in flight:

| case | loopback | TX atten | probe words | non-zero | RSSI |
|---|---|---|---|---|---|
| dig0 | 1 | 0 dB | 2048 | **0** | 113.75 dB |
| dig89 | 1 | -89 dB | 2048 | **0** | 114.75 dB |
| rf0 | 0 | 0 dB | 2048 | **0** | 114.25 dB |

The discriminator was deliberate: a **digital** loopback sits upstream of the RF
chain, so attenuating the transmitter by 89 dB could not change what the
receiver sees. Nothing changed between any of the three, which is consistent
with the attribute being inert -- but it cannot be concluded from this, because
the probe read zero in *every* case including plain RF.

Two further things were eliminated along the way:

- **The `dd` drain recipe in `iq_probe_read.sh`'s header does not work on 6.12.**
  Raw `read()`/`write()` on `/dev/iio:*` does not program the DMA on this
  kernel; the advice predates that. An undrained chain backpressures the
  packetizer and every probe word reads zero -- indistinguishable from "no
  signal", which is exactly the trap the header warns about. Draining with
  `iio_readdev` instead still produced zeros, so the stall is not the cause.
- **The probe peripherals are present in the golden bitstream.** Writing
  `0xA5A50000` to `0x43C20000`, `0x43C30000` and `0x43C40000` reads back
  unchanged, so an all-zero capture is not a missing peripheral. (They were
  cleared to 0 afterwards.)

So the demodulator's IQ input shows no activity while the transmitter is
demonstrably feeding. That narrows the fault to the receive datapath between
the ADC interface and the demodulator input, or to the probe's arming sequence
-- and those two must be separated before anything else is attempted.

## RESOLVED: the chain works; the loopback signal is 21 dB too weak

Probing with the receive chain properly drained changes the conclusion above.
All three probes capture real data:

| probe | status | non-zero | sample |
|---|---|---|---|
| TX IQ `0x43C20000` | `0x8000` done | 32/32 | `0x1226FBF6` |
| RX IQ `0x43C30000` | `0x8000` done | 32/32 | `0xFEE80120` |
| DAC pin `0x43C40000` | `0x1000` | 32/32 | `0xED720048` |

**The receive datapath is fine and the demodulator is being fed IQ.** The
earlier all-zero readings were caused by a stale `dd` of mine (pid 3375) still
holding `/dev/iio:device3`: raw `read()` never programs the DMA on 6.12, so it
blocked forever AND held the device single-open, making every later
`iio_readdev` drain fail with `EBUSY (16)`. With no drain the DMA backpressures
the packetizer and the demodulator input stalls -- the RX probe read
`running=1, waddr=0`, which is "armed, saw no beats", not "no signal". Kill any
`dd` on an IIO character device before concluding anything about the receive
path.

Reading the probe's STATUS word is what separated those cases. Earlier analysis
filtered for lines starting with `0x` and discarded the `# status=` line, which
is the only thing that distinguishes a capture that never armed from one that
armed and saw nothing.

**Measured demodulator input level**, 512 samples, transmit feed in flight,
RX gain 73 dB, RSSI 110.5 dB:

    rms 395, peak 1200
    known-good reference (ADC fmt 0x51): rms 4465, peak 7648
    shortfall: 11.3x amplitude = 21.1 dB   (peak 6.4x = 16.1 dB)

So the single-board loopback delivers a signal **~21 dB below** the level at
which this demodulator is characterised to work. That is why the baseline does
not reproduce, and it is consistent with everything in the record: RSSI
110.5 dB sits inside the 111-117 dB the original run reported, and with roughly
zero margin, 10 dB or 20 dB of transmit attenuation killing it outright is
expected rather than surprising. The original run succeeded marginally at about
this level; small physical variations decide it either way.

Expressed as signal-to-noise, which is the number that decides whether a
demodulator can lock, it is starker still. With no transmitter the demodulator
input reads **rms 242** -- that is the noise floor. The loopback reads rms 395,
so the signal alone is about 312 and the **loopback SNR is roughly 2 dB**. The
known-good level of 4465 corresponds to about **25 dB**. No QPSK demodulator
locks at 2 dB, so the loopback was never close, and no amount of software
would have changed that.

**This is not a software problem and no software change should be made for it.**
Reaching a usable baseline needs ~21 dB more signal at the demodulator: two
boards with a real coax path and controlled attenuation, which is the Phase 8
arrangement anyway. As commit 7f77e83 put it, loopback proves the path exists
and cannot characterise it.

## What is not eliminated

The receive lock itself. The transmitter emits and the receiver captures, but
~7 dB above the noise floor is far too weak to demodulate -- which is precisely
the marginal coupling the original commit described.

**Digital loopback appears to do nothing here.** In digital mode RSSI tracked
the RF coupling identically to RF mode (107.5 dB in both). Were the AD9363's
internal TX->RX loopback engaged, the demodulator would see a strong signal
independent of the RF path and RSSI would not track it. The debugfs attribute
reads back `1`, so it is being accepted and apparently not acting. Establishing
whether that attribute is functional on this build is the next thing to settle;
until it is, "digital loopback" cannot be assumed to be a safe substitute for
real coupling.

Captures kept: `base_rf2.bin` (4,044,128 B), `base_dig2.bin` (4,016,742 B).
Neither is a usable Phase 9B input -- they contain no frames.
