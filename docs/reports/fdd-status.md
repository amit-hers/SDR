# Frequency-division duplex: works for a raw stream, not yet for the bridge

## Confirmed: FDD removes the self-jamming

Single frequency, both units keyed (idle fill is continuous): **zero bytes
delivered in either direction**, and a unit alone on the air decoded 207,991
false frames with 101,622 CRC failures -- demodulating itself.

Crossed frequencies (A tx 434 / rx 444, B tx 444 / rx 434), **both units
transmitting simultaneously**, fed a raw byte stream with `tx_feed.sh`:

| direction | decoded | PER | CRC | payload mismatches |
|---|---|---|---|---|
| A -> B | 8661 / 8661 | **0.00%** | 0 | 0 |
| B -> A | 8333 / 8338 | **0.06%** | 1 | 0 |

So the duplex collision is solved at the modem layer, and the fix is
configuration rather than hardware.

## Open: the appliance bridges do not reproduce it

With the same FDD configuration but the transmitters driven by `sdr_bridge`
instead of `tx_feed.sh`:

    UNIT-A: rx 1519 dma,       0 frames, 0 B (crcerr 0)   <- decodes nothing
    UNIT-B: rx 3513 dma, 1336779 frames, 0 B (crcerr 82)  <- decodes A's keepalives

UNIT-B decodes UNIT-A's transmission (the 1.3M zero-payload frames are the
bridge's idle fill). UNIT-A decodes nothing from UNIT-B.

### What has been ruled out

- **Signal level.** A hears B at 66.00 dB. Silencing B alone takes A to
  117.75 dB, and silencing A as well gives 116.25 dB, so the 66 dB is B's
  signal and not A's own transmitter leaking 10 MHz across. 51 dB above noise.
- **Demodulator stall.** A soft reset was pulsed with B already transmitting;
  lock_count and mu_clamped both reset cleanly and A still decoded nothing over
  the following 20 s.
- **Frequency.** 444 MHz decodes fine in the `tx_feed.sh` test -- that run is
  B transmitting on exactly this frequency and A recovering 8333 frames.
- **Configuration.** Both units read back the intended crossed LOs, matched
  sample rate, matched diff_mode, ADC format 0x51.

### What distinguishes the two cases

The only difference between the working and failing runs is **what drives the
transmitter**: a continuous raw byte stream from `tx_feed.sh` versus framed
Ethernet plus idle fill from `sdr_bridge`. Both units run the same binary, so
whatever this is, it is not symmetric between them -- which is the part that
does not yet make sense and should not be explained away.

### That thread was pulled: the transmitter is not the difference

UNIT-B's TX IQ probe (`0x43C20000`, the modulator's own output) captured under
each driver, 512 samples:

| driver | rms | peak | crest | distinct I levels |
|---|---|---|---|---|
| `sdr_bridge` | 5398 | 5894 | 1.09 | 306 |
| `tx_feed.sh` | 5403 | 5944 | 1.10 | 439 |

Statistically the same waveform. **The bridge does not hand the modulator
something different**, so that hypothesis is closed and the fault is on the
receive side.

By elimination the remaining candidate is UNIT-A's receive path *as driven by
the bridge*, as distinct from the same RF captured raw. The same signal recovers
8333 frames when captured with `iio_readdev` and analysed offline, and zero when
the bridge deframes it live. UNIT-B's bridge deframes its peer without trouble,
so this is not simply "the bridge cannot deframe".

Testing that cleanly is awkward: the RX character device is single-open, so
capturing raw on UNIT-A requires stopping its bridge, which removes the very
condition under test. A comparison that does not disturb it would use the RX IQ
probe on UNIT-A while its bridge runs, and check whether the demodulator input
is the same as in the working case.

### Unrelated observation, recorded so it is not lost

Crest factor at the modulator output is **1.09-1.10 under both drivers**, where
the known-good figure in this project is rms 4465 / peak 7648 = **1.71**.
Because both drivers agree it cannot explain the asymmetry, but it does not look
like filtered QPSK and is worth understanding on its own.

## Configuration changes made

`rx_framed.sh` now takes the RX LO as a parameter -- it was hardcoded to
434 MHz, so frequency division could not be expressed at all.
`appliance_start.sh` passes `RX_FREQUENCY`, warns when TX and RX are equal, and
`bridge.conf.example` documents that the two units' frequencies must be crossed.
