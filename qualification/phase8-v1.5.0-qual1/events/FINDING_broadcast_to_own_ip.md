# Finding: broadcast ARP to a unit's own locally-configured IP doesn't cross the bridge

**Date**: 2026-09-26, Phase 8 qualification, candidate v1.5.0-qual1.

## Observation

- Unit A (mgmt 192.168.2.17) has `192.1.1.2/24` configured directly on its
  `eth0` (confirmed via `ip addr show eth0` over SSH) -- a leftover/manual
  diagnostic address, unrelated to the appliance software.
- PC-A (192.1.1.3) cannot ARP-resolve 192.1.1.2 (`ip neigh` shows `FAILED`).
- Unit A's own kernel ARP table (`ip neigh show` on the unit itself) has
  **no entry at all for PC-A** -- it has never seen an ARP request
  originating from 192.1.1.3.
- By contrast, PC-A **does** correctly ARP-resolve PC-B (192.1.1.10, on
  unit A's same local segment) to PC-B's real MAC, and unicast IP traffic
  (ping, all 5 raw-Ethernet frame sizes, DF-set full-size ping) between
  PC-A and PC-B crosses the RF link cleanly and repeatedly in this session.
- Unit A pinging PC-B locally (not crossing RF) works normally
  (0.5-0.8ms), confirming 192.1.1.2 itself is a normal, working local
  address -- the issue is specifically inbound broadcast frames destined
  for it from across the RF link.

## What this means and doesn't mean

This does **not** indicate the bridge is broken for its actual job: every
PC-A<->PC-B unicast test in this qualification run passed. The anomaly is
narrower and specific: a broadcast ARP request that crosses the RF link
successfully when it's *for a third-party host on the far segment* (PC-B)
appears to be dropped when it's *for the unit's own locally-configured IP*.
That distinction (self vs. third-party as the ARP target) is exactly the
kind of thing `LoopGuard`'s self-traffic suppression
(`to_self`/`from_self`/`local_copies`, `include/sdr/bridge/LoopGuard.hpp`)
is designed to reason about, making it a plausible place to look, though
this was not confirmed by packet capture.

## Why this wasn't root-caused further

No `tcpdump`/packet-capture permission on PC-A (no sudo in this
environment) and no `arping` available, so it wasn't possible to directly
observe whether the ARP request ever left PC-A's NIC, whether unit B
received and attempted to relay it, or whether unit A received it but its
kernel silently failed to reply. Root-causing this properly needs packet
capture at at least one hop (ideally on unit A's `eth0` itself, over SSH,
where `tcpdump` may already be available with root -- not attempted this
session for time reasons).

## Relevance to the qualification plan

Section 14 (LoopGuard Validation) asks for broadcast behavior to be
exercised specifically; this finding surfaced from an unrelated
investigation (a user question about ARP reachability) but belongs there.
Recommend a follow-up session with packet capture on the units themselves
(SSH + local tcpdump, avoiding the need for capture permission on the host
PCs) to confirm whether this is LoopGuard-related or something else in the
raw-eth AF_PACKET relay path.
