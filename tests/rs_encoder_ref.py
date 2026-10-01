#!/usr/bin/env python3
"""Independent, auditable GF(256) reference for the RS(255,223) systematic
encoder in fpga/rtl/rs_encoder.v.

NOT a copy of (or verified against) the host's liquid-dsp LIQUID_FEC_RS_M8
codec in src/core/fec/ReedSolomon.cpp -- liquid-dsp's internal root/basis
convention isn't available without its source. This implements the standard
construction directly: GF(2^8) with primitive polynomial 0x11D
(x^8+x^4+x^3+x^2+1), primitive element alpha=0x02, first consecutive root
index fcr=0, generator g(x) = prod_{i=0}^{31} (x - alpha^i).

Correctness is checked structurally, not by comparison to another codec:
every codeword this module emits must evaluate to zero at all 32 generator
roots, which is the defining property of an RS(255,223) codeword. See
self_check() below, run automatically when this file is executed directly.
"""
PRIM = 0x11D
K = 223
NPAR = 32

exp = [0] * 512
log = [0] * 256
_x = 1
for _i in range(255):
    exp[_i] = _x
    log[_x] = _i
    _x <<= 1
    if _x & 0x100:
        _x ^= PRIM
for _i in range(255, 512):
    exp[_i] = exp[_i - 255]


def gmul(a, b):
    if a == 0 or b == 0:
        return 0
    return exp[log[a] + log[b]]


def gpow(a, p):
    return exp[(log[a] * p) % 255]


def gen_poly(nsym):
    # g[k] = coefficient of x^k, lowest degree first; g[nsym] = 1 (monic).
    g = [1]
    for i in range(nsym):
        root = gpow(2, i)
        new_g = [0] * (len(g) + 1)
        for k, c in enumerate(g):
            new_g[k] ^= gmul(c, root)
            new_g[k + 1] ^= c
        g = new_g
    return g


G = gen_poly(NPAR)
assert G[NPAR] == 1 and len(G) == NPAR + 1


def rs_encode(data):
    assert len(data) == K
    reg = [0] * NPAR
    for d in data:
        fb = d ^ reg[NPAR - 1]
        for i in range(NPAR - 1, 0, -1):
            reg[i] = reg[i - 1] ^ gmul(fb, G[i])
        reg[0] = gmul(fb, G[0])
    parity = [reg[i] for i in range(NPAR - 1, -1, -1)]
    return data + parity


def poly_eval(coeffs_high_first, x):
    r = 0
    for c in coeffs_high_first:
        r = gmul(r, x) ^ c
    return r


def self_check(trials=50, seed=640016):
    import random
    rng = random.Random(seed)
    for t in range(trials):
        data = [rng.randint(0, 255) for _ in range(K)]
        cw = rs_encode(data)
        assert len(cw) == K + NPAR
        for i in range(NPAR):
            s = poly_eval(cw, gpow(2, i))
            if s != 0:
                raise AssertionError(
                    f"trial {t}: codeword nonzero at alpha^{i}: {s:#x}")
    return trials


if __name__ == '__main__':
    import sys
    if len(sys.argv) == 2 and sys.argv[1] == '--self-check':
        n = self_check()
        print(f"OK: {n} random codewords all zero at all {NPAR} generator roots")
        sys.exit(0)
    if len(sys.argv) == 3 and sys.argv[1] == '--vectors':
        # Emit N_BLOCKS * 223 data bytes then N_BLOCKS * 255 codeword bytes,
        # one hex byte per line, deterministic seed -- for the RTL testbench.
        import random
        n_blocks = int(sys.argv[2])
        rng = random.Random(640016)
        data_out, exp_out = [], []
        for _ in range(n_blocks):
            data = [rng.randint(0, 255) for _ in range(K)]
            cw = rs_encode(data)
            data_out += data
            exp_out += cw
        for b in data_out:
            print(f"{b:02x}")
        print("--")
        for b in exp_out:
            print(f"{b:02x}")
        sys.exit(0)
    print(__doc__)
    sys.exit(1)
