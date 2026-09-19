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

That asymmetry is the thread to pull next: capture UNIT-B's transmitted IQ with
the TX probe while its bridge drives it, and compare against the same probe
while `tx_feed.sh` drives it. If the waveforms differ, the fault is in what the
bridge hands the modulator; if they match, it is in UNIT-A's receive path at
444 MHz.

## Configuration changes made

`rx_framed.sh` now takes the RX LO as a parameter -- it was hardcoded to
434 MHz, so frequency division could not be expressed at all.
`appliance_start.sh` passes `RX_FREQUENCY`, warns when TX and RX are equal, and
`bridge.conf.example` documents that the two units' frequencies must be crossed.
