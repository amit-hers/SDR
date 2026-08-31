#!/usr/bin/env python3
"""Self-test for decode_pn.py against synthetic captures with a KNOWN answer.

There is exactly one chance to take the real capture, so the decode is verified
here first: a correct solver must recover a known permutation, and -- just as
important -- must REJECT noise rather than inventing a plausible-looking map.
"""
import random
import subprocess
import sys

NL, N = 12, 1024
HERE = "/home/amither/Documents/SDR/fpga/probe"


def pn9(n):
    """x^9 + x^5 + 1, period 511."""
    reg, out = 0x1FF, []
    for _ in range(n):
        bit = ((reg >> 8) ^ (reg >> 4)) & 1
        out.append(reg & 1)
        reg = ((reg << 1) | bit) & 0x1FF
    return out


def make(perm, width=12, invert=(), noise=False):
    s = pn9(width * N + 64)
    words = []
    for n in range(N):
        w = 0
        for lane in range(NL):
            if noise:
                b = random.randint(0, 1)
            else:
                b = s[(width * n + perm[lane]) % 511]
                if lane in invert:
                    b ^= 1
            w |= b << lane
        w |= (n & 1) << 12                      # RX_FRAME alternates I/Q
        words.append(w)
    return words


def run(words, tag):
    p = f"/tmp/{tag}.hex"
    open(p, "w").write("\n".join(f"{w:08x}" for w in words))
    r = subprocess.run([sys.executable, f"{HERE}/decode_pn.py", p],
                       capture_output=True, text=True)
    return r.stdout


def check(name, out, expect_lines, want_solution=True):
    ok = all(e in out for e in expect_lines)
    if want_solution:
        ok = ok and "AUTHORITATIVE MAP" in out
    else:
        ok = ok and "NO SOLUTION" in out
    print(f"[{'PASS' if ok else 'FAIL'}] {name}")
    if not ok:
        print(out)
    return ok


random.seed(1)
LANE_PINS = ["Y18", "V17", "V20", "R16", "W18", "V16",
             "Y19", "V18", "W20", "R17", "W19", "W16"]
allok = True

# 1. identity map
perm = list(range(NL))
allok &= check("identity mapping recovered", run(make(perm), "id"),
               [f"rx_data_in[ {b}] = {LANE_PINS[perm.index(b)]}" if b < 10
                else f"rx_data_in[{b}] = {LANE_PINS[perm.index(b)]}"
                for b in range(NL)])

# 2. shuffled map -- lane 0 deliberately NOT bit 0, which is the case a
#    relative-only solver gets wrong
perm = [7, 3, 11, 0, 5, 9, 1, 8, 4, 10, 2, 6]
allok &= check("shuffled mapping recovered (lane0 = bit 7)",
               run(make(perm), "sh"),
               [f"rx_data_in[ {b}] = {LANE_PINS[perm.index(b)]}" if b < 10
                else f"rx_data_in[{b}] = {LANE_PINS[perm.index(b)]}"
                for b in range(NL)])

# 3. shuffled + two inverted lanes
allok &= check("inversion detected", run(make(perm, invert=(2, 5)), "inv"),
               ["(INVERTED)"])

# 4. pure noise MUST be rejected -- the failure mode that matters
allok &= check("noise rejected", run(make(perm, noise=True), "noise"), [],
               want_solution=False)

print("\nALL DECODE TESTS PASS" if allok else "\nDECODE TESTS FAILED")
sys.exit(0 if allok else 1)
