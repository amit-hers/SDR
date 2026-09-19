# The appliance jams itself: same-frequency full duplex

**This is why the appliance has never carried Ethernet traffic**, and it is
independent of every throughput, latency and loop finding so far.

## What was measured

Both units in appliance mode at 15.36 MS/s on one switch, 20 broadcast frames
injected:

    UNIT-A: tx 20 pkts | rx 876 dma, 332846 frames, 0 B (crcerr 45)  | loopsup 0
    UNIT-B: tx 20 pkts | rx 876 dma, 332487 frames, 0 B (crcerr 21)  | loopsup 0

Each forwarded its 20 frames to the radio, and **zero bytes were delivered** at
either end despite hundreds of thousands of "frames" being decoded.

Stopping UNIT-A's bridge entirely, so only UNIT-B transmits:

    UNIT-B alone: rx 876 dma, 207991 frames, 0 B, crcerr 101622
                  offsets 207991/0/0/0

208,000 false frames and 101,000 CRC failures with **no other transmitter on the
air**. UNIT-B is decoding its own signal.

## Cause

Both units run `tx_lo = rx_lo = 433999998` in `fdd` mode -- they transmit and
receive on the same frequency at the same time. The transmitter is physically
adjacent to the receiver, so each unit's own signal arrives far stronger than
the peer's, and the demodulator locks onto it.

The appliance also transmits **continuously**: `txLoop` sends idle fill when
there is no traffic, deliberately, because "the demodulator's timing loop has
nothing to track through a silent carrier". `idle 347855` in the counters above
is that fill. So both radios are keying up permanently.

## Why earlier tests passed

Every successful link measurement in this project had **one transmitter**. The
modem link test runs one direction at a time: it starts `tx_feed` on one unit
and captures on the other, and the receiving unit's bridge is not running. That
is a half-duplex measurement, and under it the link is excellent -- 6.96 Mbit/s
at 0.00% PER.

Those results are still valid. They measure one direction with a quiet receiver,
which is a real operating mode. They do not measure two appliances talking at
once, and nothing in this project had done so until now.

## What it means

The link is **half duplex on a single frequency**, and the appliance is written
as though it were full duplex. Three ways out, in rough order of effort:

1. **Two frequencies (FDD).** A transmits on f1 and receives on f2, B the
   reverse. The AD9363 has independent TX and RX synthesisers, so this needs
   configuration rather than new hardware, and it costs spectrum.
2. **Time division (TDD).** One carrier, with the two ends alternating under a
   schedule. No extra spectrum, but it needs a protocol the modem does not have
   and it caps each direction at under half the link.
3. **Transmit only when there is traffic.** Removes the permanent jam but not
   collisions when both ends have data, and it forfeits the timing-lock reason
   the idle fill exists.

## Consequence for the loop-suppression work

The loop-guard acceptance test is **inconclusive, not passed**. No storm
occurred, but `loopsup 0` and zero bytes delivered show the reason was that no
frame ever crossed -- not that the guard suppressed anything. The guard's logic
is verified by unit test across broadcast, multicast, ARP, IPv6 ND, unknown
unicast, MAC movement, malformed sources and table flooding, and it is built
into the bridge. **It cannot be validated on hardware until two appliances can
exchange a frame at all.**
