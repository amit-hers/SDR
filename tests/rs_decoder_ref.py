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
    if len(sys.argv) == 3 and sys.argv[1] == '--forney-vectors':
        # Emit N_BLOCKS full pipelines (real encode+corrupt+syndrome+BM+Chien,
        # 1..16 errors each) for the Forney RTL testbench: each block's
        # locator C (degree+1 bytes), its 32 syndromes, its found degrees,
        # and the expected (position, magnitude) pairs Forney should produce
        # for each -- which must reconstruct the exact injected errors.
        import random
        n_blocks = int(sys.argv[2])
        rng = random.Random(640016)
        coef_out, coef_lens = [], []
        synd_out = []
        deg_out, deg_counts = [], []
        mag_out = []  # magnitude per found degree, same order as deg_out
        for b in range(n_blocks):
            data = [rng.randint(0, 255) for _ in range(K)]
            cw = rs_encode(data)
            nerr = rng.randint(1, NPAR // 2)
            received = cw[:]
            for p in rng.sample(range(255), nerr):
                received[p] ^= rng.randint(1, 255)
            synd = syndromes(received)
            C, L = berlekamp_massey(synd)
            degrees = chien_search(C)
            errs = forney(C, synd, degrees)
            assert errs is not None, f"block {b}: Forney failure in vector generation"
            coef_out += C
            coef_lens.append(len(C))
            synd_out += synd
            deg_counts.append(len(degrees))
            for j in degrees:
                deg_out.append(j)
                mag_out.append(errs[j])
        for x in coef_out: print(f"{x:02x}")
        print("--")
        for x in coef_lens: print(f"{x:02x}")
        print("--")
        for x in synd_out: print(f"{x:02x}")
        print("--")
        for x in deg_counts: print(f"{x:02x}")
        print("--")
        for x in deg_out: print(f"{x:02x}")
        print("--")
        for x in mag_out: print(f"{x:02x}")
        sys.exit(0)
    if len(sys.argv) == 3 and sys.argv[1] == '--chien-vectors':
        # Emit N_BLOCKS locator polynomials (from REAL encode+corrupt+BM, 1..16
        # errors each) and their expected root degrees, for the Chien-search
        # RTL testbench. Coefficient stream per block: degree+1 bytes
        # (low-degree first, as rs_berlekamp_massey.v emits them); degrees are
        # emitted in the same increasing-j order Chien search itself produces.
        import random
        n_blocks = int(sys.argv[2])
        rng = random.Random(640016)
        coef_out = []      # degree+1 bytes per block, one block after another
        coef_lens = []     # length of each block's coefficient stream
        degree_counts = [] # number of roots found in each block
        degrees_out = []   # all found degrees, one block after another
        for b in range(n_blocks):
            data = [rng.randint(0, 255) for _ in range(K)]
            cw = rs_encode(data)
            nerr = rng.randint(1, NPAR // 2)
            received = cw[:]
            for p in rng.sample(range(255), nerr):
                received[p] ^= rng.randint(1, 255)
            synd = syndromes(received)
            C, L = berlekamp_massey(synd)
            degrees = chien_search(C)
            coef_out += C
            coef_lens.append(len(C))
            degree_counts.append(len(degrees))
            degrees_out += degrees
        for b in coef_out:
            print(f"{b:02x}")
        print("--")
        for n in coef_lens:
            print(f"{n:02x}")
        print("--")
        for n in degree_counts:
            print(f"{n:02x}")
        print("--")
        for d in degrees_out:
            print(f"{d:02x}")
        sys.exit(0)
    if len(sys.argv) == 3 and sys.argv[1] == '--bm-vectors':
        # Emit N_BLOCKS syndrome sets (0..16 injected errors each, including
        # at least one all-zero/no-error block) and their expected
        # Berlekamp-Massey output (degree + locator coefficients, low-degree
        # first, padded to NPAR+1 bytes with a separate degree byte) for the
        # rs_berlekamp_massey.v RTL testbench.
        import random
        n_blocks = int(sys.argv[2])
        rng = random.Random(640016)
        synd_out = []
        degree_out = []
        coef_out = []  # NPAR+1 bytes per block, padded with 0 beyond degree
        for b in range(n_blocks):
            data = [rng.randint(0, 255) for _ in range(K)]
            cw = rs_encode(data)
            nerr = 0 if b == 0 else rng.randint(0, NPAR // 2)
            received = cw[:]
            for p in rng.sample(range(255), nerr):
                received[p] ^= rng.randint(1, 255)
            synd = syndromes(received)
            C, L = berlekamp_massey(synd)
            synd_out += synd
            degree_out.append(L)
            padded = C + [0] * (NPAR + 1 - len(C))
            coef_out += padded
        for b in synd_out:
            print(f"{b:02x}")
        print("--")
        for d in degree_out:
            print(f"{d:02x}")
        print("--")
        for b in coef_out:
            print(f"{b:02x}")
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
    if len(sys.argv) == 3 and sys.argv[1] == '--decoder-vectors':
        # Emit N_BLOCKS full encode+corrupt+decode cases for rs_decoder.v's
        # integration testbench: real received codewords (255 bytes each),
        # covering 0 errors (clean), 1..16 errors (correctable), and
        # deliberately >16 errors (uncorrectable) -- plus, for each block,
        # the expected 223-byte output, a fail flag, and an error count.
        # Mirrors rs_decoder.v's own all-or-nothing correction semantics
        # (matching rs_decode()'s behavior): a failed block's expected
        # output is the ORIGINAL received bytes, uncorrected.
        import random
        n_blocks = int(sys.argv[2])
        rng = random.Random(640016)
        recv_out, exp_out, fail_out, errcnt_out = [], [], [], []
        for b in range(n_blocks):
            data = [rng.randint(0, 255) for _ in range(K)]
            cw = rs_encode(data)
            if b % 7 == 0:
                nerr = 0
            elif b % 11 == 0:
                nerr = rng.randint(NPAR // 2 + 1, 24)  # deliberately uncorrectable
            else:
                nerr = rng.randint(1, NPAR // 2)
            received = cw[:]
            for p in rng.sample(range(255), min(nerr, 255)):
                received[p] ^= rng.randint(1, 255)
            recv_out += received

            synd = syndromes(received)
            fail = False
            ncorr = 0
            out = received[:K]
            if any(s != 0 for s in synd):
                C, L = berlekamp_massey(synd)
                if L == 0 or L > NPAR // 2:
                    fail = True
                else:
                    degrees = chien_search(C)
                    if len(degrees) != L:
                        fail = True
                    else:
                        errs = forney(C, synd, degrees)
                        if errs is None:
                            fail = True
                        else:
                            corrected = received[:]
                            for j, e in errs.items():
                                corrected[254 - j] ^= e
                            if any(s != 0 for s in syndromes(corrected)):
                                fail = True
                            else:
                                out = corrected[:K]
                                ncorr = len(errs)
            exp_out += out
            fail_out.append(1 if fail else 0)
            errcnt_out.append(ncorr)
        for x in recv_out:
            print(f"{x:02x}")
        print("--")
        for x in exp_out:
            print(f"{x:02x}")
        print("--")
        for x in fail_out:
            print(f"{x:02x}")
        print("--")
        for x in errcnt_out:
            print(f"{x:02x}")
        sys.exit(0)
    print(__doc__)
    sys.exit(1)
