# Antenna RF investigation on UNIT-A

Antennas were fitted to TX and RX at 434 MHz. This records what changed, what
was found, and what is still unexplained.

## Setup as found

**Only UNIT-A is connected.** USB enumerates one radio, serial
`AKRLJ24FWXM7H65X`. UNIT-B (`3SXRLJMXS7EL5IBJ`) is absent, so no two-unit test
was possible and everything below is single-board self-loopback.

**`192.168.2.1` answers, and it is NOT UNIT-B.** `ip route get` shows it
resolving via `wlp2s0` to the internet gateway; it pings but refuses SSH. A ping
to a board address proves nothing unless the route is checked -- the same trap
that produced a false "board is up" earlier in this project.

## Antennas transformed the RF side

| | bare connectors | with antennas |
|---|---|---|
| noise floor (no TX) | rms 242 | rms 569 |
| demodulator input, TX on | rms 395 | **rms 3472**, peak 6728 |
| RSSI idle -> transmitting | 114.5 -> 109 dB | 108.5 -> **66.5 dB** |
| SNR | 2.2 dB | **15.6 dB** |

A 41 dB RSSI change and a 13.4 dB SNR improvement. The higher noise floor is
expected: an antenna collects ambient 434 MHz energy a bare connector does not.
The signal now sits close to the known-good working level of rms 4465, so the
**level problem identified in Phase 9A is solved**.

## The transmit chain is healthy on UNIT-A

All three probes capture real data with a feed in flight:

    TX IQ   0x43C20000  status 0x8000 done  32/32 non-zero  0x042E156C
    RX IQ   0x43C30000  status 0x8000 done  32/32 non-zero  0x03E810C0
    DAC pin 0x43C40000                      32/32 non-zero  0x0E00F02E

This **contradicts the note in `7f77e83`** that "UNIT-A's fabric modulator has
never produced output". That was recorded before UNIT-A carried this bitstream;
both units now report identical FPGA identity (`0x5344524C`, `0x00010300`) and
UNIT-A's modulator demonstrably modulates.

The AGC and RF chain are also well behaved. Sweeping transmit attenuation
0 -> -40 dB moves RSSI 67 -> 102 dB linearly while RX gain tracks 46 -> 73 dB.
Nothing is saturating or overloading.

## DEFECT: rx_framed.sh hardcoded diff_mode

`tx_fabric.sh` takes `diff_mode` as its third argument and writes it to the
modulator (`0x43C10020`). `rx_framed.sh` wrote a **literal 1** to the
demodulator (`0x43C00028`). So `tx_fabric.sh <fs> <lo> 0` produced
**mod=0 / dem=1**, a mismatch the register map documents as undecodable, while
every register still reads healthy and the signal level looks correct. Observed
directly: `mod diff_mode = 0x00000000`, `dem diff_mode = 0x00000001`.

Fixed: `rx_framed.sh` now takes `diff_mode` as its second argument.

Note also that the diff registers are at **mod `0x20` / dem `0x28`**, and
`dem 0x18` is `lock_count` (RO). Reading offset `0x00` -- which returns `0x81`
on both -- says nothing about diff mode, and misled this investigation for
several rounds.

## Still unexplained: nothing decodes

With good signal and diff matched on both ends, **no frame sync word appears at
any of the four symbol phases**, and a trivial counter pattern (0..255
repeating) does not round-trip either.

What has been ruled out:

- **Signal level.** rms 3472 against a known-good 4465, SNR 15.6 dB.
- **Overload.** Swept across 40 dB; zero sync at every level.
- **Constellation rotation, symbol ordering, byte offset.** All 32 combinations
  of rotation (0-3), symbol-order reversal, and bit offset (0/2/4/6) searched.
  No hits.
- **Framing/scrambling complexity.** A raw counter fails too.
- **Silence in the capture.** With the feed outlasting the capture the symbol
  distribution is 26.2/23.2/24.4/26.1% -- balanced and healthy, matching the
  reference's 24.7/23.7/27.6/24.0%.

Balanced symbol statistics with no correlation to the transmitted data is what
decoding **noise** looks like. The demodulator is producing well-distributed
decisions that bear no relation to what was sent.

Two measurement faults of mine were also found and must not be repeated: a
4 MB pattern feeds in ~8 s, so a capture started 4 s in reads mostly silence
(that produced a misleading 40% symbol-0 excess); and in the final run the feed
terminated early (`feed=0` on a 25 MB file), so RSSI must be sampled DURING the
capture, not after.

## Next

The signal is now adequate, so the remaining fault is in the modulator ->
demodulator mapping, not the RF path. The highest-value next step is a
**two-unit** test, which removes same-board TX/RX interaction entirely -- and
that needs UNIT-B reconnected.
