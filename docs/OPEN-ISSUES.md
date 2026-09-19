# Open issues

Everything currently open, with the evidence behind it. Anything stated as
measured was measured; anything unproven says so.

---

## 1. Blocked on physical setup (not software)

### 1.1 Phase 8 cannot start: both RJ45 ports share one switch segment

**Corrected 2026-09-19.** This was recorded as a direct cable between the two
units. It is a switch, and the development host's `enp3s0` is on it too. Proven
by injecting a broadcast from the host: both boards' NICs received exactly 39
frames each. `enp3s0` also carries the host's internet (`67.186.1.88/24`), so a
forwarding loop here would disrupt more than the test.

A ping between two hosts on this segment crosses copper and would report a
sub-millisecond RTT that says nothing about the radio. Isolation needs either
two separate NICs or VLAN-separated switch ports.

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

### 1.1a LOOP GUARD IN PLACE: UNIT-B's appliance autostart is disabled

The loop in 1.1 became reachable once forwarding was fixed. Earlier it could not
fire because the binary deployed on the boards forwarded nothing (issue 3.6);
both boards now run a build that does, and both had the appliance hooked into
`autorun.sh`. The next power cycle would have brought up two promiscuous
bridges on one switch segment -- and that segment carries the development
host's internet uplink, so the storm would not have stayed inside the test.

**Mitigation applied:** the appliance hook in `/mnt/jffs2/autorun.sh` on
**UNIT-B only** is commented out, marked `LOOP GUARD`, with the original saved
as `autorun.sh.bak`. UNIT-A still autostarts -- a single bridge cannot loop.

**To re-enable** (after the two RJ45 ports are on separate segments): delete the
`#` in front of the `[ -x /mnt/jffs2/appliance_start.sh ]` line on UNIT-B, or
restore `autorun.sh.bak`.

This is a workaround for the topology, not a fix. A transparent L2 bridge that
can be cabled into a loop should defend itself -- the frames it injects are
indistinguishable to it from frames a peer originated, so suppressing them needs
something the bridge can recognise, such as tagging its own injections or
learning which source MACs arrive from the radio. Worth designing before the
product is deployed anywhere with a switch behind it.

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

## 2b. Fixed on 2026-09-19 (verify after any regression)

| | fix | commit |
|---|---|---|
| Both units transmitted and received on ONE frequency, so each jammed its own receiver and nothing crossed | frequency-division duplex; `rx_framed.sh` RX LO is now a parameter, `appliance_start.sh` passes `RX_FREQUENCY` and warns when TX==RX | `2057ba7` |
| No L2 loop suppression: two appliances on one segment would replicate without bound | `LoopGuard` source learning, 18 unit tests | `2efd8ab` |
| Demod recovery could not rescue a COLD stall -- `recovery_armed` started false and only armed after a frame decoded | armed from startup | `dacbbd0` |
| Recovery trigger demanded `mu_clamped` climb, missing a stall where it moved by 1 in 20 s | trigger on DMA advancing with the deframer inactive | `34f2160` |
| Recovery then reset HEALTHY demodulators whenever traffic repeated | liveness counts frames + duplicates + CRC failures | `34f2160` |
| Interface drop counter reported the kernel's boot-time total as bridge loss | baselined at start, labelled with the real interface | `780968f` |

## 3. Open, not yet explained

### 3.1 The link asymmetry disappears at stronger signal

**Updated 2026-09-19.** Re-measured after the boards were repositioned, with
both running the same binary:

| direction | RSSI | PER | CRC | payload mismatches |
|---|---|---|---|---|
| A -> B | 67.75 dB | **0.00%** (3573/3573) | 0 | 0 |
| B -> A | 65.00 dB | **0.03%** (1/3573) | 1 | 0 |

Signal is about 17 dB stronger than the runs below (65-68 dB against 83-84 dB)
and the asymmetry is gone: A -> B went from a repeatable 1.4-1.6% to 0.00%.

That supports placement or antenna coupling rather than a defective board, which
is why no board was named. The antenna-swap test in 1.3 is now lower priority --
the effect it was meant to explain is not present at this signal level. What it
does establish is that the earlier figures were a property of the SETUP at that
distance, not of the units, so neither set should be quoted as a hardware
specification.

The original measurements are kept below for the record.

### 3.1a The earlier asymmetry, at 83-84 dB RSSI (historical)

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

### 3.3 RESOLVED: the low-rate anomaly was a signal-level artifact

**Closed 2026-09-19.** Re-measured at RSSI 68 dB: 3.84 MS/s gives **0.00% PER**
(3598 frames) and 2.40 MS/s also 0.00%, while 7.68 MS/s gives 0.03%. Throughput
scales linearly and stays within 0.9% of theoretical at every rate. The
"degrades at the lower rate" effect below is absent at adequate signal -- it was
the same marginal-level artifact as the direction asymmetry in 3.1, not a
property of the rate. Original text follows.

### 3.3a The original observation (historical)

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

### 3.6 A deployed bridge binary forwarded nothing; the repo build does

Observed on UNIT-A, 2026-09-19. With the binary that was on the board
(md5 `3173e787...`) the appliance brought the modem up correctly and the NIC
received 201 injected frames, while the bridge reported **`tx 0 pkts`** -- it
forwarded none of them. Rebuilding from the repo (md5 `d80799ee...`) and running
the identical appliance config forwarded **all 200** (`tx 200 pkts / 15600 B`)
and additionally decoded 91 frames of its own transmission through antenna
self-coupling.

The difference is the binary, not the configuration. The previous one is saved
on the board at `/tmp/sdr_bridge.other` rather than discarded.

**Always verify a deployed binary against the build it is supposed to be.** A
bridge that silently forwards nothing looks exactly like a dead RF link, and
that is where the investigation would have gone.

### 3.7 Two sessions are editing the same files

The repository carries three commits and twelve uncommitted changes from another
session, including a 4 KiB packetizer, `axi_version_id` changes, and autonomous
demodulator recovery built on the soft_reset finding above. `sdr_bridge.cpp` is
modified in that set.

That work is left untouched here, and nothing has been committed over it. But
concurrent edits to the same files will produce a conflict or a silent revert
sooner or later, and a binary built from one session's tree was already found
deployed while the other session was debugging it. **Decide which session owns
`fpga/tools/sdr_bridge.cpp` before either changes it again.**

### 3.8 RESOLVED: the two units shared a node id

**Closed 2026-09-19.** Both ran `node_id 1`, so the self-reception filter
discarded every frame the peer sent. The discard preceded the FL_CTRL test, so
keepalives were not counted as control either -- 445,000 frames, 0 bytes,
ctrl 0 were three symptoms of that one cause. `rx_self` was counted but never
printed, which is what made it invisible.

Fixed in `43d56e3`: NODE_ID is a bridge.conf setting, `rx_self` is reported, and
the bridge warns once by name. Ethernet now crosses end to end -- 4100 B
delivered from 50 injected frames, exactly 100%.

### 3.8a Historical description

With FDD configured and both units transmitting, a raw byte stream crosses at
0.00% / 0.06% PER. The appliance bridges do not reproduce it: injected Ethernet
frames reach the far unit's radio and are decoded, but the payload has not been
observed arriving on the far wire. Best observed was 40 frames / 48,000 B on
UNIT-A before the counter went flat.

This is the single blocker for Phase 8, for the loop-guard hardware acceptance,
and for any latency measurement.

### 3.9 The two units run different bridge binaries

UNIT-A `96dede1f...`, UNIT-B `ed148d99...`. Version skew between the ends makes
every result ambiguous -- a binary that forwards nothing was already found
deployed on one unit while the other was being debugged (3.6). **Check both
checksums before interpreting any two-unit measurement.**

### 3.10 Modulator crest factor is 1.09, not 1.71

The TX IQ probe reads rms 5398 / peak 5894 under both the bridge and
`tx_feed.sh`, a crest factor of 1.09-1.10, where the known-good figure recorded
in this project is rms 4465 / peak 7648 = 1.71. Because both drivers agree it
does not explain any current fault, but a near-constant envelope is not what
root-raised-cosine filtered QPSK looks like. Either the probe taps before pulse
shaping or the shaping is not doing what is assumed.

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
- **UNIT-B drops off USB shortly after enumerating. The fault follows the
  BOARD, not the port.** It enumerates correctly -- serial `3SXRLJMXS7EL5IBJ`,
  RNDIS registered -- then resets with `device descriptor read/64, error -71`
  (`-EPROTO`) and disconnects, between 1 and 5 seconds later.

  **This corrects an earlier entry here** which blamed host port 1-8 and said it
  was "not a board fault". UNIT-B failed on 1-8, and now fails identically on
  1-7, while UNIT-A runs fine on 1-8. Two ports, one board.

  Clean enumeration followed by collapse on reset points at the cable or the
  board's USB connector/supply rather than the host. Try in order: a different
  USB cable (cheapest and most likely), then a powered hub, then a different
  board-side connector if one is available. Note the appliance brings the AD9363
  up at 0 dB attenuation shortly after boot, so current draw steps up around the
  time of the failure -- a marginal supply would show exactly this.
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
