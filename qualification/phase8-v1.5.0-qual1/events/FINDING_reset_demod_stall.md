# Finding: reset_demod left the demodulator stalled (0% decode activity)

**Date**: 2026-09-26, Phase 8 qualification run against candidate v1.5.0-qual1
(commit 2249997), unit at 192.168.2.17 (serial AKRLJ24FWXM7H65X, node_id
327951062), acting as the appliance behind PC-B (192.1.1.10).

## Sequence

1. Baseline: A<->B link healthy, peer COMPATIBLE both directions, DF-set
   1472-byte ping sessions passed 10/10 both directions (0% loss).
2. `POST /api/v1/control/reset_demod?confirm=yes` issued against unit A,
   as part of Phase 8 section 7's "reacquire/restart demod between some
   runs" requirement. Response: `{"action":"reset_demod","ok":true,
   "output":"demod reset pulsed: lock=0x00000004 mu_clamped=0x00000002\n"}`
   -- the script itself reported success and a non-zero lock register.
3. Within seconds, PC-A -> PC-B DF-set ping: 100% loss (0/10). Raw ICMP
   (no DF): also 100% loss. SSH to PC-B: "No route to host" (not just an
   SSH-level hang -- L3 reachability was gone).
4. Unit A's own diagnostics at that point:
   - `/api/v1/peer`: `{"compatibility":"STALE", ...}`
   - `/api/v1/metrics`: `"cpu_decode_pct": 0.0`, `"recoveries": 0`,
     `crc_errors` unchanged from before the reset (530, not climbing),
     `rx.frames` barely advancing.
   - RSSI reported normal (65.75 dB) -- not an RF power/antenna issue.
5. Recovery attempted via `POST /api/v1/control/restart_bridge?confirm=yes`
   (killed 2 processes, supervisor present). This DID recover it: within
   ~20s, `cpu_decode_pct` returned to 52.0%, peer went back to COMPATIBLE,
   and PC-A -> PC-B ping returned to 0% loss (RTT 166-340ms, elevated/noisy
   right after reacquisition but functional). So `restart_bridge` is a
   working remedy for this specific stall; `reset_demod` alone was not
   enough and did not self-correct on its own within the ~1-2 minutes this
   was observed before restart_bridge was issued.

## Why this matters

`cpu_decode_pct: 0.0` is the key signal: the bridge's decode loop was not
merely receiving a degraded/noisy signal (which would show elevated
`crc_errors` climbing, or `recoveries` incrementing from the bridge's own
automatic recovery logic) -- it looks like it stopped doing decode work
entirely. That is a different failure mode than "RF got worse": it suggests
`reset_demod.sh`'s direct register pulse (no separate drain process, per
its own design -- see `fpga/scripts/reset_demod.sh` and
[[sdr-agent-control-api]]) can, at least sometimes, leave the demodulator in
a state the bridge's own automatic recovery logic does not detect or does
not recover from on its own -- `recoveries` stayed at 0, meaning the
bridge's own stall-detection-and-recover path never even fired.

This is the FIRST time `reset_demod` has been exercised against a live,
actively-forwarding link on real hardware (task 10's hardware verification
was previously marked pending in memory). The action's own design intent
was: the running bridge's own `iio_readdev` already drains the RX DMA the
reset needs, so no separate drain process is required. This incident
suggests that assumption may not always hold, or that the pulse can race
something in the bridge's own receive state, and needs further
investigation before `reset_demod` is trusted as an unattended recovery
action rather than one that itself sometimes requires a follow-up
`restart_bridge`.

## Open questions for follow-up (not resolved in this session)

- Is this reproducible, or a one-off? (single occurrence so far)
- Does it correlate with the bridge being under active peer traffic at the
  moment of the pulse (as opposed to reset_demod's original bring-up-only
  validation, which was on an idle/just-started link)?
- Should `reset_demod`'s response include a post-pulse decode-activity
  check (poll `cpu_decode_pct` / a frames-advancing check for a bounded
  window) before reporting `"ok":true`, rather than trusting the script's
  own immediate register readback alone?
