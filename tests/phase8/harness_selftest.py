#!/usr/bin/env python3
"""Test the harness itself, by giving it conditions that must NOT pass.

This exists because the measurement system has repeatedly been the thing that
was wrong: fixtures that compared a unit with itself, a capture that outran its
source, a parser that matched a substring. A harness that cannot detect its own
invalid inputs will certify them.
"""
import os, sys, json, copy
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import harness as H

fails = 0
def ck(ok, what):
    global fails
    print(f"  {what:<62} {'PASS' if ok else 'FAIL'}")
    if not ok: fails += 1

GOOD_A = {
  "unit": {"serial": "A", "mac": "00:60:88:3d:32:ae", "uptime_s": 100},
  "fpga": {"magic": "0x5344524C", "version": "0x00010300", "abi": "0x00000003", "regmap": "0x00000003"},
  "config": {"status": "valid", "provisioning": "matches_hardware", "node_id": "327951062",
             "tx_hz": "434000000", "rx_hz": "444000000", "sample_rate": "15360000"},
  "peer": {"compatibility": "UNKNOWN"},
  "modem": {}, "queues": {"control_depth": "UNKNOWN"},
  "bridge": {"node_id": 327951062,
             "rx": {"dma": 10, "frames": 20, "bytes": 30, "crc_errors": 0,
                    "duplicates": 0, "control": 5, "self": 0, "inject_err": 0},
             "tx": {"packets": 1, "errors": 0, "oversize": 0},
             "queues": {"control_depth": 0, "bulk_depth": 0, "control_drops": 0,
                        "bulk_drops": 0, "drain_ms": 0},
             "loop_guard": {"suppressed": 0}, "recoveries": 0},
  "ethernet": {"rx_packets": 5, "tx_packets": 5},
  "supervisor": {"bridges_running": 1, "supervisors": 1, "recoveries": 0, "restarts": 1, "faulted": 0},
  "progress": {"frames_idle_s": 0, "delivered_idle_s": 0, "verdict": "forwarding"},
}
def mkB():
    b = copy.deepcopy(GOOD_A)
    b["unit"]["mac"] = "00:60:88:3e:18:23"
    b["config"]["node_id"] = "465429558"
    b["config"]["tx_hz"] = "444000000"; b["config"]["rx_hz"] = "434000000"
    return b

def with_status(a, b):
    """Run check_preconditions against fixed dicts instead of live units."""
    saved = H.status, H.sh
    H.status = lambda host: a if host == "A" else b
    H.sh = lambda host, cmd, timeout=60: "samehash"
    try:
        return H.check_preconditions("A", "B", "ifA", "ifB")
    finally:
        H.status, H.sh = saved

def blocking_fail(pres, name_frag):
    return any((not p.ok) and p.blocking and name_frag in p.name for p in pres)

print("fixture identity -- a 'two unit' test whose units are the same unit")
a, b = copy.deepcopy(GOOD_A), mkB()
b["unit"]["mac"] = a["unit"]["mac"]
ck(blocking_fail(with_status(a, b), "MAC_A != MAC_B"), "identical MACs are rejected as a fixture error")

a, b = copy.deepcopy(GOOD_A), mkB()
b["config"]["node_id"] = a["config"]["node_id"]
ck(blocking_fail(with_status(a, b), "NODE_ID"), "identical NODE_IDs are rejected")

print("\nconfiguration that causes silent total loss")
a, b = copy.deepcopy(GOOD_A), mkB()
b["config"]["rx_hz"] = "444000000"           # not crossed
ck(blocking_fail(with_status(a, b), "crossed"), "uncrossed frequencies are rejected")

a, b = copy.deepcopy(GOOD_A), mkB()
b["config"]["tx_hz"] = b["config"]["rx_hz"] = "444000000"
ck(blocking_fail(with_status(a, b), "TX != RX"), "tx == rx on a unit is rejected")

a, b = copy.deepcopy(GOOD_A), mkB()
b["config"]["sample_rate"] = "7680000"
ck(blocking_fail(with_status(a, b), "sample rates"), "sample-rate mismatch is rejected")

a, b = copy.deepcopy(GOOD_A), mkB()
b["fpga"]["abi"] = "0x00000004"
ck(blocking_fail(with_status(a, b), "FPGA ABI"), "FPGA ABI mismatch is rejected")

a, b = copy.deepcopy(GOOD_A), mkB()
b["supervisor"]["bridges_running"] = 2
ck(blocking_fail(with_status(a, b), "exactly one bridge"), "duplicate bridges are rejected")

a, b = copy.deepcopy(GOOD_A), mkB()
b["supervisor"]["faulted"] = 1
ck(blocking_fail(with_status(a, b), "supervisor fault"), "a standing supervisor fault is rejected")

print("\nthe good pair must pass, or every negative above proves nothing")
pres = with_status(copy.deepcopy(GOOD_A), mkB())
ck(not any(p.blocking and not p.ok for p in pres), "a correctly configured pair has no blocking failures")
ck(any((not p.ok) and not p.blocking for p in pres),
   "unintegrated dependencies are recorded as non-blocking limitations")

print("\nstimulus lifetime")
s = H.Stimulus("feed"); s.check(alive=False)
try: s.validate(10.0); ck(False, "a source that ended early is INVALID")
except H.Invalid: ck(True, "a source that ended early is INVALID")
s = H.Stimulus("feed")
try: s.validate(10.0); ck(False, "an unchecked source is INVALID")
except H.Invalid: ck(True, "an unchecked source is INVALID")
s = H.Stimulus("feed"); s.expected = 100; s.check(alive=True, produced=40)
try: s.validate(10.0); ck(False, "producing less than expected is INVALID")
except H.Invalid: ck(True, "producing less than expected is INVALID")
s = H.Stimulus("feed"); s.check(alive=True, produced=100); s.expected = 100
try: s.validate(10.0); ck(True, "a live source that produced its quota validates")
except H.Invalid: ck(False, "a live source that produced its quota validates")

print("\ncounter deltas -- boot totals must never be reported as losses")
a2 = copy.deepcopy(GOOD_A); a2["bridge"]["rx"]["crc_errors"] = 7
d = H.deltas(GOOD_A, a2)
ck(d["rx.crc_errors"] == {"before": 0, "after": 7, "delta": 7}, "delta is after-before, not after")
a3 = copy.deepcopy(GOOD_A); del a3["bridge"]["rx"]["duplicates"]
ck(H.deltas(GOOD_A, a3)["rx.duplicates"]["delta"] is None, "a missing field is None, not zero")

print("\nprogress and liveness -- which layer stopped?")
st = copy.deepcopy(GOOD_A); st["progress"]["frames_idle_s"] = 120
stalled, why = H.progress_stalled(st)
ck(stalled and "RX DMA" in why, "frames not advancing names the RF/demodulator layer")
st = copy.deepcopy(GOOD_A); st["progress"]["delivered_idle_s"] = 120
stalled, why = H.progress_stalled(st)
ck(stalled and "delivered" in why, "frames advancing but bytes flat names the deframe/inject layer")
st = copy.deepcopy(GOOD_A); st["supervisor"]["bridges_running"] = 0
ck(H.progress_stalled(st)[0], "no bridge process is a stall")
ck(not H.progress_stalled(copy.deepcopy(GOOD_A))[0], "a healthy unit is not reported stalled")

print("\nhidden instability -- delivering every byte while recovering")
d = H.deltas(GOOD_A, GOOD_A)
ck(H.unexpected_recovery(d) == [], "a quiet run reports no instability")
a4 = copy.deepcopy(GOOD_A); a4["bridge"]["recoveries"] = 3; a4["supervisor"]["restarts"] = 2
r = H.unexpected_recovery(H.deltas(GOOD_A, a4))
ck(len(r) == 2, "recoveries and restarts during a run are both surfaced")

print("\nlatency reporting")
dist = H.distribution([1.0]*95 + [50.0]*5)
ck(dist["p50"] < 2 and dist["max"] == 50.0, "the tail is visible where an average would hide it")
ck(H.distribution([])["n"] == 0, "no samples reports n=0 rather than inventing a number")

print("\nstatus parsing")
saved = H.sh
H.sh = lambda host, cmd, timeout=60: "not json at all"
try: H.status("X"); ck(False, "unparseable status raises INVALID")
except H.Invalid: ck(True, "unparseable status raises INVALID")
H.sh = lambda host, cmd, timeout=60: ""
try: H.status("X"); ck(False, "empty status raises INVALID")
except H.Invalid: ck(True, "empty status raises INVALID")
H.sh = saved

print(f"\n{'ALL PASS' if not fails else 'FAILED'} ({fails} failures)")
sys.exit(1 if fails else 0)
