# Phase 8 software candidate — frozen baseline

Frozen 2026-09-20. The control plane is feature-complete for
Phase 8. **Accept no further changes except a fix for a reproducible blocker,
or what the 8192-byte FPGA integration requires.** No opportunistic
refactoring: the value of this baseline is that it is the thing that was tested.

## Commit

    c3b4faf  Record the two testing rules, with the failures that earned them

## What is in it

| capability | verified by |
|---|---|
| L2 loop suppression | 18 unit tests + hardware: loopsup 77/76, no escalation over 40 s |
| Fail-safe startup | 6 preflight checks; refuses tx==rx and bad NODE_ID, logs the reason |
| Config schema + atomic writes | 15 negative cases rejected; truncated config leaves a safe non-forwarding state |
| Provisioning from immutable MAC | distinct ids derived on both units; registry rejects a collision |
| Demodulator recovery | fires on DMA-without-frames; does not fire on duplicate traffic |
| Supervisor | crash recovered ≤30 s; 5 restarts then FAULT with forwarding stopped |
| Traffic classes + congestion | 23 assertions; bulk 99 ms / control 9 ms against their budgets |
| Peer handshake | COMPATIBLE on hardware; degrades to STALE when the peer falls silent |
| Observability | bridge publishes JSON; 21 counter paths resolve; progress verdict |
| Phase 8 harness | 27 self-tests; classifies the current bench INVALID, correctly |
| Resilience | 13 checks pass on both units |

## Deployed binaries

    repo build   df174732cd290f95b6f5d5caedd59704
    UNIT-A       50d061457d080eb418b96383baed5214
    UNIT-B       50d061457d080eb418b96383baed5214

## Unit identities

    UNIT-A  serial AKRLJ24FWXM7H65X  mac 00:60:88:3d:32:ae  node 327951062
            tx 434 MHz / rx 444 MHz
    UNIT-B  serial 3SXRLJMXS7EL5IBJ  mac 00:60:88:3e:18:23  node 465429558
            tx 444 MHz / rx 434 MHz
    both    FPGA 0x5344524C v0x00010300 ABI 3 regmap 3, 15.36 MS/s, diff 1

## Measured at the modem layer (NOT product numbers)

These come from one transmitter into one receiver with no Ethernet involved.
They are an upper bound on what the appliance can deliver, not a statement of
what it does deliver, and the product envelope must be measured end to end.

    15.36 MS/s   6.96 Mbit/s payload, PER 0.00%, DMA gap 0 B
     7.68 MS/s   3.46 Mbit/s payload, PER 0.03%
     3.84 MS/s   1.73 Mbit/s payload, PER 0.00%
    17.28 MS/s   fails at 94% PER -- below the documented 17.5 MS/s clock ceiling
    data/video   4 MiB H.264-shaped payload delivered byte-exact, 3496/3496 chunks

## The two external blockers

1. **8192-byte FPGA at 15.36 MS/s.** RAM-load first; verify identity and
   RX_PKT_BYTES=8192; verify acquisition and FDD both ways; confirm the ~8.5 ms
   packetisation interval. At the current 32768 the floor is 34 ms, so the 10 ms
   latency target is unreachable without it.
2. **Separated Ethernet segments.** Endpoint A to UNIT-A's RJ45, endpoint B to
   UNIT-B's RJ45, with no alternate L2 path during Phase 8. The harness refuses
   to benchmark across a shared segment, and is correct to: it measured 178
   frames crossing with UNIT-B's demodulator disabled. Keep the shared-segment
   arrangement only for the dedicated LoopGuard hardware test.

## Then

Run Phase 8 as prepared, and establish the product envelope from it -- usable
Ethernet goodput, control p50/p95/p99/max, video and telemetry coexistence,
loss, queue occupancy, CPU, recoveries, RF margin. Not from the modem numbers
above.
