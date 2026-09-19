# L2 loop suppression: hardware acceptance

**PASS.** Two appliances on one switch segment, both bridges running and
forwarding, do not replicate uncontrollably.

## Conditions

Both units on the golden image with identical bridge binaries, FDD (A tx 434 /
rx 444, B tx 444 / rx 434), distinct node ids, 15.36 MS/s, both RJ45 ports on
one switch together with the development host -- the loop topology. 25 broadcast
frames injected from the host.

## Result

| | UNIT-A | UNIT-B |
|---|---|---|
| eth0 rx | 204 -> 229 (+25) | 204 -> 229 (+25) |
| eth0 tx | 53, unchanged | 54, unchanged |
| **loop suppressions** | **77** | **76** |
| delivered before suppression | 4404 B | 4486 B |
| escalation over 40 s | none | none |

The guard is demonstrably doing the work -- 77 and 76 frames dropped as echoes
returning over Ethernet -- and the counters are flat from t+3 s to t+40 s.
Replication is bounded at the frames that crossed before each unit learned the
source, which is the designed behaviour rather than an absence of traffic.

**This is the first run in which the test could mean anything.** Two earlier
attempts also produced no storm, but with `loopsup 0` and zero bytes delivered:
nothing crossed the link, so there was nothing to suppress. They were recorded
as INCONCLUSIVE at the time and should stay that way.

## A consequence of this topology, not a defect

On this bench the host sits on the same segment as both units, so a frame it
sends reaches each unit directly AND arrives again from the peer over RF. The
guard learns the host's address as radio-side and then suppresses the host's own
later frames. That is why `eth0 tx` stops advancing here.

In the product topology -- a host behind each unit on separate segments -- a
station's address arrives from one side only, and this does not occur. The
behaviour on the bench is correct for what it is being told: in a topology where
a legitimate frame and an echo are genuinely indistinguishable, suppressing is
the safe choice, because the alternative is a broadcast storm on the customer's
network.

It does mean **this bench arrangement cannot also serve as the Phase 8 Ethernet
test**, which needs the two ports on separate segments regardless.

## What is still not tested

Sustained load. 25 frames prove the mechanism; they do not prove it holds up
with a video stream running, where the learning table is exercised continuously
and entries age out under traffic.
