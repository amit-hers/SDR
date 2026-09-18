# Two-unit modem test: UNIT-A <-> UNIT-B over the air

First test with both boards on the golden image, antennas fitted, transmitting
into each other. No Ethernet, no bridge, no RJ45.

**Result: FAIL in both directions.** No frame sync word at any symbol phase,
no frame recovery, in either direction, at adequate signal level.

## Positive identification

Serials are read from the HOST's USB enumeration. Asking a board reads its own
(empty) USB device tree -- it is a gadget, not a host -- which silently
identifies every board as `""`.

| | UNIT-A | UNIT-B |
|---|---|---|
| USB port / serial | 1-7 / `AKRLJ24FWXM7H65X` | 1-6 / `3SXRLJMXS7EL5IBJ` |
| kernel | `6.12.0-g39414bc4038b-dirty` | same |
| FPGA magic / ABI / reg map | `0x5344524C` / 3 / 3 | same |
| bridge md5 | `58fc077c...` | same |

## Settings verified identical, read back from both boards

`fs_rx`, `fs_tx` 7,680,000; `lo_tx`, `lo_rx` 433,999,998; `bw_rx` 4,000,000;
`mod_diff` 1; `dem_diff` 1; `adc_fmt0/1` `0x51`; `mod_bpsk` 0. The transmitter's
`mod_diff` was also checked against the receiver's `dem_diff` specifically,
since that pairing is what carries the link and a mismatch there decodes nothing
while every register reads healthy.

## Signal level is adequate

| transmit attenuation | RSSI at the far end |
|---|---|
| -20 dB | 101.0 dB (A->B), 102.25 dB (B->A) |
| **0 dB** | **84.0 dB both directions** |

Idle noise floor with antennas is ~108 dB, so at full power the link runs about
**24 dB above noise** -- the range in which this demodulator is characterised to
work (~25 dB). The two directions agree to within 0.25 dB, so the path is
reciprocal and nothing is one-sided.

## What this eliminates

- **Same-board TX/RX interaction.** Two physically separate boards fail exactly
  as the single-board loopback did. This was the main reason to build the
  two-unit test, and it is now ruled out.
- **Signal level.** 24 dB SNR, symmetric.
- **Configuration mismatch.** Every relevant register read back and compared.
- **Direction.** A->B and B->A behave identically; symmetry was not assumed.

## The fabric modulator cannot be bypassed on this bitstream

An attempt to isolate the modulator by replaying host-modulated IQ -- the
configuration in which the demodulator was originally proven -- **does not work
here**. With `mod_en=0` the far end's RSSI falls to 113.5 dB, the noise floor:
the board stops transmitting altogether. The transmit chain is
DMA -> `qpsk_mod` -> `axis_to_iq` -> DAC, so disabling the modulator breaks the
path rather than passing IQ through it.

This matters for diagnosis. The historical evidence that the demodulator works
came from a stock PL replaying host-modulated IQ, and that configuration is not
available on the golden bitstream, so the modulator and demodulator cannot be
separated by this method. Any future attempt to isolate them needs a different
approach.

## Where the fault now sits

Two independent boards, correct and verified-identical settings, adequate and
reciprocal signal, and no frame recovery in either direction. The fault is in
the modulator -> demodulator chain itself, not in RF level, not in
configuration, and not in same-board coupling.

Note that the 7.85 Mbit/s and PER 0.00% figures in the project's history
characterise the DEMODULATOR against a known-good host transmitter (see
`7f77e83`). The fabric MODULATOR has been shown to decode into a real
demodulator exactly once, marginally, at PER 1.53%. It is the least-proven
element in this chain and the first place to look.
