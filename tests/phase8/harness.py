"""Phase 8 measurement machinery.

This module exists because of a specific history: in this project, several
test suites reported green while exercising nothing, and several measurements
produced plausible numbers from invalid conditions.

  * peer fixtures left both MACs zeroed, so every "pair" compared a unit with
    itself and 13 assertions passed for the wrong reason
  * a config harness never regenerated its file, so 15 negative cases each
    re-validated the previous case's already-invalid file
  * RF captures ran past the end of the transmit feed, so the tail recorded
    silence and "the demodulator saturates at 11 MS/s" was a fixture artifact
  * a status parser matched "rx" inside "rx gap" and reported 0 where the line
    said 863

So this module treats an invalid run as a distinct outcome from a failed one.
A PASS that was never exercised is worse than a FAIL, because it closes the
question. Anything that cannot be measured is reported UNKNOWN, never inferred.
"""
from __future__ import annotations
import json, re, subprocess, time
from dataclasses import dataclass, field
from enum import Enum
from typing import Any

SSH = ["sshpass", "-p", "analog", "ssh", "-o", "StrictHostKeyChecking=no",
       "-o", "UserKnownHostsFile=/dev/null", "-o", "ConnectTimeout=10"]


class Verdict(str, Enum):
    PASS = "PASS"
    FAIL = "FAIL"        # the appliance did not meet the requirement
    INVALID = "INVALID"  # the TEST was not valid; says nothing about the appliance
    SKIP = "SKIP"        # not applicable / dependency not integrated


class Invalid(Exception):
    """Raised when a run cannot produce a meaningful result."""


def sh(host: str, cmd: str, timeout: int = 60) -> str:
    r = subprocess.run(SSH + [f"root@{host}", cmd], capture_output=True,
                       text=True, timeout=timeout, stdin=subprocess.DEVNULL)
    return r.stdout.strip()


# ── status snapshot ────────────────────────────────────────────────────────
def status(host: str) -> dict:
    """Full observability JSON from a unit. A snapshot that cannot be taken is
    itself a reason to invalidate a run, so this raises rather than returning
    a partial dict that later code would treat as measured."""
    raw = sh(host, "/mnt/jffs2/appliance_status.sh")
    if not raw:
        raise Invalid(f"status collection failed on {host} (no output)")
    try:
        return json.loads(raw)
    except json.JSONDecodeError as e:
        raise Invalid(f"status from {host} is not valid JSON: {e}")


# ── counter deltas ─────────────────────────────────────────────────────────
# Counters here are cumulative since boot and cannot be reset, so every number
# reported must be a delta. Reporting a boot-time total as a test loss has
# already happened once in this project: "tun drops tx 4" was the kernel's own
# pre-bridge count.
# The bridge publishes its own counters as JSON and the status view embeds them
# verbatim under "bridge", so these read from there. Kernel interface counters
# stay under "ethernet" because they are NOT the bridge's: a boot-time
# tx_dropped was once reported as bridge loss precisely because the two were
# conflated.
COUNTER_PATHS = [
    ("rx.dma", ("bridge", "rx", "dma")), ("rx.frames", ("bridge", "rx", "frames")),
    ("rx.delivered_bytes", ("bridge", "rx", "bytes")),
    ("rx.crc_errors", ("bridge", "rx", "crc_errors")),
    ("rx.duplicates", ("bridge", "rx", "duplicates")),
    ("rx.control", ("bridge", "rx", "control")),
    ("rx.self_discarded", ("bridge", "rx", "self")),
    ("rx.inject_err", ("bridge", "rx", "inject_err")),
    ("tx.packets", ("bridge", "tx", "packets")), ("tx.errors", ("bridge", "tx", "errors")),
    ("tx.oversize", ("bridge", "tx", "oversize")),
    ("q.control_depth", ("bridge", "queues", "control_depth")),
    ("q.bulk_depth", ("bridge", "queues", "bulk_depth")),
    ("q.control_drops", ("bridge", "queues", "control_drops")),
    ("q.bulk_drops", ("bridge", "queues", "bulk_drops")),
    ("eth.rx_packets", ("ethernet", "rx_packets")),
    ("eth.tx_packets", ("ethernet", "tx_packets")),
    ("loop.suppressed", ("bridge", "loop_guard", "suppressed")),
    ("sup.recoveries", ("bridge", "recoveries")),
    ("sup.restarts", ("supervisor", "restarts")),
    ("sup.faulted", ("supervisor", "faulted")),
]


def _dig(d: dict, path: tuple) -> Any:
    for k in path:
        if not isinstance(d, dict) or k not in d:
            return None
        d = d[k]
    return d


def deltas(before: dict, after: dict) -> dict:
    """before/after/delta for every counter. A field missing from either side
    is reported as None rather than 0 -- absent is not zero."""
    out = {}
    for name, path in COUNTER_PATHS:
        b, a = _dig(before, path), _dig(after, path)
        if isinstance(b, (int, float)) and isinstance(a, (int, float)):
            out[name] = {"before": b, "after": a, "delta": a - b}
        else:
            out[name] = {"before": b, "after": a, "delta": None}
    return out


# ── stimulus lifetime ──────────────────────────────────────────────────────
@dataclass
class Stimulus:
    """Proof that a source was still producing when the measurement ended.

    A capture that outran its source measures silence in its tail. That
    produced two retracted conclusions here, so a measurement whose stimulus
    ended early is INVALID rather than merely poor."""
    name: str
    started: float = field(default_factory=time.time)
    alive_at_end: bool | None = None
    produced: int | None = None
    expected: int | None = None

    def check(self, alive: bool, produced: int | None = None) -> None:
        self.alive_at_end = alive
        self.produced = produced

    def validate(self, measurement_s: float) -> None:
        if self.alive_at_end is False:
            raise Invalid(f"stimulus '{self.name}' ended before the measurement completed")
        if self.alive_at_end is None:
            raise Invalid(f"stimulus '{self.name}' liveness was never checked")
        if self.expected is not None and (self.produced or 0) < self.expected:
            raise Invalid(f"stimulus '{self.name}' produced {self.produced} of "
                          f"{self.expected} expected")
        if measurement_s <= 0:
            raise Invalid(f"measurement window for '{self.name}' was not positive")


# ── latency distribution ───────────────────────────────────────────────────
def distribution(samples: list[float]) -> dict:
    """p50/p95/p99/max, never an average alone. An average hides the tail, and
    the tail is what a control channel is judged on."""
    if not samples:
        return {"n": 0, "note": "no samples"}
    s = sorted(samples)
    def pct(p: float) -> float:
        if len(s) == 1:
            return s[0]
        i = min(len(s) - 1, max(0, int(round(p * (len(s) - 1)))))
        return s[i]
    return {"n": len(s), "min": s[0], "p50": pct(.50), "p95": pct(.95),
            "p99": pct(.99), "max": s[-1],
            "mean": sum(s) / len(s)}


# ── progress / recovery ────────────────────────────────────────────────────
def progress_stalled(st: dict, limit_s: int = 30) -> tuple[bool, str]:
    """Distinguish which layer stopped, rather than declaring a dead link.
    A bridge process can be alive while nothing is delivered."""
    p = st.get("progress", {})
    if st.get("supervisor", {}).get("bridges_running", 0) == 0:
        return True, "no bridge process running"
    fi, di = p.get("frames_idle_s"), p.get("delivered_idle_s")
    if isinstance(fi, int) and fi > limit_s:
        return True, f"RF frames have not advanced for {fi}s (RX DMA or demodulator)"
    if isinstance(di, int) and di > limit_s:
        return True, f"frames advance but delivered bytes have not for {di}s (deframe/inject)"
    return False, "progressing"


def unexpected_recovery(d: dict) -> list[str]:
    """A run can deliver every byte while hiding instability behind automatic
    recovery. That is not a clean pass."""
    out = []
    for k, label in (("sup.recoveries", "demodulator recovery"),
                     ("sup.restarts", "bridge restart"),
                     ("sup.faulted", "supervisor safe-fault")):
        dv = d.get(k, {}).get("delta")
        if isinstance(dv, int) and dv > 0:
            out.append(f"{label} occurred {dv} time(s) during the run")
    return out


# ── preconditions ──────────────────────────────────────────────────────────
@dataclass
class Precondition:
    name: str
    ok: bool
    detail: str = ""
    blocking: bool = True   # False => records a limitation, does not invalidate


def check_preconditions(a_host: str, b_host: str,
                        endpoint_a: str | None, endpoint_b: str | None) -> list[Precondition]:
    """Everything that must hold before a number from this pair means anything.

    Fixture assertions come FIRST and are blocking: a "two unit" test whose two
    units are the same unit, or share an identity, is not the scenario it
    claims. That exact fixture error produced 13 false passes here."""
    P: list[Precondition] = []
    def add(n, ok, d="", blocking=True): P.append(Precondition(n, bool(ok), d, blocking))

    try:
        sa, sb = status(a_host), status(b_host)
    except Invalid as e:
        add("both units reachable and reporting status", False, str(e))
        return P
    add("both units reachable and reporting status", True)

    # --- fixture identity: are these genuinely two different units? ---
    mac_a = sa["unit"].get("mac"); mac_b = sb["unit"].get("mac")
    add("MAC_A != MAC_B", bool(mac_a and mac_b and mac_a != mac_b),
        f"A={mac_a} B={mac_b}")
    nid_a = sa["config"].get("node_id"); nid_b = sb["config"].get("node_id")
    add("NODE_ID_A != NODE_ID_B", bool(nid_a and nid_b and nid_a != nid_b),
        f"A={nid_a} B={nid_b}")
    if endpoint_a is not None or endpoint_b is not None:
        add("endpoint_A != endpoint_B", bool(endpoint_a and endpoint_b and endpoint_a != endpoint_b),
            f"A={endpoint_a} B={endpoint_b}")

    # --- configuration agreement: mismatches here cause silent total loss ---
    add("configs valid on both units",
        sa["config"].get("status") == "valid" and sb["config"].get("status") == "valid",
        f"A={sa['config'].get('status')} B={sb['config'].get('status')}")
    add("provisioning matches hardware on both",
        sa["config"].get("provisioning") == "matches_hardware"
        and sb["config"].get("provisioning") == "matches_hardware",
        f"A={sa['config'].get('provisioning')} B={sb['config'].get('provisioning')}")
    add("sample rates match",
        sa["config"].get("sample_rate") == sb["config"].get("sample_rate"),
        f"A={sa['config'].get('sample_rate')} B={sb['config'].get('sample_rate')}")
    add("FPGA ABI and register map match",
        sa["fpga"].get("abi") == sb["fpga"].get("abi")
        and sa["fpga"].get("regmap") == sb["fpga"].get("regmap"),
        f"A={sa['fpga'].get('abi')}/{sa['fpga'].get('regmap')} "
        f"B={sb['fpga'].get('abi')}/{sb['fpga'].get('regmap')}")

    # --- frequencies must be CROSSED, and not equal on either unit ---
    atx, arx = sa["config"].get("tx_hz"), sa["config"].get("rx_hz")
    btx, brx = sb["config"].get("tx_hz"), sb["config"].get("rx_hz")
    add("TX != RX on each unit", bool(atx and arx and atx != arx and btx and brx and btx != brx),
        f"A {atx}/{arx}  B {btx}/{brx}")
    add("frequencies crossed between units", atx == brx and arx == btx,
        f"A.tx={atx} B.rx={brx} | A.rx={arx} B.tx={btx}")

    # --- runtime health ---
    add("exactly one bridge per unit",
        sa["supervisor"].get("bridges_running") == 1 and sb["supervisor"].get("bridges_running") == 1,
        f"A={sa['supervisor'].get('bridges_running')} B={sb['supervisor'].get('bridges_running')}")
    add("no supervisor fault standing",
        sa["supervisor"].get("faulted", 0) == 0 and sb["supervisor"].get("faulted", 0) == 0)
    add("bridge binaries identical",
        sh(a_host, "md5sum /mnt/jffs2/sdr_bridge | cut -d' ' -f1")
        == sh(b_host, "md5sum /mnt/jffs2/sdr_bridge | cut -d' ' -f1"))

    # --- dependencies that are not integrated yet: record, do not invent ---
    pc_a = sa.get("peer", {}).get("compatibility", "UNKNOWN")
    add("peer compatibility reported", pc_a == "COMPATIBLE",
        f"peer handshake reports {pc_a}; tests needing it are skipped, not assumed",
        blocking=False)
    # Queue metrics now come from the bridge's own JSON, so this reads from
    # there. It was left pointing at the old location after the scheduler was
    # integrated, and reported "not wired into txLoop" about a scheduler that
    # was -- a precondition describing a stale layout is itself a fixture bug.
    q = _dig(sb, ("bridge", "queues", "control_depth"))
    add("queue metrics available", isinstance(q, (int, float)),
        "scheduler metrics not published by the bridge; queue tests skipped",
        blocking=False)
    return P


def topology_is_separated(a_host: str, b_host: str) -> tuple[bool, str]:
    """Refuse to benchmark across a shared L2 segment.

    Both RJ45 ports on one switch means Ethernet crosses by copper, so every
    throughput and latency number would describe the switch. Proven here by
    disabling UNIT-B's demodulator and watching frames still arrive."""
    saved = sh(b_host, "devmem 0x43C00010 32")
    if not saved.startswith("0x"):
        return False, "could not read the demodulator enable to run the check"
    before = sh(b_host, "cat /sys/class/net/eth0/statistics/rx_packets")
    sh(b_host, "devmem 0x43C00010 32 0")
    try:
        sh(a_host, "ip addr add 10.201.0.1/24 dev eth0 2>/dev/null; "
                   "for h in 2 3 4 5 6 7 8; do ping -c1 -W1 -I eth0 10.201.0.$h >/dev/null 2>&1; done; "
                   "ip addr del 10.201.0.1/24 dev eth0 2>/dev/null", timeout=90)
        time.sleep(2)
        after = sh(b_host, "cat /sys/class/net/eth0/statistics/rx_packets")
    finally:
        sh(b_host, f"devmem 0x43C00010 32 {saved}")
    try:
        leaked = int(after) - int(before)
    except ValueError:
        return False, "could not read Ethernet counters for the topology check"
    if leaked > 0:
        return False, (f"{leaked} frames reached UNIT-B with its demodulator DISABLED: "
                       "the two RJ45 ports share an L2 segment, so any result would "
                       "measure copper, not the radio")
    return True, "no frames crossed with the radio disabled"
