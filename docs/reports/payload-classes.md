# Data, video and telemetry over the RF link

Measured at 15.36 MS/s, 434 MHz, differential QPSK, RSSI ~69 dB, both boards on
the golden image. No Ethernet involved -- this is the modem layer.

## Bulk data / video: byte-perfect

A 4 MiB payload with H.264-like structure (NAL start codes, mixed I/P slice
sizes, high-entropy slice data) framed into 3496 chunks and transmitted:

    chunks: 3496 of 3496 recovered (100.00%), 0 missing
    CRC failures 0, out-of-range seq 0
    byte-exact: 3496 of 3496 recovered chunks match the source (0 differ)
    video bytes delivered: 4194304 of 4194304 (100.00%)

**Every byte arrived and every byte matched.** For a file transfer or a video
stream, the link is lossless at this signal level.

Framing overhead on 1200-byte payloads is **5.51%** (4,194,304 B of source
becomes 4,439,024 B on the wire).

### One figure to be careful with

The reassembly tool printed "GOODPUT 10.74 Mbit/s" for this run. That is not the
link's throughput. The stream was transmitted twelve times cyclically -- the
same run reports 14,647 duplicates against 3496 unique chunks -- so the figure
divides unique bytes by content duration rather than by wall clock. **The
measured goodput is 6.96 Mbit/s**, from the controlled runs in
`throughput-latency-targets.md`. Quoting 10.74 would overstate the link by 54%.

## Telemetry: the link carries it easily, but efficiency collapses

Telemetry is the opposite shape -- small, frequent messages -- and the fixed
70-byte frame overhead dominates:

| payload | wire | efficiency | frames/s available |
|---|---|---|---|
| 20 B | 90 B | **22.2%** | 10,639 |
| 50 B | 120 B | 41.7% | 7,979 |
| 100 B | 170 B | 58.8% | 5,632 |
| 200 B | 270 B | 74.1% | 3,546 |
| 500 B | 570 B | 87.7% | 1,680 |
| 1200 B | 1270 B | 94.5% | 754 |

Capacity is not the constraint: even at 22% efficiency the link offers over
10,000 small frames per second, against a typical telemetry need of tens per
second. A 10 kbit/s telemetry stream needs 62.5 messages/s at 20 B, which is
0.6% of what is available.

**So telemetry and video can share this link comfortably** -- the concern is
latency and scheduling, not bandwidth.

## Latency is the same for all three classes

The deframer cannot run until a whole DMA packet has arrived, so **frame size
does not affect latency at all**. A 20-byte telemetry message waits exactly as
long as a 1200-byte video chunk:

| rate | PKT_BYTES | latency floor |
|---|---|---|
| 7.68 MS/s | 32768 | 68.3 ms |
| 7.68 MS/s | 8192 | 17.1 ms |
| 15.36 MS/s | 32768 | 34.1 ms |
| **15.36 MS/s** | **8192** | **8.5 ms** |

This is the dominant issue for telemetry and for any control traffic. A 20-byte
command occupies 94 microseconds of air and then waits up to 34 ms for the
packet to fill. **Only `PKT_BYTES` changes that**, and at 15.36 MS/s a value of
8192 brings it inside a 10 ms budget.

## What has not been tested

- **Mixed traffic.** Video and telemetry were measured separately. Whether a
  large video frame delays a small telemetry frame behind it -- head-of-line
  blocking -- is a scheduling question this link has no answer for yet, and it
  is the one that decides whether a single link can serve both.
- **End-to-end latency**, as distinct from the packetizer floor above. That
  needs the isolated Ethernet topology.
