# Task 15 short soak (scaled-down)

**Duration**: ~5.3 minutes (2026-09-26 17:15:47 to ~17:21). NOT a
production-length soak -- see the caveat below.

**Load**: sustained 512B ICMP payload at 0.2s interval (~5 pkt/s, ~20kbit/s)
from PC-A to PC-B, continuous, while the active health probe and control
(HELLO) traffic ran normally alongside it (this counts as genuine "mixed
traffic": payload + control + probe simultaneously, per task 15's own
wording, just not at a saturating rate).

## Result

- **1589/1591 ICMP replies received (0.13% loss)**, 0 duplicates.
- **Zero events** logged on unit A during the window (`/api/v1/events`
  returned `{"events":[]}` immediately after) -- no RF_LOSS, no
  DEMOD_RECOVERY, no faults of any kind.
- **Zero recoveries** triggered on either unit (both `recoveries` counters
  unchanged before/after).
- CRC errors climbed by 251 (unit A) / 211 (unit B) over the window --
  consistent with steady-state RF noise floor accumulation already observed
  throughout this session's idle/light-load periods, not a load-induced
  spike.
- jffs2 free space and memory on both units: see `soak_A_sysinfo_before.txt`
  / `soak_B_sysinfo_before.txt` (not re-checked after -- a 5-minute window
  at this data rate cannot meaningfully move flash usage; a real soak would
  need before/after here too).

## Caveat: this is NOT the production-length soak task 15 asks for

Five minutes is a smoke test, not a soak. It confirms the appliance holds
up cleanly under sustained mixed traffic for a short window with zero
anomalies, which is a genuine data point, but a real production soak (the
plan's own wording: "meaningful long-duration interval," tracking JFFS2
free space drift, memory drift, and counter discontinuities over hours) is
still outstanding and needs a dedicated multi-hour (or longer) session.
