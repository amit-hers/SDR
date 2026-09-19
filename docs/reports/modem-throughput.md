# Sustained modem goodput

Measured over a single 30.02 s window, UNIT-A transmitting into UNIT-B, no
Ethernet involved. 7.68 MS/s, 434 MHz, differential QPSK, 1200-byte payloads,
transmitter at 0 dB.

| quantity | value |
|---|---|
| window | 30.02 s (wall clock, read from `/proc/uptime` either side) |
| demodulated byte rate | **3.829 Mbit/s** (14,367,114 B) |
| frames decoded | 10,867 of 10,870 capturable |
| PER | **0.03%** (3 missing, 2 CRC failures) |
| payload mismatches | **0** |
| **payload goodput** | **3.475 Mbit/s** |
| framing efficiency | 90.8% of the demodulated stream is payload |
| capture duty cycle | 100.0% |

## The raw rate is exactly the theoretical one

At 7.68 MS/s with 4 samples per symbol, QPSK carries
`1.92 Msym/s x 2 bits = 3.84 Mbit/s`. The measured demodulated byte rate is
**3.829 Mbit/s**, which is 99.7% of that. The modem is running at its nominal
rate with no hidden stall, and the DMA is keeping up completely -- 100% of the
demodulated stream reached userspace and the gap between consecutive packets was
0 B.

## Where the remaining 9.2% goes

Framing, not loss. Each 1200-byte payload occupies 1270 bytes on the wire
(header, CRC, and the sync word), which alone accounts for 94.5%. The rest is
the frames that straddle a capture boundary at the start and end of the window.
PER is 0.03%, so essentially nothing is being lost to the channel.

## What this is and is not

**It is** the modem layer's sustained capability at this sample rate: about
3.48 Mbit/s of payload, error-free within measurement, over 10,867 consecutive
frames.

**It is not** an Ethernet-over-RF throughput figure. No bridge, no RJ45 and no
host traffic were involved, so it sets an upper bound for Phase 10 rather than
answering it. The Ethernet path adds per-frame capture, the 1400-byte payload
limit, and whatever the two bridges cost.

**Note the appliance runs at 3.84 MS/s**, half this rate, so its ceiling is
about 1.74 Mbit/s of payload. Choosing the product rate should follow the
Phase 8 Ethernet measurement rather than this one.

## Measurement caution

The RSSI printed at the end of this run was 114.50 dB, which is the noise floor:
it was sampled after the transmitter had stopped. The link during the capture
was at 65-68 dB, per the bidirectional test run immediately before. **Sample
RSSI during the capture, never after** -- a post-run reading reports silence as
though it were the test condition, which has already produced one misleading
result in this project.
