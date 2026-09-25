# Authenticated diagnostics API (`sdr-agent`)

`sdr-agent` is the on-board management daemon: one small service per unit that
owns the read-only HTTP diagnostics surface. It is intended to replace routine
SSH inspection; it does not expose a shell, configuration writes, resets, or
arbitrary file access. Source: `fpga/tools/sdr_agent.c`; test:
`tests/sdr_agent_test.sh`.

## Endpoints

All endpoints require `Authorization: Bearer <token>`.

| Endpoint | Source |
|---|---|
| `GET /api/v1/health` | API process health |
| `GET /api/v1/status` | `/mnt/jffs2/appliance_status.sh` (the whole document) |
| `GET /api/v1/fpga`, `/modem`, `/config`, `/ethernet`, `/supervisor`, `/progress` | That top-level section of the status document |
| `GET /api/v1/radio` | Requested (`bridge.conf`) **and** actual (AD9363 IIO sysfs) RF configuration, side by side, with a `match` verdict |
| `GET /api/v1/metrics` | `/tmp/bridge_stats.json` |
| `GET /metrics`, `/api/v1/bridge` | Aliases of `/api/v1/metrics` |
| `GET /api/v1/peer`, `/queues` | That top-level section of the bridge metrics |
| `GET /api/v1/events` | Bounded selection of important appliance events |
| `GET /api/v1/logs` | Last 64 KiB of the appliance log |
| `GET /api/v1/diagnostic-bundle` | Status, metrics, radio, events, and recent log in one bounded JSON document (`bundle_version` 2) |
| `GET /api/v1/stream` | Live telemetry, one JSON object per line every ~1 s, [Server-Sent Events](https://developer.mozilla.org/en-US/docs/Web/API/Server-sent_events) |
| `GET /api/v1/fault-history` | The persistent fault journal (`/mnt/jffs2/fault_history.jsonl`), same shape as `/events` -- survives a reboot, `/events` does not |
| `GET /api/v1/bundles` | List of automatically captured diagnostic bundles (id, trigger, when) |
| `GET /api/v1/bundles/latest`, `/api/v1/bundles/<id>` | One captured bundle in full |

Unknown routes return 404. Any method other than GET returns 405. Missing or
incorrect authentication returns 401. A section that the source document does
not contain returns 503, never an empty object.

`/api/v1/radio` exists because debugging from what software asked for is how a
link was once chased while the LO sat at the previous frequency. `requested`
comes from `bridge.conf` (`FREQUENCY`, `RX_FREQUENCY`, `SAMPLE_RATE`,
`TX_RF_BANDWIDTH`, `RX_RF_BANDWIDTH`, `TX_ATTENUATION_DB`, `RX_GAIN_MODE` --
required by the schema since schema 2, null only on an older config);
`actual` is read live from the `ad9361-phy` IIO device (LOs, sample rate, RF
bandwidths, `ensm_mode`, TX attenuation, TX LO powerdown, RX gain mode and
gain, RSSI, die temperature). `match` compares LOs and sample rate within
`tolerance_hz`, because the PLL quantises (444000000 requested reads back
443999998).

### `/api/v1/stream`: live telemetry

A polling dashboard means every viewer re-runs the diagnostics bundle's full
cost (a status-script fork plus a metrics-file read) on its own schedule, and
"roughly continuous" visibility competes with every other client for the same
one-request-at-a-time budget every other route in this API uses. This route
is the exception: the connection stays open, and the server pushes one JSON
object per line, framed as `data: {...}\n\n` (plain [SSE](https://developer.mozilla.org/en-US/docs/Web/API/Server-sent_events);
a browser's `EventSource` consumes it directly, and `curl -N` shows it raw).

```json
data: {"t":1758790000123,"rssi_db":69.25,"rx_gain_db":47.000000,"bridge":{"tx":{...},"rx":{...},"queues":{...},"peer":{...},"cpu_decode_pct":43.6,"recoveries":0},"supervisor":{"bridges_running":1,"supervisors":1}}
```

`bridge` is `/tmp/bridge_stats.json` embedded verbatim -- it already carries
everything the roadmap named except RSSI/gain (frames, delivered bytes, CRC
errors, duplicates, `rx.self`, DMA progress, queue depths, peer state, CPU,
recoveries), so nothing here re-derives those fields. RSSI and gain come from
two direct IIO sysfs reads. `supervisor` is resampled every 10th tick, not
every tick -- it changes only on a bridge restart or fault, so paying its
`/proc` scan at the same rate as the traffic counters would be pure overhead
against every connected subscriber.

The connection forks: accepting it and then blocking on it for as long as the
client stays subscribed would stop this single-threaded daemon from serving
anyone else at all. The parent returns to `accept()` immediately; the child
owns the socket until a write to it fails (the client disconnected) or a
bounded ~4-hour session limit is reached (a live viewer's `EventSource`
reconnects on its own). At most **4** concurrent subscribers; a 5th gets 503
rather than being queued. Same bearer-token check as every other route,
applied before the fork.

### `/api/v1/fault-history`: what happened before the last reboot

`/tmp/appliance.log` is on the ramdisk; a reboot wipes it, and with it every
event `/api/v1/events` could otherwise show. `/mnt/jffs2/fault_history.jsonl`
is the one thing on this board that does not go away: `sdr-agent` mirrors
every event it recognises -- log-classified (`PEER_STALE`, `DEMOD_RECOVERY`,
`BRIDGE_RESTART`, ...) and counter-derived (`RF_LOSS`, `QUEUE_DROP`) alike --
into it as they happen, and this endpoint serves it back in the same shape as
`/api/v1/events` (`{"events":[...]}`), so the two are directly comparable.

Deliberately a FAULT JOURNAL, not telemetry: only edge-triggered transitions
ever reach it, at most a handful per hour even on a struggling link, never a
per-second counter dump -- this is flash with finite write endurance and no
swap to fall back on if it were exhausted. Bounded at 32 KiB
(`FAULT_JOURNAL_MAX_BYTES`; test-only override `--fault-max-bytes`): oldest
whole entries are dropped once the file exceeds that, never a partial line
that would leave a truncated object on disk. Each line is a self-contained
JSON object appended independently, so a crash mid-write loses at most the
one entry being written, never corrupts what came before it.

### `/api/v1/bundles`: automatic diagnostic bundle on fault

`/api/v1/diagnostic-bundle` only captures what an operator asks for, at
whatever moment they happen to ask -- a transient condition can be long gone
by the time anyone looks. `sdr-agent` captures the same KIND of snapshot
itself, automatically, the moment it recognises one of:

- `SUPERVISOR_FAULT` or `PREFLIGHT_FAIL` -- either on its own is a FAULT;
- `REPEATED_RECOVERY` -- 3 or more `DEMOD_RECOVERY` events within 300 s
  (`RECOVERY_WINDOW_S`/`RECOVERY_WINDOW_THRESHOLD`; a single recovery is the
  modem doing its job, not a fault worth a snapshot on its own);
- `RF_LOSS` -- the same 120 s stall `/api/v1/fault-history` above already
  treats as a fault.

Each captured bundle (`bundle_version` 1) contains everything the roadmap
named: `trigger` and `detail`, `software_version` (`/mnt/jffs2/sdr-release`),
`config` (the raw `bridge.conf`, bounded to 4 KiB) with `config_fnv1a` (a
drift-detection hash, not a cryptographic one -- the same reasoning and
algorithm `LoopGuard.hpp` uses elsewhere in this project for the same "notice
if this changed" purpose), the `fpga` and `supervisor` sections of the status
document, `radio` (requested vs actual, the same view `/api/v1/radio` gives),
`bridge` (bridge_stats.json, counters and peer state together), and
`recent_events` (the structured distillation, not raw log text).

Its own format, not `/diagnostic-bundle`'s: that one embeds the WHOLE status
document and the full 64 KiB log ring, sized for one on-demand pull. Every
field here comes from a specific section or an already-structured source, so
several of these fit on flash for what one `/diagnostic-bundle` pull alone
would cost. Bounded on every axis: at most 5 bundles kept (oldest pruned,
`MAX_BUNDLES`), and captures are rate-limited to at most one per 60 s
(`BUNDLE_CAPTURE_COOLDOWN_S`; test-only overrides `--bundle-cooldown-s` and
`--recovery-window-s`) so a flapping fault cannot spam the flash with
captures.

`GET /api/v1/bundles` lists what has been captured (id, trigger, when, not
the full content); `GET /api/v1/bundles/<id>` or `.../bundles/latest` fetches
one in full. `<id>` is validated as a plain non-negative integer before it
ever reaches a filename -- never passed through unchecked.

## Security contract

- Bind to the USB management address, never `0.0.0.0`.
- Generate a unique random token per unit.
- Store the token in a regular file with mode 0600. The service refuses to
  start if group or other permissions are present.
- HTTP is plaintext. Treat the USB management network as trusted and isolated;
  do not route this port onto the RF/user Ethernet or the public Internet.
- The status collector is executed directly with `exec`, never through a
  shell. Request parameters cannot select commands or paths.
- Requests, command runtime, status output, metrics, logs, events, and socket
  waits all have fixed bounds.

Structured events include `timestamp_monotonic_s`, `severity`, `subsystem`,
`code`, `node_id`, `peer_id`, and the original bounded message. The service
normalizes peer, recovery, preflight, configuration, supervisor, and hang
events from the existing appliance log -- including telling a bridge's first
start in a restart window (`BRIDGE_START`, informational) from the supervisor
bringing it back up after a crash or hang (`BRIDGE_RESTART`, a warning; the
two share one log line, distinguished only by the attempt number in it). New
log producers should emit stable event codes directly rather than requiring
further text classification.

Two codes have no line in the log to classify at all: `RF_LOSS` (the receiver
stopped decoding anything -- `bridge_stats.json`'s `rx.frames` has not moved
in over 120 s, the same threshold and field `appliance_status.sh`'s own
`progress.verdict` already uses, so the two never disagree) and `QUEUE_DROP`
(`queues.control_drops` or `.bulk_drops` increased). `sdr-agent` derives both
itself by polling that file about once a second and comparing against its own
last sample; each is edge-triggered, firing once per transition rather than
once per poll for as long as the condition persists, and `RF_LOSS_CLEARED`
follows once `rx.frames` resumes.

Example:

```sh
TOKEN=$(cat diagnostics.token)
curl -H "Authorization: Bearer $TOKEN" \
  http://192.168.2.1:8088/api/v1/status
```

## Deployment state

`sdr-agent` is plain C, cross-built **dynamically** against the board's shared
glibc (`release/build-release.sh`): ~22 KiB stripped, needing only `libc.so.6`.
Its static C++ predecessor was ~810 KiB and never fitted the jffs2 partition
(~280 KiB free after the bridge), so it only ever ran from `/tmp` and vanished
at reboot. The release installs the agent to `/mnt/jffs2/sdr-agent` and
`autorun.sh` starts it at boot, bound to the usb0 address the board actually
has, port 8088.

Tokens: `flash.sh` generates one per unit **once** (kept across re-flashes so
host copies stay valid), stores it at `/mnt/jffs2/agent.token` (0600) and
mirrors it to `$SDR_TOKEN_DIR` (default `~/.config/sdr/tokens/<serial>.token`,
directory 0700, file 0600).

## Central collection

The host collector retrieves all bounded endpoints from one or more units and
writes a timestamped mode-0700 evidence directory. Tokens are never archived.

```sh
scripts/collect-diagnostics.py \
  --unit UNIT-A,http://192.168.2.17:8088,~/.config/sdr/tokens/AKRLJ24FWXM7H65X.token \
  --unit UNIT-B,http://192.168.2.1:8088,~/.config/sdr/tokens/3SXRLJMXS7EL5IBJ.token \
  --output diagnostic-archive
```

Token files must be mode 0600. A partial collection is retained, records each
failed endpoint in `manifest.json`, and exits nonzero.
