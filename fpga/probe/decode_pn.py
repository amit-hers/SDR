#!/usr/bin/env python3
"""Solve the AD9363 RX lane->bit mapping from one synchronous BIST PRBS capture.

Assumption-free by construction. The AD9363 BIST drives a PN sequence onto the
data port; if the LFSR advances one bit per bus bit then lane i carries
s[W*n + i] -- i.e. EVERY lane is the same binary sequence at a different phase.
So the mapping falls out of lane-to-lane cross-correlation:

    lane_j[n] == lane_0[n + k]   ==>   b(j) - b(0) == W*k  (mod period)

which yields the bit indices without ever naming the polynomial. The period is
measured from the autocorrelation rather than assumed, so a PN9/PN15/PN23
difference changes nothing in the method.

Note the offset: correlation only ever gives bits RELATIVE to lane 0, and lane
0's own bit is unknown. The absolute map is pinned by the one rotation c that
makes the recovered set exactly {0..11} -- which is also a strong self-check,
since a wrong period, a wrong step, or noise leaves no such rotation.

Usage:  decode_pn.py <hexdump>   # 1024 words, one 32-bit hex value per token
"""
import sys
import numpy as np

# Capture coordinate system: bit i of the captured word is this package pin.
LANE_PINS = ["Y18", "V17", "V20", "R16", "W18", "V16",
             "Y19", "V18", "W20", "R17", "W19", "W16"]
NL = 12
QUAL_MIN = 0.45          # correlation floor; ~10x the noise of a 500-sample corr


def load(path):
    toks = open(path).read().split()
    hexd = set("0123456789abcdefABCDEF")
    vals = [int(t, 16) for t in toks if t and all(c in hexd for c in t)]
    return np.array(vals, dtype=np.uint32)


def bits_of(words):
    lanes = np.array([(words >> i) & 1 for i in range(NL)], dtype=np.int8)
    frame = ((words >> 12) & 1).astype(np.int8)
    return lanes, frame


def bipolar(b):
    return 2.0 * b.astype(np.float64) - 1.0


def measure_period(x):
    """Peak of the linear autocorrelation -> sequence period."""
    n = len(x)
    best, bq = 0, 0.0
    for k in range(8, n // 2):
        m = n - k
        q = abs(float(np.dot(x[:m], x[k:k + m]) / m))
        if q > bq:
            best, bq = k, q
    return best, bq


def best_shift(l0, lj, maxshift, m):
    """Linear cross-correlation; returns (shift, polarity, |quality|)."""
    bk, bq = 0, 0.0
    for k in range(maxshift):
        q = float(np.dot(lj[:m], l0[k:k + m]) / m)
        if abs(q) > abs(bq):
            bk, bq = k, q
    return bk, (1 if bq >= 0 else -1), abs(bq)


def solve(lanes, width, period, label):
    n = lanes.shape[1]
    maxshift = min(period, n // 2)
    m = n - maxshift
    if m < 64:
        return None
    l0 = bipolar(lanes[0])
    rel, pol, qual = [], [], []
    for j in range(NL):
        k, p, q = best_shift(l0, bipolar(lanes[j]), maxshift, m)
        rel.append((width * k) % period)
        pol.append(p)
        qual.append(q)

    print(f"\n=== attempt: step={width} bits/sample, period={period} ({label}) ===")
    if min(qual) < QUAL_MIN:
        worst = int(np.argmin(qual))
        print(f"  REJECTED: lane {worst} ({LANE_PINS[worst]}) correlates only "
              f"{qual[worst]:.3f} -- not a coherent PN capture")
        return None

    # Correlation gives bits relative to lane 0; pin the absolute map by the one
    # rotation that yields exactly {0..11}. No such rotation => wrong hypothesis.
    for c in range(period):
        bits = [(r + c) % period for r in rel]
        if sorted(bits) == list(range(NL)):
            print("  lane  pin    shift  pol  quality  -> AD9363 bit")
            for j in range(NL):
                inv = "  (INVERTED)" if pol[j] < 0 else ""
                print(f"   {j:2d}   {LANE_PINS[j]:4s}  {rel[j]:5d}  {pol[j]:+d}  "
                      f"{qual[j]:7.3f}  -> {bits[j]:2d}{inv}")
            print(f"  VALID: clean permutation of 0..11 (rotation c={c}, "
                  f"min quality {min(qual):.3f})")
            return bits, pol
    print("  REJECTED: no rotation yields a permutation of 0..11")
    return None


def main():
    words = load(sys.argv[1])
    print(f"loaded {len(words)} words")
    lanes, frame = bits_of(words)

    print("\n=== per-lane activity (a stuck lane invalidates everything) ===")
    print("  lane  pin    ones%   transitions")
    for j in range(NL):
        tr = int(np.count_nonzero(np.diff(lanes[j])))
        flag = "  <-- STUCK" if tr == 0 else ""
        print(f"   {j:2d}   {LANE_PINS[j]:4s}  {100.0*lanes[j].mean():6.2f}   "
              f"{tr:6d}{flag}")
    ftr = int(np.count_nonzero(np.diff(frame)))
    print(f"  RX_FRAME: ones={100.0*frame.mean():.2f}%  transitions={ftr}")
    if ftr:
        idx = np.flatnonzero(np.diff(frame)) + 1
        print(f"  frame edge gaps (first few): {np.diff(idx)[:8]}")

    period, quality = measure_period(bipolar(lanes[0]))
    print(f"\n=== measured period of lane 0: {period} (autocorr {quality:.3f}) ===")

    tried = set()
    for name, p in (("measured", period), ("PN9", 511), ("PN15", 32767)):
        if not p or p in tried or p >= len(words):
            continue
        tried.add(p)
        for width in (12, 24):
            r = solve(lanes, width, p, name)
            if r:
                print("\n*** AUTHORITATIVE MAP: rx_data_in bit -> package pin ***")
                bits, pol = r
                for bit in range(NL):
                    j = bits.index(bit)
                    print(f"    rx_data_in[{bit:2d}] = {LANE_PINS[j]}"
                          f"{'   (INVERTED)' if pol[j] < 0 else ''}")
                return
    print("\nNO SOLUTION -- capture is not a coherent PN sequence. "
          "Check that BIST really engaged (reg 0x3F4) before trusting anything.")


if __name__ == "__main__":
    main()
