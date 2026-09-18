# Open issues

Everything currently open, with the evidence behind it. Anything stated as
measured was measured; anything unproven says so.

---

## 1. Blocked on physical setup (not software)

### 1.1 Phase 8 cannot start: the two RJ45 ports are cabled together

UNIT-A emitted 12 ARP broadcasts and UNIT-B received exactly 12 **while
UNIT-B's demodulator was disabled and no bridge was running**, so they crossed
copper. Re-confirmed several times since; most recently a delta of 6 frames.

On this topology every Ethernet test in Phase 8 passes with both radios switched
off, so a PASS would certify copper. `tests/phase8/acceptance.py` refuses to run
until this is removed, and that gate is self-tested
(`tests/phase8/selftest.sh`).

It is also an active hazard: with both bridges running, A transmits over RF, B
injects onto its wire, and copper carries the frame back to A, which transmits
it again. `PACKET_IGNORE_OUTGOING` does not prevent this -- it suppresses a
bridge's own emissions, not a peer's arriving over copper. Both appliances now
autostart, so the loop is armed the moment any host sends a frame.

**Needed:** remove the cable.

### 1.2 No host endpoints

Phase 8 needs an independent Ethernet endpoint per unit. The development machine
has only `enp3s0` (its internet uplink) and `wlp2s0`. Traffic generated *on* a
board cannot substitute: the bridge deliberately ignores frames the board itself
emits, and 400 pings out of UNIT-A's own `eth0` produced `tx 0 pkts`.

**Needed:** two machines, or two USB-Ethernet adapters.

### 1.3 The antenna-swap test has not been run

The A -> B direction is consistently worse than B -> A (see 3.1). Whether that
belongs to a board or to an antenna is unresolved and is a two-minute physical
test: swap the antennas between the units and repeat. If the weak direction
follows the antenna, it is the antenna.

Until this is done, the per-direction figures must not be quoted as per-unit
hardware specifications.

---

## 2. Defects found and fixed (verify after any regression)

| | fix | commit |
|---|---|---|
| Demodulator comes up stalled; no frames decode at any signal level | drained `soft_reset` in `rx_framed.sh` | `6b3b60e` |
| `rx_framed.sh` hardcoded `diff_mode=1` while `tx_fabric.sh` parameterised it | `diff_mode` is now an argument | `bd2b119` |
| AF_PACKET bridge re-captured its own injections (unconditional loop) | `PACKET_IGNORE_OUTGOING` | `7d1fd8b` |
| MTU silently never set; banner printed the requested value | set with link down, report `/sys` | `7d1fd8b` |
| Static 542 KB binary silently truncated on jffs2 | dynamic glibc, 120 KB | `7d1fd8b` |
| Appliance used ramdisk `TOOLS` path; no single-instance guard | persistent path, `/proc/PID/exe` guard | `7d1fd8b` |
| Appliance raced IIO probing at boot | wait by device name | `2e7ccb8` |

---

## 3. Open, not yet explained

### 3.1 The link is asymmetric and it is repeatable

Three consecutive runs at 7.68 MS/s, 0 dB: A -> B 1.60 / 1.44 / 1.56%,
B -> A 0.00 / 0.00 / 0.00%. B -> A decoded ~10,745 consecutive frames with zero
CRC failures. Payload mismatches 0 in all six runs.

At the demodulator inputs, A -> B arrives 2.6 dB weaker (rms 3698 vs 4963) and
correlates 0.9714 against the reference where B -> A reaches 0.9930. DC offset
and IQ imbalance are equivalent in both directions, so neither explains it.

Unresolved: board or antenna. See 1.3.

### 3.2 Only ~10 dB of link margin, with a hard cliff

PER against transmit attenuation: 0 dB -> 1.44% / 0.03%; -10 dB -> 2.11% /
0.11%; **-20 dB -> no frames at all in either direction**. The transition is
abrupt -- there is no graceful degradation to detect on the way down.

RSSI tracks attenuation at 8.5 dB per 10 dB, so the RF chain is linear and the
cliff is a demodulator threshold rather than a front-end effect.

Consequence for Phase 12: an adaptive scheme cannot wait for rising PER, because
between "working" and "nothing" there is roughly one 10 dB step.

### 3.3 The appliance runs at a rate where the weak direction is much worse

`bridge.conf` sets 3.84 MS/s. Measured A -> B PER is 4.71% there against 0.67%
at 7.68 MS/s, while B -> A is 0.00% at both. Degrading at the LOWER sample rate
is the opposite of the usual expectation and is unexplained. Decide the product
rate only after it is understood.

### 3.4 `tun drops tx 4` -- RESOLVED, was a reporting bug

Not loss. The field mirrored the kernel's own
`/sys/class/net/<iface>/statistics/tx_dropped`, which is cumulative since boot
and counts drops from any source, so the board's pre-bridge history was being
attributed to the bridge. Both units show it: 4 and 3 dropped against 8 and 3
transmitted, all of it the local stack emitting before the interface was ready,
and static ever since.

Now baselined at bridge start and reported as a delta, and labelled with the
actual interface rather than "tun" -- which was wrong in raw-eth mode and sent
attention to the wrong device. Verified: kernel still reads 4 and 3, the bridge
reports `eth0 drops tx 0 rx 0 (since start)` on both.

### 3.5 UNIT-B's flash is prefix-identical, not identical

UNIT-B's mtd3 matches the golden image over the first 11,907,300 bytes; the
final 33 bytes read `0xFF` where the FDT has `0x00` padding, from a truncated
first write. It boots and runs correctly, which is what shows those bytes are
inert. An attempt to correct them with a direct `dd` to `/dev/mtd3` was refused
by the permission layer.

**Verify UNIT-B by prefix, not whole-file hash**, until it is rewritten.

---

## 4. Constraints to respect (not bugs)

- **`dd` on `/dev/iio:*` wedges the device.** Raw `read()`/`write()` does not
  program the DMA on 6.12, so `dd` blocks forever AND holds the single-open
  character device, making every later `iio_readdev` fail with `EBUSY`. One
  stale `dd` invalidated an entire round of measurements. Use libiio tools, and
  pass the device NAME -- the sysfs path and `iio:deviceN` both give a silent
  0-byte capture.
- **The IQ probes record only on `tvalid & tready`.** An undrained receive chain
  backpressures the packetizer and every probe word reads zero, which is
  indistinguishable from "no signal". Always read the probe's `# status` word:
  it is the only thing separating "never armed" from "armed and saw nothing".
- **`capturable = 0` is NO SIGNAL, never `PER 0.00%`.** The analysis tool prints
  the latter, and it has misled work here more than once.
- **The fabric modulator cannot be bypassed.** With `mod_en=0` the far end sees
  the noise floor; the chain is DMA -> `qpsk_mod` -> `axis_to_iq` -> DAC.
  Pre-modulated IQ cannot be replayed, so the historical method of isolating the
  demodulator is unavailable on this bitstream.
- **Single-board loopback is not a test bed.** Bare connectors gave ~2 dB SNR
  against the ~25 dB needed. With antennas it reaches usable level, but it
  cannot characterise a link.
- **The appliance transmits at 0 dB attenuation.** `tx_fabric.sh` sets it, so
  that is the state of an autostarted unit. Before any coax connection, use
  `fpga/tools/rf_power_bringup.sh`, which steps power UP from the hardware
  minimum against the far end's measured level.
- **jffs2 is 896 KB and fragments.** Delete, `sync`, confirm the space came back,
  write, then verify by checksum.
- **USB port 1-8 on the development machine is unreliable.** It failed two
  devices with `error -71` about 5 s after enumeration. UNIT-B has been stable
  since moving to 1-6. Not a board fault.
- **Never `pkill -f <pattern>` where the pattern appears in your own command
  line.** It matches the searching shell. Resolve by `/proc/PID/exe`, or use
  `pkill -x`.

---

## 5. Earlier conclusions now corrected

These are recorded because each was believed and acted upon:

- **"UNIT-A's fabric modulator has never produced output"** (`7f77e83`) -- no
  longer true on the golden bitstream. Both modulators correlate **1.0000**
  against the host reference.
- **"The fabric modulator is the least-proven element and the likely culprit"**
  -- wrong; it is bit-exact on both boards. The fault was the demodulator
  stalling.
- **"~179 bytes lost per DMA buffer refill"** (`phase9-dma-loss.md`) -- fitted
  indirectly from capture rate against buffer size. Direct measurement of the
  gap between consecutive packets gives **0 B median, max 1 B, 100% duty
  cycle**. Phase 9 should be re-scoped against the direct measurement before any
  buffer or deframer work.
- **"192.168.2.1 is reachable, so UNIT-B is up"** -- it was routing via
  `wlp2s0` to the internet. Check the route, not the ping.
