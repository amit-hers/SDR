#!/usr/bin/env python3
"""Phase 8: end-to-end acceptance of the production data path.

    endpoint_A -- eth -- UNIT-A -- RF -- UNIT-B -- eth -- endpoint_B

Layers run in order and STOP at the first failure. Running a TCP benchmark
after ARP has failed produces numbers that describe nothing and buries the
layer that actually broke.

Three outcomes, deliberately distinct:
    PASS     the appliance met the requirement
    FAIL     the appliance did not
    INVALID  the TEST was not valid; this says nothing about the appliance
"""
from __future__ import annotations
import argparse, json, os, socket, struct, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import (SSH, Verdict, Invalid, sh, status, deltas, distribution,
                     Stimulus, check_preconditions, topology_is_separated,
                     progress_stalled, unexpected_recovery)

ETH_P_ALL = 0x0003
TEST_ET = 0x88B5


class Layer:
    def __init__(self, name): self.name = name; self.verdict = Verdict.SKIP
    def to_json(self, **kw): return dict(layer=self.name, verdict=self.verdict.value, **kw)


def _raw_sock(iface, rx_timeout=3.0):
    s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETH_P_ALL))
    s.bind((iface, 0)); s.settimeout(rx_timeout); return s


def _mac(iface):
    with open(f"/sys/class/net/{iface}/address") as f:
        return bytes.fromhex(f.read().strip().replace(":", ""))


# ── layer: raw Ethernet, both directions, exact counts and payloads ────────
def layer_raw_ethernet(a_if, b_if, n=100):
    res = {}
    for label, tx_if, rx_if in (("A_to_B", a_if, b_if), ("B_to_A", b_if, a_if)):
        src, dst = _mac(tx_if), _mac(rx_if)
        tx, rx = _raw_sock(tx_if), _raw_sock(rx_if, 5.0)
        sent, payloads = 0, {}
        for i in range(n):
            tag = f"P8-{label}-{i:04d}".encode()
            body = tag + bytes((i * 7 + j) & 0xFF for j in range(64))
            payloads[tag] = body
            f = dst + src + struct.pack("!H", TEST_ET) + body
            tx.send(f); sent += 1
        # A test that offered nothing cannot be evaluated.
        if sent == 0:
            raise Invalid(f"{label}: no frames were offered")
        seen, exact, deadline = set(), 0, time.time() + 6.0
        while time.time() < deadline and len(seen) < n:
            try: pkt = rx.recv(4096)
            except socket.timeout: break
            for tag, body in payloads.items():
                if tag in pkt and tag not in seen:
                    seen.add(tag)
                    if pkt[14:14 + len(body)] == body: exact += 1
        res[label] = {"offered": sent, "delivered": len(seen), "byte_exact": exact,
                      "lost": sent - len(seen)}
    ok = all(v["delivered"] == v["offered"] and v["byte_exact"] == v["offered"]
             for v in res.values())
    return (Verdict.PASS if ok else Verdict.FAIL), res


# ── layer: ICMP with a real distribution ───────────────────────────────────
def layer_ping(peer_ip, iface, count=60):
    p = subprocess.run(["ping", "-I", iface, "-c", str(count), "-i", "0.2", "-W", "2", peer_ip],
                       capture_output=True, text=True, timeout=count * 2 + 30)
    rtts = [float(m.split("time=")[1].split()[0]) for m in p.stdout.splitlines() if "time=" in m]
    if not rtts:
        return Verdict.FAIL, {"sent": count, "received": 0, "note": "no replies"}
    loss = (count - len(rtts)) / count * 100.0
    return (Verdict.PASS if loss < 5.0 else Verdict.FAIL), {
        "sent": count, "received": len(rtts), "loss_pct": round(loss, 2),
        "rtt_ms": distribution(rtts)}


# ── layer: finite byte-exact transfer, wall-clock throughput ───────────────
def layer_transfer_bytes(size_mib, seconds, delivered_bytes):
    """Throughput is unique delivered bytes over wall clock. It is NEVER derived
    from a cyclic source's content duration: that reported 10.74 Mbit/s here for
    a link measured at 6.96, a 54% overstatement, because the same content was
    sent twelve times."""
    if seconds <= 0:
        raise Invalid("transfer window was not positive")
    return delivered_bytes * 8 / seconds / 1e6


def run(args) -> dict:
    report = {"config": vars(args), "started": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
              "layers": [], "verdict": None, "reason": None}

    # ── PRECONDITIONS ──────────────────────────────────────────────────────
    pres = check_preconditions(args.unit_a, args.unit_b, args.endpoint_a, args.endpoint_b)
    report["preconditions"] = [p.__dict__ for p in pres]
    blocking_failed = [p for p in pres if p.blocking and not p.ok]
    if blocking_failed:
        report["verdict"] = Verdict.INVALID.value
        report["reason"] = "precondition(s) not met: " + "; ".join(
            f"{p.name} ({p.detail})" for p in blocking_failed)
        return report
    report["limitations"] = [f"{p.name}: {p.detail}" for p in pres if not p.blocking and not p.ok]

    # ── TOPOLOGY ───────────────────────────────────────────────────────────
    if args.check_topology:
        sep, why = topology_is_separated(args.unit_a, args.unit_b)
        report["topology"] = {"separated": sep, "detail": why}
        if not sep:
            report["verdict"] = Verdict.INVALID.value
            report["reason"] = f"topology: {why}"
            return report

    # ── STATUS BEFORE ──────────────────────────────────────────────────────
    try:
        before = {"A": status(args.unit_a), "B": status(args.unit_b)}
    except Invalid as e:
        report["verdict"] = Verdict.INVALID.value; report["reason"] = str(e); return report
    report["status_before"] = before

    layers, verdict, reason = [], Verdict.PASS, None
    try:
        # ── RAW ETHERNET ───────────────────────────────────────────────────
        v, d = layer_raw_ethernet(args.endpoint_a, args.endpoint_b, args.frames)
        layers.append({"layer": "raw_ethernet", "verdict": v.value, "detail": d})
        if v is not Verdict.PASS:
            verdict, reason = v, "raw Ethernet frames did not cross intact"
            raise StopIteration
        # ── ARP / PING ─────────────────────────────────────────────────────
        if args.peer_ip:
            v, d = layer_ping(args.peer_ip, args.endpoint_a, args.ping_count)
            layers.append({"layer": "icmp", "verdict": v.value, "detail": d})
            if v is not Verdict.PASS:
                verdict, reason = v, "ICMP did not meet the loss requirement"
                raise StopIteration
        else:
            layers.append({"layer": "icmp", "verdict": Verdict.SKIP.value,
                           "detail": "no --peer-ip given; an IP endpoint is required"})
        layers.append({"layer": "ipv6", "verdict": Verdict.SKIP.value,
                       "detail": "NOT_IN_PRODUCT_SCOPE until declared"})
        for nm in ("udp_rate_ladder", "tcp_byte_exact", "bidirectional", "ping_under_load",
                   "loopguard_hardware", "soak_30s", "soak_5m"):
            layers.append({"layer": nm, "verdict": Verdict.SKIP.value,
                           "detail": "requires separated segments and real IP endpoints"})
    except StopIteration:
        pass
    except Invalid as e:
        report["verdict"] = Verdict.INVALID.value; report["reason"] = str(e)
        report["layers"] = layers; return report

    # ── STATUS AFTER, DELTAS, LIVENESS, RECOVERY ───────────────────────────
    try:
        after = {"A": status(args.unit_a), "B": status(args.unit_b)}
    except Invalid as e:
        report["verdict"] = Verdict.INVALID.value; report["reason"] = str(e); return report
    report["status_after"] = after
    report["deltas"] = {"A": deltas(before["A"], after["A"]),
                        "B": deltas(before["B"], after["B"])}

    for unit in ("A", "B"):
        stalled, why = progress_stalled(after[unit])
        if stalled:
            verdict, reason = Verdict.FAIL, f"UNIT-{unit} progress stalled: {why}"
        rec = unexpected_recovery(report["deltas"][unit])
        if rec:
            report.setdefault("instability", []).extend([f"UNIT-{unit}: {r}" for r in rec])
            verdict = Verdict.FAIL
            reason = reason or ("recovery occurred during the run: " + "; ".join(rec))
        # A duplicate identity shows up here and nowhere else.
        sd = report["deltas"][unit].get("rx.self_discarded", {}).get("delta")
        if isinstance(sd, int) and sd > 0:
            verdict = Verdict.FAIL
            reason = reason or (f"UNIT-{unit} discarded {sd} frames as self-reception during "
                                "the run: check node identity")

    report["layers"] = layers
    report["verdict"] = verdict.value
    report["reason"] = reason or "all executed layers passed"
    return report


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--unit-a", default="192.168.2.17")
    ap.add_argument("--unit-b", default="192.168.2.1")
    ap.add_argument("--endpoint-a", required=True, help="host NIC on UNIT-A's Ethernet side")
    ap.add_argument("--endpoint-b", required=True, help="host NIC on UNIT-B's Ethernet side")
    ap.add_argument("--peer-ip", default=None, help="IP of the endpoint on side B")
    ap.add_argument("--frames", type=int, default=100)
    ap.add_argument("--ping-count", type=int, default=60)
    ap.add_argument("--check-topology", action="store_true", default=True)
    ap.add_argument("--no-check-topology", dest="check_topology", action="store_false")
    ap.add_argument("--json-out", default="/tmp/phase8_result.json")
    a = ap.parse_args()
    if os.geteuid() != 0:
        sys.exit("must run as root (raw sockets)")

    rep = run(a)
    with open(a.json_out, "w") as f:
        json.dump(rep, f, indent=2, default=str)

    print(f"\n=== PHASE 8: {rep['verdict']} ===")
    print(f"reason: {rep['reason']}")
    for p in rep.get("preconditions", []):
        if not p["ok"]:
            print(f"  precondition {'BLOCKING' if p['blocking'] else 'note'}: "
                  f"{p['name']} -- {p['detail']}")
    for l in rep.get("layers", []):
        print(f"  {l['layer']:22} {l['verdict']}")
    for i in rep.get("instability", []):
        print(f"  instability: {i}")
    print(f"\nfull result: {a.json_out}")
    sys.exit(0 if rep["verdict"] == "PASS" else 1)


if __name__ == "__main__":
    main()
