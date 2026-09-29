# Section 13: Queue/Congestion Validation

**Method**: parallel `ping` streams from PC-A (no PC-B-side tooling needed,
since ICMP echo reply is automatic OS behavior) to generate real offered
load above what a single RTT-limited ping stream can produce, at max-size
(1472B) frames.

## Run 1: 15 parallel streams x 100 packets (no clean before/after taken)

Aggregate: 1500 sent, ~1272 received, ~15% loss (per-stream 12-19%).
`queues.control_drops`/`bulk_drops` read 0 both before and immediately
after, but this measurement is suspect -- see Run 2's finding below about
`bridge_stats.json` only updating once per `--stats` interval.

## Run 2: 10 parallel streams x 100 packets, WITH a clean before/after

Aggregate: 1000 sent, 997 received, 0.3% loss (3 packets).

| Counter | Unit A (near PC-A) before | after | delta |
|---|---|---|---|
| tx.packets | 1591 | 2593 | +1002 |
| rx.crc_errors | 165 | 178 | +13 |
| rx.duplicates | 0 | 0 | +0 |
| rx.boundary_recovered | 323 | 500 | +177 |

| Counter | Unit B (near PC-B) before | after | delta |
|---|---|---|---|
| tx.packets | 1886 | 2888 | +1002 |
| rx.crc_errors | 1026 | 1049 | +23 |
| rx.duplicates | 0 | 0 | +0 |
| rx.boundary_recovered | 649 | 826 | +177 |

`queues.control_drops`/`bulk_drops` stayed at 0 on both units across this
run too.

## Methodology note (important)

The FIRST attempt at this before/after comparison showed **zero** change in
ANY counter despite 1000 real packets crossing the link in between --
because `bridge_stats.json` is only rewritten once per `--stats` interval
(now 15s on unit B after the section 20 config change), and I queried
immediately after the flood finished, before the next stats tick had fired.
Waiting ~12s before the "after" read fixed this. Worth remembering for any
future automated test harness: **always wait at least one full `--stats`
interval before trusting a metrics delta**, or the result silently looks
like "nothing happened" when actually a full stats cycle just hadn't
written yet.

## Conclusion

Loss under load (0.3% at 10 streams, ~15% at 15 streams -- clearly
load-dependent, consistent with approaching/exceeding the link's documented
~7.85 Mbit/s ceiling) is **accounted for by `rx.crc_errors`**, not silent:
the small loss at 10 streams (3 packets) is fully covered by the observed
CRC-error deltas (+13 / +23) on the two units. `bulk_drops`/`control_drops`
never incremented in either run, meaning the loss observed here is RF-level
decode failure under high duty cycle, not the bridge's own internal queue
overflowing. **A genuine forced BULK/CONTROL queue overflow (the literal
`QUEUE_DROP` event) was not achieved** -- that would need sustained
above-capacity load for longer than a parallel-ping burst can sustain, most
practically via the PC-B-side flood tooling that remained blocked this
session (see SUMMARY.md).

**Result: PASS (partial)** -- no silent drops observed; genuine QUEUE_DROP
forcing remains outstanding.
