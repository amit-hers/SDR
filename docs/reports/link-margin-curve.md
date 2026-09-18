# Link margin and directional characterisation

Two units, antennas, 434 MHz, 7.68 MS/s, differential QPSK, 1200-byte payloads.
Receiver brought up fresh for every point, so each includes the demodulator
soft_reset. `NO SIGNAL` means `capturable = 0` -- **not** a PER of 0.00%.

## PER against transmit power

| TX attenuation | A -> B RSSI / PER | B -> A RSSI / PER |
|---|---|---|
| 0 dB | 83.25 dB, **1.44%** (52/3600) | 82.75 dB, **0.03%** (1/3573) |
| -10 dB | 91.75 dB, **2.11%** (76/3600) | 92.25 dB, **0.11%** (4/3598) |
| -20 dB | 101.00 dB, NO SIGNAL | 100.75 dB, NO SIGNAL |
| -30 dB | 109.50 dB, NO SIGNAL | 107.00 dB, NO SIGNAL |

**The cliff is between -10 and -20 dB**, i.e. between roughly RSSI 92 dB and
101 dB. There is no graceful degradation across that step: the link goes from
single-digit-percent PER to no frames at all. Useful margin above the cliff is
therefore only about **10 dB** in this bench arrangement, which is not much for
anything mobile.

RSSI tracks attenuation almost exactly (8.5 dB of RSSI per 10 dB of
attenuation), so the RF chain is linear across the range and the cliff is a
demodulator threshold, not a front-end effect.

## Repeatability of the asymmetry

Three consecutive full runs at 0 dB, 7.68 MS/s:

| run | A -> B PER | CRC | B -> A PER | CRC |
|---|---|---|---|---|
| 1 | 1.60% | 57 | **0.00%** | 0 |
| 2 | 1.44% | 52 | **0.00%** | 0 |
| 3 | 1.56% | 56 | **0.00%** | 0 |

Payload mismatches were 0 in all six directional runs, and the DMA gap stayed at
0 B median (max 1 B) throughout. B -> A decoded roughly **10,745 consecutive
frames with zero CRC failures**.

The asymmetry is therefore a stable property of this pair, not run-to-run noise:
A -> B sits at 1.4-2.1% while B -> A is at or near zero, and the gap persists at
both power levels and both sample rates.

## What it is not

Measured at each demodulator input while the far end transmitted:

| direction | rms | abs(DC) | IQ imbalance | correlation vs reference |
|---|---|---|---|---|
| A -> B | 3698 | 485 | 1.066 | 0.9714 |
| B -> A | 4963 | 481 | 0.940 | 0.9930 |

A -> B is ~2.6 dB weaker **and** measurably more distorted. DC offset is equal
and IQ imbalance is within ~7% of unity both ways, so neither explains it.

**Nothing here identifies a defective board.** The signature is equally
consistent with an antenna or placement asymmetry. The cheap discriminator,
which has not yet been run: **swap the antennas between the two units and
repeat**. If the weak direction follows the antenna, it is the antenna; if it
stays with the board, it is the board. Do that before treating these as per-unit
figures.

## Product implications

- Quoting this link as "PER 0.00%" would describe one direction and misdescribe
  the other. Both directions must always be reported.
- ~10 dB of margin is the number to design against until antennas and placement
  are settled, and it is the starting point for Phase 12 adaptive modulation:
  the fallback must engage well before the cliff, because there is no gentle
  slope to detect on the way down.
