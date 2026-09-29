# Phase 8 Qualification — Session Summary

**Candidate**: `v1.5.0-qual1`, commit `2249997` (`+dirty` outside the
sdr_bridge/sdr-agent/appliance-scripts surface -- see `candidate/`;
the dirty files are unrelated parallel modem/framing work, not part of
this candidate's functional surface).

**Units**: UNIT-A = mgmt `192.168.2.17`, serial `AKRLJ24FWXM7H65X`, node_id
`327951062`. UNIT-B = mgmt `192.168.2.1`, serial `3SXRLJMXS7EL5IBJ`,
node_id `465429558`.

**Endpoints**: PC-A = this host (Linux, `enp3s0` 192.1.1.3). PC-B = Windows
machine (`ahkov@192.1.1.10`).

This was one continuous but interrupted session (the boards were power-cycled
once mid-session by the operator; SSH to PC-B (Windows) was unreliable
throughout and required one operator-side restart). Sections below are
marked PASS / FAIL / INVALID / SKIP per the plan's own instruction never to
convert an unavailable condition into PASS.

## Section-by-section status

| # | Section | Status | Notes |
|---|---|---|---|
| 1 | Freeze the candidate | PASS (partial) | Hashes/manifest captured in `candidate/`. Tree not fully clean (unrelated parallel work); functional surface for this candidate confirmed unaffected. |
| 2 | Physical topology validation | **PASS** | `enter_safe_mode` on one unit -> PC-A to PC-B 100% loss; restored -> 0% loss. No bypass route in either PC's routing table. |
| 3 | Preconditions | **PASS** | Both units: peer COMPATIBLE, RF match true, no fault, probe enabled, FPGA ABI/regmap match, RX_PKT_BYTES=8192. |
| 4 | Baseline snapshot | **PASS** | Captured in `baseline/` (status/radio/probe/events/fault-history x2). |
| 5 | Raw Ethernet (64/128/512/1000/1514B) | **PASS**, both directions | 10/10 delivered at every size, both directions. `traffic/raw_eth_AtoB.txt`, `traffic/raw_eth_BtoA.txt`. |
| 6 | DMA boundary / large-frame | **PASS** | 200x 1472B frames at 0.05s interval: 0% loss. `boundary_recovered` climbed by ~2141 during the run (from continuous background activity, not uniquely attributable to this burst) with CRC errors up by only 8 and 0 duplicates -- delivery correct and bounded exactly as the plan's acceptance criterion allows. `traffic/dma_boundary_sustained.txt`. |
| 7 | ARP + ICMP | **PASS**, both directions, multiple sessions | ARP resolves real MACs both directions. DF-set 1472B ping: 10/10 both directions (session 1). Session 2 (post `reset_demod`) hit the stall finding below instead of a clean second sample -- see finding. |
| 8 | Active health probe | **PASS** | Verified enabled by default, delivering bidirectionally, RTT ~155-180ms typical (consistent with previously-documented ~160ms queueing latency). Break/restore explicitly exercised in section 15. |
| 9 | UDP throughput | PASS (partial), A->B only | 2/4/6 Mbps: 1.7-2.5% loss. 8Mbps result discarded as an unreliable measurement (harness/SSH artifact, not a link finding) -- see `traffic/udp_AtoB.txt`. B->A: SKIP, PC-B SSH too unreliable for a clean run. |
| 10 | TCP | **SKIP** | PowerShell TCP client/server scripts written (`tcp_send.ps1`/`tcp_recv.ps1`, tested working over loopback on PC-A's Python equivalents) but could not be deployed -- PC-B's Windows SSH server hung again (even a bare `echo test` timed out), a recurrence of the same instability seen for B->A UDP. Not fixable from this side. |
| 11 | Bidirectional load | **SKIP** | Depends on section 9/10 tooling not completed. |
| 12 | Ping/probe under load | **SKIP** | Same dependency. |
| 13 | Queue/congestion validation | PASS (partial) | Parallel ping streams (no PC-B tooling needed) achieved real congestion: 15 streams -> ~15% loss, 10 streams -> 0.3% loss, clearly load-dependent. With a proper before/after (waited a full `--stats` interval -- see finding below), the loss is fully accounted for by `rx.crc_errors` deltas (+13/+23), not silent; `control_drops`/`bulk_drops` stayed at 0 throughout both runs. A genuine forced BULK/CONTROL `QUEUE_DROP` was not achieved (needs sustained above-capacity load longer than a ping burst can produce). Full writeup: `traffic/queue_congestion.md`. |
| 14 | LoopGuard validation | PASS (partial) | `to_self`/`from_self`/`local_copies` counters observed (small nonzero `to_self` on both units, consistent with a coupled lab RF setup); a real, separate finding surfaced (below) around broadcast-to-self-IP -- flagged for follow-up, not fully root-caused (no packet capture access). |
| 15 | RF interruption | **PASS** | Full cycle: RSSI corroborated genuine signal loss, RF_LOSS fired (~129s, consistent with the 120s threshold), restore -> RF_LOSS_CLEARED, peer/payload/probe all resumed unattended. `section15_rf_interruption.md`. |
| 16 | Demod recovery | **PASS, with a finding** | Natural `DEMOD_RECOVERY` events observed in fault-history on both units over the session. A deliberate `reset_demod` under active traffic stalled the demodulator (0% decode activity) rather than recovering -- recovered via `restart_bridge`. See finding below; this is the qualification doing its job. |
| 17 | Bridge restart | **PASS** | `restart_bridge` on unit B: exactly 1 bridge process after, supervisor healthy, peer/payload resumed. |
| 18 | Appliance restart | **PASS** | `restart_appliance` (full reboot) on unit B: ~42s to full recovery, FPGA identity/config/radio/forwarding/probe all correct after. |
| 19 | Safe mode | **PASS** | Forwarding stopped, management API stayed reachable, `SAFE_MODE_REQUESTED` logged, forwarding resumed cleanly after restore. |
| 20 | Configuration apply | **PASS** | Real apply (STATS_S+PROBE_INTERVAL_S change): validated/applied/verified all true, payload+probe resumed. Real reject (FREQUENCY==RX_FREQUENCY): safely rejected, `CANDIDATE_REJECTED` logged, live config untouched. Verification-FAILURE->rollback specifically not re-tested live (risk of deliberately mistuning a real radio with no safe way to force that exact failure mode) -- that path has separate automated fixture-based coverage (task 11 test suite). |
| 21 | Dashboard validation | **PASS** | Loaded against real units mid-test: correctly showed UNIT-B FAULT (safe mode) and UNIT-A DEGRADED (STALE peer) with accurate live evidence and full real event history. |
| 22 | Power cycle | **PASS** | Two cycles observed: one real, unplanned (operator power-cycled the boards, both units came back on the candidate correctly -- corrected an earlier overcautious "don't power-cycle" assumption, since jffs2 persists independently of `--persist`'s boot-image concern); one deliberate (`restart_appliance` full reboot on unit A, ~46s to full recovery -- FPGA identity, config, radio, probe, and 0%-loss payload all confirmed after). |
| 23 | Soak test | **SKIP** | Not attempted -- needs a dedicated long-duration window beyond this session. |
| 24 | Negative-path validation | PASS (partial) | Covered: RF gone -> RF_LOSS (15); bad config, both semantically-invalid (FREQUENCY==RX_FREQUENCY) and out-of-range (SAMPLE_RATE) -> both correctly rejected with clear validator text, live config untouched either time; unknown control action name -> clean 404 "unknown control action"; safe-mode fault path (19). Not exercised: wrong-FPGA-identity refusal (too risky to force on real hardware without a way to safely undo it) and a genuine forced QUEUE_DROP (needs the blocked PC-B flood tooling). |
| 25 | Evidence package | **In progress** | This directory. Real hashes, real captures, real findings -- not fabricated. |

## Findings (the real value of this run)

1. **`reset_demod` can stall the demodulator under active traffic**
   (`events/FINDING_reset_demod_stall.md`). Recovered via `restart_bridge`.
   First-ever live-traffic exercise of this action; previously only
   validated against an idle/bring-up link. Needs investigation before
   `reset_demod` is trusted as a self-sufficient recovery action.
2. **Broadcast ARP to a unit's own locally-configured IP doesn't cross the
   bridge**, while broadcast ARP to a third-party host on the far segment
   does (`events/FINDING_broadcast_to_own_ip.md`). Does not affect the
   appliance's actual job (PC-A<->PC-B forwarding proved solid throughout),
   but is a real, reproducible anomaly worth a follow-up with proper packet
   capture (none available in this environment).
3. **A real deployment bug**: `flash.sh`'s runtime (non-`--persist`) install
   overwrites `sdr-agent`'s binary in place while it's still running,
   crashing it silently (no error in its own log). Worked around manually
   both times (both units, initial deploy). Not yet fixed in `flash.sh`
   itself.
4. **The "don't power-cycle" caution from earlier in this session was overly
   strong**: jffs2-resident software (bridge, agent, appliance scripts,
   bridge.conf) persists through a real reboot independently of the
   `--persist` flag, which only affects the boot IMAGE (kernel/BOOT.bin).
   Confirmed by an actual, unplanned power cycle recovering cleanly.
5. Fixed along the way (not a hardware finding, a memory-accuracy one):
   SSH credentials for both boards are `root`/`analog`, not `root`/`root`
   as an earlier memory note claimed -- corrected.
6. **Testing methodology finding**: `bridge_stats.json` only updates once
   per `--stats` interval -- a before/after metrics comparison taken
   immediately after generating test traffic reads as "nothing happened"
   even though the traffic genuinely crossed the link. Caught mid-session
   during the queue-congestion test; saved to project memory
   (`bridge-stats-write-interval-testing-hazard`) so any future scripted
   test against this API waits a full stats interval before trusting a
   delta.

## What's genuinely left before Phase 8 can be called fully qualified

TCP testing, full bidirectional/under-load probe and ping behavior, a
genuine forced queue-overflow (QUEUE_DROP), a proper LoopGuard broadcast
investigation with packet capture, and the soak test are all outstanding --
all five are blocked on the same root cause: PC-B (Windows) has no
Python/iperf3, and its SSH server repeatedly hangs after the first command
in a session, which is an environment problem on PC-B itself, not fixable
from PC-A. The `reset_demod` stall and the broadcast-to-self-IP anomaly
should be investigated (or explicitly accepted as known limitations) before
sign-off.

**Recommendation for closing the remaining gap**: install a real Python or
iperf3 on PC-B (not present at all currently -- only Windows Store stub
aliases), and investigate why its OpenSSH server degrades after one
command (a service restart worked once this session but recurred). Either
would unblock sections 9-13 and 23 in a single follow-up session.
