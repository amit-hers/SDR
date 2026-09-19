# The demodulator has more than one stall mode, and recovery only detects one

## Proven: the signal reaching the demodulator is correct

UNIT-A's RX IQ probe read **while its own bridge was running and draining**,
with the peer transmitting and RSSI verified stable at 65.50 dB before and
after the capture:

    rms 4813, peak 7496, crest 1.56        (known-good: 4465 / 7648 / 1.71)
    correlation vs reference: 0.9969 at CFO +0 kHz

At that moment the bridge reported `rx 2105 dma, 0 frames, crcerr 0`.

**A near-perfect signal goes in and nothing comes out.** Zero CRC failures
alongside zero frames is the important detail: the deframer is not rejecting
corrupted frames, it is not finding a sync word at all. Everything upstream --
RF path, AD9363, sample rate, LO, ADC format, signal level -- is eliminated by
that one measurement.

## Stall mode A: cold, at bring-up

The appliance resets the demodulator during bring-up, which happens before the
peer is transmitting, and the core can come up stalled on an idle channel.
Restarting the appliance once the peer was transmitting immediately delivered
48,000 bytes.

**Recovery could not fire here**, because `recovery_armed` started `false` and
is only set when a frame decodes. A demodulator that never decodes a first
frame never arms the monitor, so a cold stall persists for ever. Fixed: the
monitor is now armed from the start. Firing still needs DMA advancing and
`mu_clamped` climbing for two intervals, so an absent signal does not cause a
reset loop.

## Stall mode B: after decoding, with mu_clamped STATIC

A second stall was then observed which the detector still cannot see. The
bridge decoded 40 frames / 48,000 B and stopped, while `rx dma` kept advancing
1524 -> 1720. Sampling the counters over 20 s:

| lock_count | mu_clamped | rssi |
|---|---|---|
| 0x03EA9EE1 | 0x0010DC46 | 116.00 dB |
| 0x0409B2D4 | 0x0010DC46 | 116.50 dB |
| 0x042A9955 | 0x0010DC47 | 65.50 dB |
| 0x044B7FFD | 0x0010DC47 | 65.50 dB |
| 0x046C66CD | 0x0010DC47 | 65.75 dB |

`lock_count` advances steadily. **`mu_clamped` moves by 1 in 20 seconds.**

The recovery trigger requires `mu_delta >= 32` per reporting interval, so this
stall is invisible to it by construction. The trigger was derived from a stall
where "mu_clamped rapidly increased", which is a real mode -- but not the only
one.

**A detector keyed on the symptom of one stall mode will miss the others.** The
robust signal is the one that defines the fault in the first place: RX DMA
advancing while frames stay at zero, for long enough that it cannot be a gap in
traffic. `mu_clamped` is corroborating evidence, not the trigger.

## Test-harness caveat in the above

RSSI alternates between 116 dB and 65 dB in that table because the transmit
feed loops a file and there is a gap at each restart. That is a property of the
test rig, not the link, but it means "frames stopped" and "the peer stopped
transmitting" overlap in these runs and must be separated before this stall is
characterised further. A feed that never gaps is needed.


## The detector, corrected twice

**First correction -- trigger on the defining fault.** The trigger now fires on
RX DMA advancing while the deframer is inactive, with `mu_clamped` logged as
corroboration rather than demanded. On hardware it immediately caught the mode
the old trigger could not:

    RECOVERY ... (dma +147, frames +0, mu_clamped +0,      lock=23999595)
    RECOVERY ... (dma +131, frames +0, mu_clamped +0,      lock=9257078)
    RECOVERY ... (dma +293, frames +0, mu_clamped +562632, lock=28801962)

Two with `mu_clamped +0` -- invisible to the previous rule by construction.

**Second correction -- those three were FALSE POSITIVES.** The counters in the
same run showed `dup` climbing 50693 -> 57995: the demodulator was decoding
around 58,000 frames and suppressing them as duplicates, because the test feed
loops the same 40 frames. `frames` stayed at 40 because only 40 unique frames
exist. A perfectly healthy demodulator was reset three times.

Liveness is therefore **any deframer activity** -- unique frames plus duplicates
plus CRC failures. A duplicate proves the demodulator decoded a frame and
checked its CRC; so does a CRC failure. Counting only unique deliveries declares
a healthy link stalled whenever traffic repeats, and **real traffic repeats**:
ARP, keepalives, retransmissions, video I-frames.

After the fix, the same conditions give **one** recovery (at startup, before
lock) and none thereafter, while `dup` climbs 34556 -> 41838 across an interval
-- alive, and correctly left alone.

### The general lesson

Both mistakes were the same shape: a detector keyed on a *proxy* for the fault
rather than the fault. `mu_clamped` rising was one stall's symptom, not the
stall. Unique frames delivered was one traffic pattern's liveness, not liveness.
