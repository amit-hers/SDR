# Pluto+ Remote Diagnostics Cheat Sheet

## Current unit addresses

| Unit | USB management IP | TX | RX | Node ID |
|---|---:|---:|---:|---:|
| UNIT-A | `192.168.2.17` | 434 MHz | 444 MHz | `327951062` |
| UNIT-B | `192.168.2.1` | 444 MHz | 434 MHz | `465429558` |

Both units should report FPGA version 1.3.0, ABI 3, register map 4,
`RX_PKT_BYTES=8192`, and sample rate 15.36 MS/s.

## Diagnostics API: current deployment

`sdr-agent` runs persistently on BOTH units from jffs2, started by autorun.sh
at boot, on the USB management address:

```text
UNIT-A  http://192.168.2.17:8088   token ~/.config/sdr/tokens/AKRLJ24FWXM7H65X.token
UNIT-B  http://192.168.2.1:8088    token ~/.config/sdr/tokens/3SXRLJMXS7EL5IBJ.token
```

The on-board copy is `/mnt/jffs2/agent.token`; it is kept across re-flashes.

Load a token without printing it:

```bash
TOKEN=$(tr -d '\r\n' < ~/.config/sdr/tokens/3SXRLJMXS7EL5IBJ.token)
API=http://192.168.2.1:8088
```

Never paste the token into logs, tickets, screenshots, or shell history.

## Quick health check

```bash
curl -sS --fail --max-time 5 \
  -H "Authorization: Bearer $TOKEN" \
  "$API/api/v1/health"
```

Expected:

```json
{"status":"ok","service":"sdr-agent","version":2}
```

HTTP meanings:

| Code | Meaning |
|---:|---|
| 200 | Request succeeded |
| 401 | Token missing or incorrect |
| 404 | Endpoint does not exist |
| 405 | Write method rejected; API is read-only |
| 503 | Underlying status, metrics, events, or logs unavailable |

## Requested vs actual RF (the first thing to check on a dead link)

```bash
curl -sS --fail --max-time 5 -H "Authorization: Bearer $TOKEN" "$API/api/v1/radio" | python3 -m json.tool
```

`match.tx_lo`, `match.rx_lo`, `match.sample_rate` must all be `true`;
`actual.tx_lo_powerdown` must be `0`; `actual.ensm_mode` must be `fdd`.
Single sections are also available: `/api/v1/fpga`, `/peer`, `/queues`,
`/supervisor`, `/ethernet`, `/progress`, `/modem`, `/config`.

## Full appliance status

```bash
curl -sS --fail --max-time 5 \
  -H "Authorization: Bearer $TOKEN" \
  "$API/api/v1/status" | python3 -m json.tool
```

Important fields:

```text
fpga.magic                  must be 0x5344524C
fpga.version                must be 0x00010300
fpga.abi                    must be 0x00000003
fpga.regmap                 must be 0x00000004
fpga.packet_bytes           must be 0x00002000
config.status               must be valid
config.provisioning         must be matches_hardware
bridge.peer.compatibility   should be COMPATIBLE
bridge.tx.errors            should remain 0
bridge.rx.inject_err        should remain 0
queues.control_drops        should remain 0
queues.bulk_drops           should remain 0
supervisor.bridges_running  must be 1
supervisor.supervisors      must be 1
supervisor.faulted          must be 0
progress.verdict            should be forwarding
```

## Bridge metrics only

```bash
curl -sS --fail --max-time 5 \
  -H "Authorization: Bearer $TOKEN" \
  "$API/api/v1/metrics" | python3 -m json.tool
```

Fast summary with `jq`, if installed:

```bash
curl -sS -H "Authorization: Bearer $TOKEN" "$API/api/v1/metrics" |
  jq '{peer:.peer.compatibility,
       tx_packets:.tx.packets,
       tx_errors:.tx.errors,
       rx_dma:.rx.dma,
       rx_frames:.rx.frames,
       rx_bytes:.rx.bytes,
       crc_errors:.rx.crc_errors,
       recoveries,
       cpu_decode_pct,
       queues,
       loop_guard}'
```

Counter interpretation:

| Observation | Likely layer |
|---|---|
| `rx.dma` not increasing | RX DMA/IIO path stopped |
| `rx.dma` increases but `rx.frames` does not | demodulator/acquisition stalled |
| `rx.frames` increases but `rx.bytes` does not | only control traffic, or no endpoint data |
| `peer=STALE` | compatible HELLO not received recently |
| `peer=INCOMPATIBLE` | packet size, ABI, map, sample rate, or frequency mismatch |
| `recoveries` increases | automatic demodulator recovery occurred |
| `crc_errors` rises rapidly | RF quality, attenuation, frequency, or acquisition problem |
| queue depth/drops increase | offered load exceeds sustainable transport rate |
| `loop_guard.suppressed` increases | returned L2 frames are being blocked |

## Structured events

```bash
curl -sS --fail --max-time 5 \
  -H "Authorization: Bearer $TOKEN" \
  "$API/api/v1/events" | python3 -m json.tool
```

Event fields include monotonic timestamp, severity, subsystem, stable code,
node ID, peer ID, and message. Important codes include:

```text
PEER_COMPATIBLE
PEER_INCOMPATIBLE
PEER_STALE
DEMOD_RECOVERY
PREFLIGHT_FAIL
CONFIG_REJECTED
BRIDGE_START        first bridge start in a restart window -- ordinary, at boot
BRIDGE_RESTART      the supervisor bringing it back up after a crash or hang
BRIDGE_HUNG
SUPERVISOR_FAULT
RF_LOSS             rx.frames stalled for over 120s; RF_LOSS_CLEARED once it resumes
QUEUE_DROP          a control- or bulk-queue frame was dropped
```

Most codes are classified out of a line the bridge or supervisor already
prints. `RF_LOSS` and `QUEUE_DROP` are the two exceptions -- nothing prints a
line when those happen, so `sdr-agent` derives them itself by polling
`bridge_stats.json` about once a second and noticing when `rx.frames` stops
moving or a drop counter goes up. Both are edge-triggered: one event per
transition, not one every second for as long as the condition holds.

An empty event list is not automatically a fault. The ring holds only recent
important events, not ordinary statistics lines.

## Recent logs

Print the bounded 64 KiB RAM log ring:

```bash
curl -sS --fail --max-time 5 \
  -H "Authorization: Bearer $TOKEN" \
  "$API/api/v1/logs"
```

Save it:

```bash
curl -sS --fail --max-time 5 \
  -H "Authorization: Bearer $TOKEN" \
  "$API/api/v1/logs" \
  -o "unit-b-log-$(date -u +%Y%m%dT%H%M%SZ).txt"
```

The in-memory ring is capped at 64 KiB and the source `/tmp/appliance.log` is
capped at 256 KiB while the diagnostics service is running.

## Download a diagnostic bundle

```bash
OUT="unit-b-diagnostic-$(date -u +%Y%m%dT%H%M%SZ).json"
curl -sS --fail --max-time 10 \
  -H "Authorization: Bearer $TOKEN" \
  "$API/api/v1/diagnostic-bundle" -o "$OUT"
python3 -m json.tool "$OUT" >/dev/null && echo "valid bundle: $OUT"
sha256sum "$OUT"
```

The bundle contains one status snapshot, bridge metrics, structured events,
and recent logs. Capture this before restarting a failed service whenever
possible.

### Collect both units centrally

After the API is running on both units with separate tokens:

```bash
scripts/collect-diagnostics.py \
  --unit UNIT-A,http://192.168.2.17:8088,/secure/unit-a.token \
  --unit UNIT-B,http://192.168.2.1:8088,/secure/unit-b.token \
  --output diagnostic-archive
```

This creates a UTC-timestamped directory with per-unit evidence and a
`manifest.json` containing sizes and SHA256 hashes. Tokens are not archived.

## Watch status continuously

```bash
watch -n 2 "curl -sS --max-time 2 \
  -H 'Authorization: Bearer $TOKEN' \
  '$API/api/v1/metrics' | python3 -m json.tool"
```

Do not use a sub-second interval; diagnostics must not become load traffic.

## Live telemetry stream

The above polls; this is pushed, roughly once a second, for as long as the
connection stays open (Server-Sent Events -- a browser dashboard would use
`EventSource`, this is the same thing from a terminal):

```bash
curl -sS -N --max-time 30 -H "Authorization: Bearer $TOKEN" \
  "$API/api/v1/stream"
```

Each line is `data: {...}`. At most 4 concurrent subscribers per unit; a 5th
gets 503. Supervisor state only appears on every 10th sample -- it barely
changes, so it is not worth sampling at the same rate as the traffic counters.

## What happened before the last reboot

`/api/v1/events` reads `/tmp/appliance.log`, which is on the ramdisk and gone
after a reboot. This survives it:

```bash
curl -sS --fail --max-time 5 \
  -H "Authorization: Bearer $TOKEN" \
  "$API/api/v1/fault-history" | python3 -m json.tool
```

Same shape as `/events`. It only ever holds edge-triggered faults and
transitions, at most 32 KiB total, never a telemetry dump -- this is flash.

## Diagnostic bundles captured automatically

`sdr-agent` captures a bundle itself, without anyone asking, the moment it
sees `SUPERVISOR_FAULT`, `PREFLIGHT_FAIL`, `RF_LOSS`, or 3+ `DEMOD_RECOVERY`
within 5 minutes (`REPEATED_RECOVERY`). At most 5 kept, at most one capture
per minute even if the fault keeps repeating.

```bash
curl -sS --fail --max-time 5 -H "Authorization: Bearer $TOKEN" \
  "$API/api/v1/bundles" | python3 -m json.tool          # list: id, trigger, when

curl -sS --fail --max-time 5 -H "Authorization: Bearer $TOKEN" \
  "$API/api/v1/bundles/latest" | python3 -m json.tool   # the most recent one, in full
```

Each one carries config (+ a drift-detection hash), software version, FPGA
identity, requested-vs-actual radio, bridge counters and peer state,
supervisor state, and recent structured events -- everything you would
otherwise have had to SSH in and collect by hand, from the moment the fault
actually happened rather than whenever you got to it.

## Safe remote control

Five bounded, authenticated actions -- never a shell, never an arbitrary
command or path. Each is `POST`-only (a `GET` is 405) and additionally
requires `?confirm=yes` (missing it is 400), so a prefetch, a monitoring
crawler, or a browser revisiting history can never trigger one by accident.
Every invocation is logged as a structured event before it acts.

```bash
curl -sS --fail -X POST -H "Authorization: Bearer $TOKEN" \
  "$API/api/v1/control/restart_bridge?confirm=yes"
```

| Action | Effect |
|---|---|
| `restart_bridge` | Kills `sdr_bridge` and any leftover `iio_readdev`/`iio_writedev` helpers by `/proc` identity, same match `appliance_supervise.sh`'s own `kill_bridges()` uses. Reports `supervisor_present` so you know whether anything will bring it back up. |
| `clear_counters` | Sends `SIGUSR1` to `sdr_bridge` only (never the IIO helpers, which have no handler for it and would just die). The bridge zeros its REPORTING counters on its own next stats tick -- never the DMA/frame counters a recovery detector reads against, so this cannot itself trigger a spurious demod reset. |
| `reset_demod` | Runs `reset_demod.sh` (`/mnt/jffs2/tools/reset_demod.sh`): a direct register pulse, no separate drain process, because a running bridge's own `iio_readdev` already drains the RX DMA the reset needs. Do not reuse `rx_framed.sh`'s bring-up reset dance here -- it spawns its own reader and would EBUSY against the bridge's. |
| `enter_safe_mode` | Kills the supervisor first, then the bridge -- that order so the supervisor cannot notice the bridge died and bring it back before it too is gone. Appliance stays non-forwarding until reboot or manual intervention. |
| `restart_appliance` | `exec`s `/sbin/reboot -f` directly (never through a shell): plain `reboot` is a documented no-op on this firmware. The response is sent before the fork, so the caller's ack reaches them before the board actually goes down. |

## Safe configuration API

`bridge.conf` is never overwritten with anything that has not already been
validated. Fetch the current file, edit it, and post it back:

```bash
curl -sS --fail -H "Authorization: Bearer $TOKEN" "$API/api/v1/config/raw" \
  | python3 -c 'import json,sys; sys.stdout.write(json.load(sys.stdin)["config"])' \
  > candidate.conf
# edit candidate.conf, then:
curl -sS --fail -X POST -H "Authorization: Bearer $TOKEN" \
  --data-binary @candidate.conf "$API/api/v1/config?confirm=yes" | python3 -m json.tool
```

The response names every stage reached:
`{"action":"apply_config","validated":true,"applied":true,"verified":true,"rolled_back":false}`.
Behind that one call: the candidate is validated on disk (never the live
file) by the same `config_schema.sh` the appliance itself uses at boot; only
a validated candidate is backed up (`bridge.conf.prev`) and atomically
installed; the appliance is then killed and relaunched to pick it up
(restarting just the bridge is not enough -- the supervisor's arguments are
fixed at the moment it was started); and the AD9363 is polled for up to 20 s
to actually converge on the new values before this is called `verified`. A
candidate that fails validation never touches `bridge.conf` at all, and the
validator's own error text comes back in `validation_output`. An applied
config that never verifies is rolled back to the one backup this same
request made, and `CONFIG_APPLIED`/`CONFIG_ROLLBACK` are logged as
structured events either way -- check `/api/v1/events` if the connection
itself gets interrupted by the restart it triggered.

## Test authentication without exposing the token

Missing token should return 401:

```bash
curl -sS -o /dev/null -w '%{http_code}\n' "$API/api/v1/status"
```

A write request should return 405:

```bash
curl -sS -o /dev/null -w '%{http_code}\n' \
  -X POST -H "Authorization: Bearer $TOKEN" \
  "$API/api/v1/status"
```

## USB management reachability

```bash
ping -c 3 192.168.2.17   # UNIT-A
ping -c 3 192.168.2.1    # UNIT-B
ip route get 192.168.2.17
ip route get 192.168.2.1
ip neigh | grep '192.168.2.'
```

These pings test USB management only. They do not test Ethernet-over-RF.

## End-to-end Ethernet-over-RF test

Separated topology:

```text
Computer A 10.77.0.1/24 -> UNIT-A RJ45 -> RF -> UNIT-B RJ45 -> Computer B 10.77.0.2/24
```

On Computer A:

```bash
sudo ip link set enp3s0 mtu 1400
sudo ip addr replace 10.77.0.1/24 dev enp3s0
ping -I enp3s0 -c 20 10.77.0.2
```

On Computer B / Parallels VM:

```bash
sudo ip link set enxc84d44201e3d mtu 1400
sudo ip addr replace 10.77.0.2/24 dev enxc84d44201e3d
ping -I enxc84d44201e3d -c 20 10.77.0.1
```

Do not connect both SDR RJ45 ports to the same switch during normal Phase 8;
that creates an alternate copper path. The shared-switch topology is only for
the dedicated LoopGuard test.

## SSH fallback

Use SSH only when the API is unavailable or deeper inspection is necessary:

```bash
ssh root@192.168.2.17   # UNIT-A, password analog
ssh root@192.168.2.1    # UNIT-B, password analog
```

If host keys change after reboot:

```bash
ssh-keygen -R 192.168.2.17
ssh-keygen -R 192.168.2.1
```

Useful read-only board commands:

```bash
/mnt/jffs2/appliance_status.sh
cat /tmp/bridge_stats.json
tail -n 100 /tmp/appliance.log
ps | grep -E '[s]dr_bridge|[a]ppliance'
cat /sys/class/net/eth0/carrier
ifconfig eth0
cat /proc/uptime
uname -a
```

FPGA identity:

```bash
devmem 0x43C50000 32   # magic:       0x5344524C
devmem 0x43C50004 32   # version:     0x00010300
devmem 0x43C50008 32   # ABI:         0x00000003
devmem 0x43C5000C 32   # register map:0x00000004
devmem 0x43C50018 32   # packet bytes:0x00002000
```

Radio sample rates:

```bash
cat /sys/bus/iio/devices/iio:device0/in_voltage_sampling_frequency
cat /sys/bus/iio/devices/iio:device0/out_voltage_sampling_frequency
```

Expected value is `15360000` for both.

## Before taking recovery action

1. Download a diagnostic bundle.
2. Record `/proc/uptime`.
3. Record FPGA identity and bridge metrics.
4. Check whether counters are still advancing.
5. Record peer compatibility and recovery count.
6. Only then restart or power-cycle.

Do not reflash merely because a peer is stale. A stale peer can result from RF
loss, incorrect crossed frequencies, a stopped remote bridge, or acquisition
that needs automatic recovery.

## Current limitations

- API transport is plaintext HTTP. Use only the isolated USB management link.
- No remote write, reset, capture, or configuration endpoints exist yet.
- Historical graphs and fleet aggregation belong to the upcoming dashboard.
- The live stream keeps no history; a client that misses a sample has lost
  it. Use the diagnostic bundle for a point-in-time snapshot, or
  `/api/v1/fault-history` for faults and transitions that predate now --
  it is the one thing here that survives a reboot, but it is bounded to
  32 KiB of edge-triggered events, not a substitute for real telemetry
  history.
