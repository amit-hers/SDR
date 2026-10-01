#!/usr/bin/env python3
"""Independent, auditable GF(256) reference for an RS(255,223) decoder:
syndromes, Berlekamp-Massey, Chien search, Forney algorithm. Companion to
rs_encoder_ref.py -- same GF(2^8), primitive polynomial 0x11D, fcr=0
construction, so a codeword from rs_encoder_ref.rs_encode() decodes here.

Correctness is checked structurally (self_check(), run via --self-check):
inject 0..16 random byte errors into real codewords and confirm exact
recovery, including a dedicated run at the maximum correctable count t=16.
NOT compared against the host's liquid-dsp codec -- see rs_encoder_ref.py's
docstring for why.
"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rs_encoder_ref import gmul, gpow, exp, log, rs_encode, poly_eval, K, NPAR, G

# Array convention: received[0] is the FIRST transmitted byte, which is the
# HIGHEST-degree coefficient when treated as a polynomial (matches
# rs_encoder_ref.poly_eval's Horner convention). So an error at ARRAY INDEX p
# is an error at polynomial DEGREE j = 254 - p. Chien search and Forney work
# in degree-space (the standard formulation); rs_decode converts back to
# array indices when applying corrections.

def ginv(a):
    assert a != 0
    return exp[255 - log[a]]

def syndromes(received):
    return [poly_eval(received, gpow(2, i)) for i in range(NPAR)]

def berlekamp_massey(synd):
    C = [1] + [0]*NPAR
    B = [1] + [0]*NPAR
    L = 0
    m = 1
    b = 1
    for n in range(NPAR):
        delta = synd[n]
        for i in range(1, L+1):
            delta ^= gmul(C[i], synd[n-i])
        if delta == 0:
            m += 1
        elif 2*L <= n:
            T = C[:]
            coef = gmul(delta, ginv(b))
            for i in range(len(B)):
                if i+m < len(C):
                    C[i+m] ^= gmul(coef, B[i])
            L = n + 1 - L
            B = T
            b = delta
            m = 1
        else:
            coef = gmul(delta, ginv(b))
            for i in range(len(B)):
                if i+m < len(C):
                    C[i+m] ^= gmul(coef, B[i])
            m += 1
    return C[:L+1], L  # low-degree-first, C[0]=1

def chien_search(C):
    # Returns error DEGREES j (0..254) such that C(alpha^{-j}) == 0.
    degrees = []
    for j in range(255):
        x = gpow(2, (255 - j) % 255)  # alpha^{-j}
        s = 0; xp = 1
        for c in C:
            s ^= gmul(c, xp); xp = gmul(xp, x)
        if s == 0:
            degrees.append(j)
    return degrees

def poly_deriv_formal(C):
    # Dense, index == degree (zeros at even degrees, which vanish in char 2).
    d = [0] * (len(C) - 1)
    for i in range(1, len(C)):
        if i % 2 == 1:
            d[i-1] = C[i]
    return d

def forney(C, synd, error_degrees):
    S = synd[:]
    prod = [0] * (len(S) + len(C))
    for i, s in enumerate(S):
        if s == 0: continue
        for j, c in enumerate(C):
            if c == 0: continue
            prod[i+j] ^= gmul(s, c)
    Omega = prod[:NPAR]
    Cp = poly_deriv_formal(C)
    errors = {}
    for j in error_degrees:
        x = gpow(2, j)                      # alpha^{j} = X_j
        x_inv = gpow(2, (255 - j) % 255)     # alpha^{-j}
        num = 0; xp = 1
        for c in Omega:
            num ^= gmul(c, xp); xp = gmul(xp, x_inv)
        den = 0; xp = 1
        for c in Cp:
            den ^= gmul(c, xp); xp = gmul(xp, x_inv)
        if den == 0:
            return None
        e = gmul(x, gmul(num, ginv(den)))  # fcr=0 Forney: extra X_j^{1-fcr}=X_j factor
        errors[j] = e
    return errors

def rs_decode(received):
    synd = syndromes(received)
    if all(s == 0 for s in synd):
        return received[:], 0
    C, L = berlekamp_massey(synd)
    if L == 0 or L > NPAR//2:
        raise ValueError(f"uncorrectable: L={L}")
    degrees = chien_search(C)
    if len(degrees) != L:
        raise ValueError(f"uncorrectable: found {len(degrees)} roots, expected {L}")
    errs = forney(C, synd, degrees)
    if errs is None:
        raise ValueError("uncorrectable: Forney zero denominator")
    corrected = received[:]
    for j, e in errs.items():
        p = 254 - j
        corrected[p] ^= e
    if any(s != 0 for s in syndromes(corrected)):
        raise ValueError("uncorrectable: residual nonzero syndrome after correction")
    return corrected, L

def self_check(trials=200, seed=640016):
    import random
    rng = random.Random(seed)
    max_t = NPAR // 2
    ok = 0
    for t in range(trials):
        data = [rng.randint(0, 255) for _ in range(K)]
        cw = rs_encode(data)
        nerr = rng.randint(0, max_t)
        received = cw[:]
        positions = rng.sample(range(255), nerr)
        for p in positions:
            bad = rng.randint(1, 255)
            received[p] ^= bad
        corrected, found = rs_decode(received)
        assert found == nerr, f"trial {t}: injected {nerr}, BM found {found}"
        assert corrected == cw, f"trial {t}: corrected mismatch with {nerr} errors"
        ok += 1
    return ok

def self_check_max_t(trials=300, seed=99):
    import random
    rng = random.Random(seed)
    for t in range(trials):
        data = [rng.randint(0, 255) for _ in range(K)]
        cw = rs_encode(data)
        received = cw[:]
        positions = rng.sample(range(255), NPAR // 2)
        for p in positions:
            received[p] ^= rng.randint(1, 255)
        corrected, found = rs_decode(received)
        assert found == NPAR // 2 and corrected == cw, f"trial {t} failed at max t"
    return trials


if __name__ == '__main__':
    if len(sys.argv) == 2 and sys.argv[1] == '--self-check':
        n = self_check(trials=1000)
        m = self_check_max_t()
        print(f"OK: {n} trials (0..16 random errors) + {m} trials (always t=16), all decoded exactly")
        sys.exit(0)
    if len(sys.argv) == 3 and sys.argv[1] == '--syndrome-vectors':
        # Emit N_BLOCKS codewords (with 0..16 injected errors each) and their
        # expected syndromes, for the syndrome-calculator RTL testbench.
        import random
        n_blocks = int(sys.argv[2])
        rng = random.Random(640016)
        recv_out, synd_out = [], []
        for _ in range(n_blocks):
            data = [rng.randint(0, 255) for _ in range(K)]
            cw = rs_encode(data)
            nerr = rng.randint(0, NPAR // 2)
            received = cw[:]
            for p in rng.sample(range(255), nerr):
                received[p] ^= rng.randint(1, 255)
            recv_out += received
            synd_out += syndromes(received)
        for b in recv_out:
            print(f"{b:02x}")
        print("--")
        for b in synd_out:
            print(f"{b:02x}")
        sys.exit(0)
    print(__doc__)
    sys.exit(1)
