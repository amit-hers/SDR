#!/usr/bin/env python3
"""Summarise a PreambleSync correlation trace ($SDR_PSYNC_LOG).

The one question this answers:

    At high SNR, is PreambleSync failing because the signal is not there /
    not aligned, or because a perfectly usable preamble is being rejected?

The deciding number is the mean correlation peak ON FAILURES, per class:

    near MIN_QUALITY   -> the preamble was found and the THRESHOLD rejected
                          it.  Fix the threshold / the correlation, not the
                          search.
    near zero          -> nothing correlating was in the searched range.
                          Either it is outside the bound, or the waveform
                          does not match the reference (carrier offset).

The probe columns settle that second case: a wide rescan at several carrier
hypotheses, run on a sample of failures.
"""
import re
import sys
from collections import Counter, defaultdict

MIN_QUALITY = 0.30


def pct(a, b):
    return 100.0 * a / b if b else 0.0


def quant(xs, q):
    if not xs:
        return 0.0
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(q * len(xs)))]


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else '/tmp/psync_B.log'
    rows = []
    for line in open(path):
        if not line.startswith('[PS]'):
            continue
        kv = dict(re.findall(r'(\w+)=(\S+)', line))
        rows.append(kv)
    if not rows:
        print(f'  no [PS] lines in {path}')
        return

    byc = defaultdict(list)
    for r in rows:
        byc[r['class']].append(r)

    print('=' * 74)
    print('PreambleSync attempts: %d   (MIN_QUALITY=%.2f)' % (len(rows), MIN_QUALITY))
    print()
    hdr = ('  %-11s %7s %7s %7s   %-17s %-17s' %
           ('class', 'n', 'fail', 'fail%', 'mean peak (hit)', 'mean peak (MISS)'))
    print(hdr)
    print('  ' + '-' * (len(hdr) - 2))
    for cls in ('first', 'after_ok', 'after_fail'):
        rs = byc.get(cls)
        if not rs:
            continue
        f = [r for r in rs if r['found'] == '0']
        h = [r for r in rs if r['found'] == '1']
        mh = sum(float(r['peak']) for r in h) / len(h) if h else 0.0
        mf = sum(float(r['peak']) for r in f) / len(f) if f else 0.0
        print('  %-11s %7d %7d %6.1f%%   %-17.4f %-17.4f'
              % (cls, len(rs), len(f), pct(len(f), len(rs)), mh, mf))
    print()

    # Peak distribution on failures -- the whole question in one table.
    print('--- correlation peak on FAILED searches ---')
    for cls in ('first', 'after_ok', 'after_fail'):
        rs = [float(r['peak']) for r in byc.get(cls, []) if r['found'] == '0']
        if not rs:
            continue
        near = sum(1 for q in rs if q >= MIN_QUALITY * 0.75)
        floor = sum(1 for q in rs if q < MIN_QUALITY * 0.34)
        print('  %-11s n=%-6d p50=%.3f p90=%.3f max=%.3f | '
              'just under thresh (>=%.3f): %d (%.1f%%) | at noise floor (<%.3f): %d (%.1f%%)'
              % (cls, len(rs), quant(rs, .5), quant(rs, .9), max(rs),
                 MIN_QUALITY * 0.75, near, pct(near, len(rs)),
                 MIN_QUALITY * 0.34, floor, pct(floor, len(rs))))
    print()

    # Alignment: expected vs actual, on successful searches.
    print('--- offset error on SUCCESSFUL searches (actual - expected) ---')
    for cls in ('first', 'after_ok'):
        errs = [int(r['peak_off']) - int(r['expected_off'])
                for r in byc.get(cls, [])
                if r['found'] == '1' and int(r['expected_off']) >= 0]
        if not errs:
            continue
        print('  %-11s n=%-6d min=%-6d p50=%-6d p90=%-6d max=%-6d'
              % (cls, len(errs), min(errs), quant(errs, .5),
                 quant(errs, .9), max(errs)))
    print()

    # Peak distinctiveness.
    print('--- peak / second-peak on successful searches ---')
    for cls in ('first', 'after_ok', 'after_fail'):
        rr = [float(r['ratio']) for r in byc.get(cls, [])
              if r['found'] == '1' and float(r.get('ratio', 0)) > 0]
        if not rr:
            continue
        weak = sum(1 for x in rr if x < 1.5)
        print('  %-11s n=%-6d p10=%.2f p50=%.2f p90=%.2f | ratio<1.5 (ambiguous): %d (%.1f%%)'
              % (cls, len(rr), quant(rr, .1), quant(rr, .5), quant(rr, .9),
                 weak, pct(weak, len(rr))))
    print()

    # Probe post-mortem.
    probed = [r for r in rows if 'probe_q' in r]
    if probed:
        print('--- wide + carrier rescan of failures (n=%d) ---' % len(probed))
        wide = [r for r in probed if float(r['probe_q_df0']) >= MIN_QUALITY]
        cfo = [r for r in probed
               if float(r['probe_q_df0']) < MIN_QUALITY
               and float(r['probe_q']) >= MIN_QUALITY]
        none = [r for r in probed if float(r['probe_q']) < MIN_QUALITY]
        print('  present at df=0 but outside the search bound : %4d (%5.1f%%)'
              % (len(wide), pct(len(wide), len(probed))))
        print('  found ONLY after undoing a carrier offset    : %4d (%5.1f%%)'
              % (len(cfo), pct(len(cfo), len(probed))))
        print('  nothing correlating at any hypothesis        : %4d (%5.1f%%)'
              % (len(none), pct(len(none), len(probed))))
        if cfo:
            dfs = [float(r['probe_df']) for r in cfo]
            print('    carrier offset when that was the cause: p10=%.0f p50=%.0f p90=%.0f Hz'
                  % (quant(dfs, .1), quant(dfs, .5), quant(dfs, .9)))
        if wide:
            offs = [int(r['probe_off_df0']) for r in wide]
            bound = int(probed[0]['bound'])
            print('    where it actually sat: p50=%d p90=%d max=%d (bound was %d)'
                  % (quant(offs, .5), quant(offs, .9), max(offs), bound))
        print('  best q found in rescan: p50=%.3f p90=%.3f'
              % (quant([float(r['probe_q']) for r in probed], .5),
                 quant([float(r['probe_q']) for r in probed], .9)))
        print()

    # Verdict.
    print('=' * 74)
    fails = [r for r in rows if r['found'] == '0']
    if fails:
        mf = sum(float(r['peak']) for r in fails) / len(fails)
        near = sum(1 for r in fails if float(r['peak']) >= MIN_QUALITY * 0.75)
        if pct(near, len(fails)) >= 25.0:
            print('VERDICT: %.1f%% of failures peaked within 25%% of MIN_QUALITY.'
                  % pct(near, len(fails)))
            print('         A usable preamble is being REJECTED -- the threshold')
            print('         (or the correlation normalisation) is the problem.')
        elif probed and pct(len(cfo), len(probed)) >= 25.0:
            print('VERDICT: mean failure peak is %.3f (floor), but %.1f%% of probed'
                  % (mf, pct(len(cfo), len(probed))))
            print('         failures correlate once a carrier offset is undone.')
            print('         The preamble IS there; residual CFO cancels the peak.')
            print('         Fix carrier estimation, not the threshold or the search.')
        elif probed and pct(len(wide), len(probed)) >= 25.0:
            print('VERDICT: the preamble is present and un-shifted but sits OUTSIDE')
            print('         the search bound in %.1f%% of probed failures.'
                  % pct(len(wide), len(probed)))
        else:
            print('VERDICT: mean failure peak is %.3f and the wide/carrier rescan' % mf)
            print('         finds nothing either -- no preamble was present in the')
            print('         searched region at all. Look upstream of PreambleSync.')
    print('=' * 74)


if __name__ == '__main__':
    main()
