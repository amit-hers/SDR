#!/usr/bin/env python3
"""Summarise one or more datalink runs from their saved logs.

Every run is measured the same way, so configurations can be compared without
the comparison itself becoming a variable. Give it the log-file prefix of each
run; it expects <prefix>_A.log, <prefix>_B.log and <prefix>_dB.log.

    run_report.py baselines/2mhz-qpsk/r1 baselines/2mhz-qpsk/r2 ...
    run_report.py --label 4MHz baselines/4mhz-qpsk/r*
"""
import argparse
import gzip
import os
import re
import statistics as st
from collections import defaultdict


def _open(path):
    """Saved baselines are gzipped; live /tmp logs are not."""
    if os.path.exists(path):
        return open(path)
    return gzip.open(path + '.gz', 'rt')


def load(prefix):
    tx = {}
    for line in _open(f'{prefix}_A.log'):
        if line.startswith('[TXB]'):
            kv = dict(re.findall(r'(\w+)=(\S+)', line))
            tx[int(kv['burst'])] = {
                'seqs': [int(x) for x in kv['seqs'].split(',')] if kv['seqs'] != '-' else [],
                'samples': int(kv['samples']),
                'peak': float(kv.get('peak', 0)),
            }
    wins = []
    for line in _open(f'{prefix}_B.log'):
        if line.startswith('[RXW]'):
            kv = dict(re.findall(r'(\w+)=(\S+)', line))
            wins.append({
                'frames': int(kv['frames']),
                'exit': kv.get('exit', '?'),
                'seqs': [int(x) for x in kv['seqs'].split(',')] if kv['seqs'] != '-' else [],
            })
    d = _open(f'{prefix}_dB.log').read()
    # TX-side figures must come from the TRANSMITTING node. Node B barely
    # transmits, so reading duty or clipping from its log reports zero and
    # looks like the limiter is off.
    try:
        da = _open(f'{prefix}_dA.log').read()
    except OSError:
        da = ''
    return tx, wins, d, da


def metrics(prefix):
    tx, wins, d, da = load(prefix)

    def g(pat, default=0):
        m = re.search(pat, d)
        return int(m.group(1)) if m else default

    seq2b = {s: b for b, v in tx.items() for s in v['seqs']}
    allseq = [s for w in wins for s in w['seqs']]
    bw = defaultdict(set)
    for i, w in enumerate(wins):
        for s in w['seqs']:
            if s in seq2b:
                bw[seq2b[s]].add(i)
    per = [(len(tx[b]['seqs']),
            len([s for s in wins[list(ws)[0]]['seqs'] if seq2b.get(s) == b]))
           for b, ws in bw.items() if len(ws) == 1]
    fpw = [w['frames'] for w in wins] or [0]
    txf = sum(len(v['seqs']) for v in tx.values()) or 1
    good, bad = g(r'rx_good=(\d+)'), g(r'rx_bad=(\d+)')
    cpu = re.search(r'([0-9.]+)% of one core', d)
    duty = re.search(r'= ([0-9.]+)% duty', da)
    clamp = re.search(r'samples_clamped_at_full_scale=(\d+)', da)
    return {
        'tx_bursts': len(tx),
        'tx_frames': txf,
        'rx_frames': good,
        'crc_bad': bad,
        'crc_pct': 100.0 * bad / max(1, good + bad),
        'recov': 100.0 * good / txf,
        'full': 100.0 * sum(1 for a, b in per if b == a) / max(1, len(per)),
        'windows': len(wins),
        'fpw': st.mean(fpw),
        'cv': st.pstdev(fpw) / max(1e-9, st.mean(fpw)),
        'dup': len(allseq) - len(set(allseq)),
        'split': sum(1 for ws in bw.values() if len(ws) > 1),
        'dropped': g(r'"dropped":\s*(\d+)'),
        'mb': g(r'bytes tx=\d+ rx=(\d+)') / 1e6,
        'cpu': float(cpu.group(1)) if cpu else 0.0,
        'duty': float(duty.group(1)) if duty else 0.0,
        'clamped': int(clamp.group(1)) if clamp else 0,
        'burst_samples': st.mean([v['samples'] for v in tx.values()]) if tx else 0,
    }


ROWS = [
    ('frames transmitted',      'tx_frames',  '{:.0f}'),
    ('frames recovered',        'rx_frames',  '{:.0f}'),
    ('recovery',                'recov',      '{:.1f}%'),
    ('CRC failures',            'crc_bad',    '{:.0f}'),
    ('CRC failure rate',        'crc_pct',    '{:.3f}%'),
    ('bursts fully recovered',  'full',       '{:.1f}%'),
    ('frames per window',       'fpw',        '{:.2f}'),
    ('frames/window CV',        'cv',         '{:.2f}'),
    ('bursts split',            'split',      '{:.0f}'),
    ('duplicate frames',        'dup',        '{:.0f}'),
    ('payload delivered (MB)',  'mb',         '{:.2f}'),
    ('TX duty',                 'duty',       '{:.1f}%'),
    ('RX CPU (one core)',       'cpu',        '{:.1f}%'),
    ('capture drops',           'dropped',    '{:.0f}'),
    ('TX samples clamped',      'clamped',    '{:.0f}'),
    ('mean burst (samples)',    'burst_samples', '{:.0f}'),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('prefixes', nargs='+')
    ap.add_argument('--label', default=None)
    args = ap.parse_args()

    runs = [metrics(p) for p in args.prefixes]
    name = args.label or 'run'
    w = 22
    print('=' * (w + 13 * len(runs) + 14))
    print(f'{name}  ({len(runs)} run{"s" if len(runs) > 1 else ""})')
    print('=' * (w + 13 * len(runs) + 14))
    head = f'  {"metric":<{w}}' + ''.join(f'{"r"+str(i+1):>12}' for i in range(len(runs)))
    head += f'{"mean":>13}'
    print(head)
    print('  ' + '-' * (len(head) - 2))
    for label, key, fmt in ROWS:
        cells = ''.join(f'{fmt.format(r[key]):>12}' for r in runs)
        mean = st.mean(r[key] for r in runs)
        print(f'  {label:<{w}}{cells}{fmt.format(mean):>13}')
    print()


if __name__ == '__main__':
    main()
