# Authenticated diagnostics API (`sdr-agent`)

`sdr-agent` is the on-board management daemon: one small service per unit that
owns the HTTP diagnostics surface, five bounded authenticated control
actions, and a safe configuration-change workflow. It is intended to replace
routine SSH inspection; it does not expose a shell or arbitrary file access.
`bridge.conf` is the one file it ever writes, and only through
validate -> backup -> atomic replace -> verify -> rollback -- every reset,
restart, or config write it can perform is a fixed, hand-written operation,
never a caller-supplied command or path. Source: `fpga/tools/sdr_agent.c`;
test: `tests/sdr_agent_test.sh`.

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
| `GET /api/v1/config/raw` | The full current `bridge.conf`, verbatim -- what a candidate for `POST /api/v1/config` should be built from |
| `POST /api/v1/config?confirm=yes` | Validate, install, apply and verify a candidate `bridge.conf`; rolls back on failure |
| `POST /api/v1/control/restart_bridge?confirm=yes` | Kill the bridge (and any leftover IIO helpers) by process identity |
| `POST /api/v1/control/clear_counters?confirm=yes` | Zero the bridge's reporting counters |
| `POST /api/v1/control/reset_demod?confirm=yes` | Pulse the demodulator's soft-reset register |
| `POST /api/v1/control/enter_safe_mode?confirm=yes` | Kill the supervisor and the bridge; appliance stays non-forwarding |
| `POST /api/v1/control/restart_appliance?confirm=yes` | Reboot the appliance (`reboot -f`) |

Unknown routes return 404. Every route above the control actions and
`/api/v1/config` is GET-only; any other method is 405. The control actions
are POST-only for the same reason in reverse -- a GET must never be able to
trigger one -- and additionally require the literal query string
`confirm=yes`, or 400, as a guard against a client that automatically
follows or prefetches every link it discovers. `/api/v1/config` accepts
both: `GET` reads the section (unchanged from before this existed), `POST`
installs a candidate and requires the same `confirm=yes` gate. Missing or
incorrect authentication returns 401 on every route, control and config
actions included. A section that the source document does not contain
returns 503, never an empty object.

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

### `/api/v1/control/*`: safe remote control

Everything above is read-only. These five are not, and the roadmap for them
was explicit: bounded, authenticated operations, never a general remote
shell. `<action>` is matched against exactly five literal names; anything
else is a plain 404, the same as any other unknown route -- there is no
sixth action and no way for a request to invent one. Every action logs a
structured event (`BRIDGE_RESTART_REQUESTED`, `COUNTERS_CLEARED`,
`DEMOD_RESET_REQUESTED`, `SAFE_MODE_REQUESTED`, `APPLIANCE_REBOOT_REQUESTED`)
before it acts, so what happened and why are never separated.

- **`restart_bridge`** kills `sdr_bridge` and any leftover `iio_readdev` /
  `iio_writedev` helpers by resolved `/proc/<pid>/exe` identity -- the exact
  match `appliance_supervise.sh`'s own `kill_bridges()` already uses, and for
  the same reason that comment gives: a command-line pattern is also present
  in the argv of whatever searches for it. `sdr-agent` does not itself start
  a new bridge; it reports `supervisor_present` so the caller knows whether
  `appliance_supervise.sh` will notice the death and restart it (up to ~60 s
  later, and counting against that script's own `MAX_RESTARTS`/`WINDOW`
  fault budget) or whether nothing will.

- **`clear_counters`** sends `SIGUSR1` to `sdr_bridge` alone -- deliberately
  not the IIO helpers, which install no handler for it and would simply be
  killed by its default disposition. The bridge's signal handler only sets
  an atomic flag (the one thing actually async-signal-safe); its own stats
  thread notices the flag on its next tick and zeros a specific allowlist of
  REPORTING fields. `rx_dma`, `rx_frames`, `rx_dup`, `rx_crcerr`, and
  `recoveries` are excluded on purpose: the same stats tick's recovery
  detector reads them back against local (non-atomic) baselines carried
  between ticks, and zeroing the atomic without also resetting those
  baselines is a `uint64_t` underflow on the very next tick -- it would read
  as a runaway DMA advance and could trigger a demod reset this action never
  asked for. "Clear counters" only ever clears what is purely reported,
  never what a decision is made from.

- **`reset_demod`** execs a fixed script (`reset_demod.sh`, installed to
  `/mnt/jffs2/tools/reset_demod.sh`) that pulses the demodulator's soft-reset
  register directly, with no separate draining subprocess. That matters
  because the register only takes effect while something is actively
  reading the RX DMA fabric; `rx_framed.sh`'s bring-up reset dance satisfies
  that by spawning its own `iio_readdev`, which is correct at bring-up (when
  nothing else is reading) but would `EBUSY` against a live bridge's own
  already-open reader -- the single-open IIO character device allows only
  one. On a running appliance the bridge's own reader already drains it, so
  the live-reset script only needs the pulse.

- **`enter_safe_mode`** kills the supervisor first, then the bridge (and its
  helpers) -- deliberately in that order, so the supervisor cannot notice
  the bridge is gone and bring a new one up in the gap before it too is
  killed. The appliance is left in the same non-forwarding safe state
  `PREFLIGHT_FAIL` already produces, until a reboot or manual intervention.

- **`restart_appliance`** execs `/sbin/reboot -f` directly, never through a
  shell: plain `reboot` is a documented no-op on this board's firmware. The
  HTTP response is sent before the fork that execs it, so the caller's
  acknowledgment reaches them ahead of the board actually going down.

Every action requires `POST` (a `GET` is 405, before the action name or
confirmation is even looked at) and the literal query string `?confirm=yes`
(otherwise 400) -- a deliberate extra gate beyond the bearer token, against a
client that follows or prefetches every link it discovers. None of the five
is exercised end-to-end by the automated test (`tests/sdr_agent_test.sh`)
against a real bridge, supervisor, or reboot binary -- doing so would risk
exactly what these actions are for in a shared test environment. The test
instead runs every action for real against an empty `/proc` match (nothing
in the sandbox resolves to `/sdr_bridge` or `appliance_supervise`) plus a
fixture script standing in for `reset_demod.sh`, and separately confirms
`restart_appliance`'s method and confirm gates without ever letting it fork.
Functional verification of a real restart, reset, and reboot is done on
hardware.

### `/api/v1/config`: safe configuration API

`GET /api/v1/config/raw` returns the full current `bridge.conf` verbatim
(`{"config": "..."}`), bounded to `MAX_CONF_BYTES` (4 KiB) -- the plain
`GET /api/v1/config` section view predates this and stays as it was (six
derived fields from the status document, not the raw file), because it costs
a status-script fork this route doesn't need to pay for a routine read.

`POST /api/v1/config?confirm=yes`, body the full candidate `bridge.conf`
(the same `KEY=VALUE` text `config_schema.sh` expects, not JSON), runs the
whole roadmap in one request: **validate candidate -> apply -> verify ->
rollback**. `bridge.conf` is never overwritten with anything that has not
already passed:

1. **Validate.** The candidate is written to `bridge.conf.candidate` --
   never the live file -- and `config_schema.sh validate` is exec'd directly
   against it (never through a shell). A candidate that fails is deleted; a
   `CONFIG_REJECTED` event is logged; the response carries the validator's
   own error text verbatim (`validation_output`) and `bridge.conf` was never
   touched.
2. **Apply.** A validated candidate is only NOW backed up (the config
   currently in effect is copied to `bridge.conf.prev`) and installed with
   the same atomic rename `config_schema.sh`'s own `write` command uses,
   `sync`ed on both sides. Only then is the appliance actually restarted to
   pick it up: killing just the bridge process is not enough here, because
   `appliance_supervise.sh` runs with the arguments `appliance_start.sh`
   built at the moment it exec'd into it -- restarting the bridge alone
   restarts it with the OLD arguments. So this kills the whole running
   appliance (bridge, its IIO helpers, and the supervisor OR a still-
   bringing-up `appliance_start.sh`, by process identity) and launches a
   fresh `appliance_start.sh`, which re-sources whatever is now on disk.
3. **Verify.** Polls for up to `CONFIG_VERIFY_TIMEOUT_S` (20 s in
   production; `--config-verify-timeout-s` test-only override) for a bridge
   process to exist AND the AD9363 to actually converge on the new
   requested LO/sample-rate values -- a config that only validates
   syntactically is not the same as one the radio actually took.
4. **Rollback.** If verify never succeeds within the timeout, the one
   backup this same request made is restored the same way, and the
   appliance is relaunched again against it. This is best-effort and NOT
   re-verified in turn -- a bad apply cannot recurse into an unbounded retry
   chain. The response's `rolled_back` field says whether this happened; if
   no prior config existed to back up, the new (unverified) one is left in
   place, since there is nothing to roll back to.

The response is one JSON object naming every stage reached:
`{"action":"apply_config","validated":true,"applied":true,"verified":true,"rolled_back":false}`
(a rejected candidate stops after `validated:false` and never sets the
later fields). `CONFIG_APPLIED` and, if it happens, `CONFIG_ROLLBACK` are
both logged as structured events regardless of how the request itself
resolves, so what happened to the appliance is on record even if the HTTP
response never arrives (the connection can be interrupted by the very
restart this request triggered).

This is the one route in this file that can legitimately take up to
`CONFIG_VERIFY_TIMEOUT_S` to answer, and deliberately does not fork the way
`/api/v1/stream` does: applying a config is a rare, operator-initiated
action that already kills and relaunches the whole appliance, and the
caller asked for one coherent apply-verify-rollback outcome, not a
"started" acknowledgment to go poll for separately. The daemon serves no
other request for that window -- an accepted tradeoff for an action this
infrequent and already this disruptive.

The automated test (`tests/sdr_agent_test.sh`) runs the REAL
`config_schema.sh` (its `validate` command has no board dependency), so the
validate-gate and its real error text are exercised end to end. Only
`appliance_start.sh` is faked, since the real one needs `devmem`, real IIO
hardware, and the real bridge/supervisor binaries: the fixture re-sources
whatever `bridge.conf` it's given, writes those values into a fixture IIO
tree (standing in for the AD9363 converging), and starts a process whose
`/proc/*/exe` genuinely resolves to a path containing `/sdr_bridge` -- the
same identity match the real bridge is found by -- so both the successful
apply path and, with a fixture that starts nothing, the verify-timeout ->
rollback path are exercised for real, not mocked. Functional verification
against the real `appliance_start.sh` and real hardware is done on the
board itself.

**A pre-existing gap this surfaced, and closed:** `apply`'s relaunch depends
on `appliance_start.sh` (and `appliance_supervise.sh`, `appliance_status.sh`,
`config_schema.sh`, `provision.sh`) actually being present at `/mnt/jffs2/`
on the board. `release/templates/flash.sh`'s generated `autorun.sh` did not
previously install or invoke any of them -- it started the bridge through an
older, separate mechanism (`bridge_up.sh` with `BRIDGE_LOCAL`/`BRIDGE_PEER`,
variables the current config schema does not even set, so that path was
already dead on a schema-2 config). Earlier tasks' hardware verification
worked only because these scripts were placed on both units by hand outside
the release pipeline. `build-release.sh` now bundles all five under
`software/appliance/` (plus `bridge.conf.example` for reference), and
`flash.sh` installs and `chmod +x`'s them to `/mnt/jffs2/` and starts
`appliance_start.sh` from `autorun.sh` in place of the old mechanism --
`bridge.conf` itself is still deliberately never auto-installed, so a fresh
flash with no `bridge.conf` still forwards nothing, exactly as before.

## Security contract

- Bind to the USB management address, never `0.0.0.0`.
- Generate a unique random token per unit.
- Store the token in a regular file with mode 0600. The service refuses to
  start if group or other permissions are present.
- HTTP is plaintext. Treat the USB management network as trusted and isolated;
  do not route this port onto the RF/user Ethernet or the public Internet.
- The status collector, every control action, and the config validator are
  executed directly with `exec`, never through a shell. Request parameters
  can select which of the five known control actions to run, or supply the
  body of a config candidate, never a command or a path.
- `bridge.conf` is the only file this service ever writes, and only through
  validate -> backup -> atomic replace -> verify -> rollback; nothing is
  overwritten without first passing `config_schema.sh`'s own validation on
  a candidate file, checked before the live one is touched.
- Requests, command runtime, status output, metrics, logs, events, config
  candidates, and socket waits all have fixed bounds.

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

`reset_demod.sh` (`fpga/scripts/reset_demod.sh`) needs no separate wiring: it
is one more `.sh` under `fpga/scripts/`, and `build-release.sh` already
copies every one of those into the release bundle's supporting-tools, which
`flash.sh` already installs whole to `/mnt/jffs2/tools/`. `sdr-agent`'s
default `--demod-reset-script` points there.

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
