# Configuration Freeze — Candidate v1.5.0-qual1

This document freezes the supported operating envelope for the
`pluto-datalink` appliance as validated by Phase 8 qualification (task 14)
and the production qualification pass (task 15), both performed against
real hardware, real RF, and physically separated Ethernet segments.

**Candidate**: commit `2249997`, release tag `v1.5.0-qual1`. FPGA identity:
magic `0x5344524C`, ABI `3`, regmap `4`, version `0x00010300`, board
`XC7Z020` (Pluto+; every on-device string claims Z7010 -- do not trust it,
read `PSS_IDCODE`).

## Frozen bridge.conf envelope

These are the values this candidate was actually run against, end to end,
on real hardware. Deviating from any of them is **outside the validated
envelope** until a fresh qualification pass covers the new value.

| Key | Frozen value | Confidence |
|---|---|---|
| `CONFIG_VERSION` | 2 | Validated -- schema enforced |
| `MODE` | `raw-eth` | Validated -- transparent L2, both units |
| `SAMPLE_RATE` | 15360000 (15.36 MS/s) | Validated |
| `DIFF_MODE` | 1 (differential QPSK) | Validated |
| `TX_RF_BANDWIDTH` / `RX_RF_BANDWIDTH` | 4000000 (4 MHz) | Validated |
| `TX_ATTENUATION_DB` | 0 | Validated (lab-distance link; not a statement about maximum range) |
| `RX_GAIN_MODE` | `slow_attack` | Validated |
| FPGA packet size (`RX_PKT_BYTES`) | 8192 | Validated -- the specific candidate this whole roadmap item exists to qualify |
| `PROBE_INTERVAL_S` | 5 (bridge default; explicit in bridge.conf optional) | Validated |
| `STATS_S` | 5-15 (tested at multiple values, all worked) | Validated |
| Frequencies | Any crossed pair within 325 MHz-3.8 GHz (schema-enforced); this run used 434/444 MHz | Validated at 434/444 MHz specifically; the range itself is the AD9363's own documented limit, not independently re-verified end-to-end at other points in it |

## Frozen traffic envelope

| Property | Frozen result |
|---|---|
| Raw Ethernet frame sizes | 64B, 128B, 512B, 1000B, 1514B -- all PASS, both directions, 0% loss |
| ARP | Resolves real MACs across the RF bridge, both directions |
| ICMP, including DF-set full-size | PASS, both directions, multiple sessions |
| UDP goodput | 2-6 Mbit/s offered: 1.7-2.5% loss. 8 Mbit/s: inconclusive (measurement-harness artifact, not a confirmed link result -- see task 14 SUMMARY.md). One direction only (A->B); B->A blocked by PC-B tooling. |
| TCP | **Not validated this pass** -- blocked entirely (see Known Gaps). |
| End-to-end RTT (ICMP) | ~155-345ms typical (~160ms floor matches the previously-documented queueing-latency measurement; upper end seen briefly right after a reacquisition/restart, not sustained) |
| Active health probe RTT | ~155-180ms typical (echo through the real RF path, no clock sync needed -- see `sdr-bridge-health-probe` memory) |
| Congestion behavior | Loss under load is load-dependent (15% at ~8.5 Mbit/s offered parallel load, 0.3% at a more moderate parallel load) and is accounted for by `rx.crc_errors`, not silent drops; `bulk_drops`/`control_drops` were not observed to increment in this pass. A genuine forced queue overflow (`QUEUE_DROP` firing) was **not achieved** -- see Known Gaps. |
| DMA boundary / large-frame recovery | `boundary_recovered` increments continuously under load with delivery correctness and boundedness intact -- accepted per the plan's own criterion ("acceptable for this candidate only if delivery remains correct and bounded"). |

## Frozen recovery envelope

| Fault | Recovery path | Frozen result |
|---|---|---|
| RF interruption | Automatic (RF_LOSS -> RF_LOSS_CLEARED on restore) | PASS -- ~129s detection (matches the 120s threshold), fully automatic resumption, no manual intervention |
| Demod stall/reset | `reset_demod` control action | **NOT SELF-SUFFICIENT** -- reproduced 2-for-2 (once under active peer traffic, once fully isolated with no peer at all), so this is not a live-traffic-specific race: `reset_demod` leaves `cpu_decode_pct` at 0% despite reporting `"ok":true` every time it has been tried on real hardware so far. `restart_bridge` is the confirmed-working recovery; always follow `reset_demod` with it and verify decode activity resumed rather than trusting the action's own response. See Known Gaps. |
| Bridge crash/hang | Supervisor auto-restart, or `restart_bridge` control action | PASS |
| Full appliance restart | `restart_appliance` control action | PASS -- ~42-46s to full recovery across two independent trials, both units |
| Config change | `POST /api/v1/config` (validate -> backup -> atomic replace -> apply -> verify -> rollback) | PASS for the apply-success and reject-invalid paths; the verify-failure -> rollback path is covered by automated fixture tests (not re-forced on live hardware this pass -- see Known Gaps) |
| Safe mode | `enter_safe_mode` control action | PASS -- forwarding stops, management API and diagnostics stay reachable, clean restore |
| Power cycle (clean, appliance-initiated) | `restart_appliance`, or an external power cycle | PASS -- 2 independent trials this session (one operator-initiated, one deliberate) |
| Power cycle (hard/uncontrolled cut, brownout) | N/A | **NOT TESTED** -- see Known Gaps, needs physical action |

## Known gaps (do not treat as qualified until closed)

1. **`reset_demod` reliably stalls decode activity (`cpu_decode_pct` -> 0%)
   despite reporting success.** Reproduced twice: once under active peer
   traffic (2026-09-26), once fully isolated with no peer connected at all
   (2026-09-29) -- ruling out "only under live traffic" as the cause.
   `restart_bridge` recovers it both times. Until root-caused in
   `fpga/scripts/reset_demod.sh`/`sdr_agent.c`, this action must never be
   used unattended: always follow it with `restart_bridge` and confirm
   `cpu_decode_pct` actually recovered.
2. **TCP is entirely unvalidated** in this qualification pass. No claim is
   made about TCP behavior over this link.
3. **A genuine forced `QUEUE_DROP` was not produced.** The queue-drop path
   itself was exercised in earlier unit/integration testing (not this
   hardware pass), but end-to-end confirmation on real hardware under
   real saturating load is outstanding.
4. **No soak test of production-representative duration** has been run
   against this candidate. A short (~5.3 minute) scaled-down soak under
   sustained mixed traffic (payload + control + probe simultaneously) DID
   run clean: 0.13% loss, zero events, zero recoveries (`soak/SOAK_RESULT.md`)
   -- a genuine, if brief, positive data point, not a substitute for a real
   multi-hour soak.
5. **No hard-power-cut or brownout test** has been performed. This needs a
   real, physical, uncontrolled power interruption -- something this
   session could not do remotely. Recommended before this envelope is
   trusted for a deployment where power quality cannot be guaranteed.
6. **Broadcast-to-self-IP anomaly** (task 14 finding) is unexplained;
   confirmed to not affect the appliance's actual forwarding job, but not
   root-caused (no packet capture access in this environment).
7. Validated only at the ONE frequency pair (434/444 MHz) actually used;
   the broader 325 MHz-3.8 GHz range is the AD9363's own datasheet limit,
   not independently re-confirmed end-to-end at other points in it.
8. `flash.sh`'s runtime (non-`--persist`) deployment path crashes a running
   `sdr-agent` by overwriting its binary in place (task 14 finding, fixed
   manually each time, not yet fixed in `flash.sh` itself). Any future
   deployment via this path needs the same manual `sdr-agent` restart this
   session performed, until `flash.sh` itself is fixed.

## What "frozen" means here

This is a **candidate qualification snapshot**, not a promise that every
value in it is the only one that could ever work -- it is the specific,
concrete envelope this candidate was actually run against and found to
behave correctly within, with the gaps above named explicitly rather than
silently assumed passing. Changing any frozen value, or attempting to rely
on any of the "not tested" items above, should be treated as leaving the
qualified envelope until a fresh pass covers it.
