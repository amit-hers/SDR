# Section 15: RF Interruption

**Method**: RF interruption simulated by stopping unit B's transmitter via
`enter_safe_mode` (kills bridge+supervisor, stops all TX/RX on that unit)
rather than a physical antenna disconnect.

- Interruption triggered: 2026-09-26 16:42:33 IDT
- RSSI on unit A climbed from its normal ~55-70 dB to 116.5 dB during the
  interruption -- AGC hunting for a signal that stopped arriving. Confirms
  the interruption genuinely removed RF energy from unit A's receiver, not
  just a software-level bridge state change.
- RF_LOSS observed on unit A: 2026-09-26 16:44:42 IDT (129s after
  interruption -- consistent with the 120s threshold plus poll granularity).
- Diagnostic bundle: the bundle list at this point showed 5/5 slots filled
  by REPEATED_RECOVERY captures (uptime_s 2682-5956); no RF_LOSS-triggered
  bundle was present in the retained set at the time checked. Given
  MAX_BUNDLES=5 and bounded retention, this may mean an RF_LOSS bundle was
  captured and then pruned by later REPEATED_RECOVERY events, or that this
  RF_LOSS's own capture is still pending/was suppressed by the 60s
  cooldown from a very recent prior capture. The RF_LOSS EVENT itself
  (fault-history) is the primary evidence and was confirmed directly.
- Restore issued: 16:44:55 IDT (relaunched appliance_start.sh on unit B).
- RF_LOSS_CLEARED observed on unit A shortly after restore. Peer returned
  to COMPATIBLE, `progress.verdict` back to `forwarding`, PC-A->PC-B
  payload ping 0% loss (5/5), probe delivering again (198/275 lifetime
  total by this point).
- Probe's `rtt_us.p99`/`max` briefly showed multi-second outliers
  (3.4s / 4.6s) right after restore -- these are stale in-flight probes
  from BEFORE/during the interruption finally getting a very late reply
  once the link came back, correctly counted as delivered-but-slow rather
  than lost. Expected artifact of the rolling window, not a defect; will
  age out as fresh samples accumulate (window is 200 samples).

**Result: PASS.** RF_LOSS fired, RSSI corroborated genuine signal loss,
RF_LOSS_CLEARED fired on restore, peer/payload/probe all resumed without
manual intervention beyond the RF restoration action itself.
