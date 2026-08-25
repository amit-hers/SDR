#!/usr/bin/env python3
"""Join a transmitter's burst log to the peer receiver's window log.

Both nodes are told to write $SDR_BURST_LOG.  The transmitting node emits one
[TXB] line per buffer that reached the air -- that IS one burst: one txPush,
one contiguous run of samples, a duty gap either side.  The receiving node
emits one [RXW] line per detected window.  The nodes share no clock, but they
do share the frame sequence numbers, so the logs join on seq.

The question this exists to answer:

    Does one TX burst arrive as one RX window, or does the receiver chop it
    into several?

    one TX burst -> several RX windows   the loss is window SEGMENTATION;
                                         fix detection / merge.
    one TX burst -> one RX window, but
    frames-per-window varies             detection is fine; the loss is inside
                                         burst acquisition / frame extraction.

Usage:
    burst_join.py --tx nodeA-burst.log --rx nodeB-burst.log
"""
import argparse
import re
from collections import Counter, defaultdict


def parse_kv(line):
    out = {}
    for m in re.finditer(r'(\w+)=(\S+)', line):
        out[m.group(1)] = m.group(2)
    return out


def parse_seqs(field):
    if not field or field == '-':
        return []
    return [int(x) for x in field.split(',') if x != '']


def load_tx(path):
    """burst id -> list of seqs, in transmit order."""
    bursts = {}
    with open(path) as f:
        for line in f:
            if not line.startswith('[TXB]'):
                continue
            kv = parse_kv(line)
            bursts[int(kv['burst'])] = {
                'seqs': parse_seqs(kv.get('seqs')),
                'samples': int(kv.get('samples', 0)),
                'frames': int(kv.get('frames', 0)),
                'air_us': int(kv.get('air_us', 0)),
            }
    return bursts


def load_rx(path):
    """List of windows, in receive order."""
    wins = []
    for i, line in enumerate(open(path)):
        if not line.startswith('[RXW]'):
            continue
        kv = parse_kv(line)

        def num(k, default=None):
            v = kv.get(k)
            if v is None or v == '-':
                return default
            return int(v)

        wins.append({
            'i': len(wins),
            'batch': num('batch', 0),
            'raw_len': num('raw_len', 0),
            'ext_len': num('ext_len', 0),
            'raw_gap_prev': num('raw_gap_prev'),
            'ext_overlap_prev': num('ext_overlap_prev'),
            'frames': num('frames', 0),
            'exit': kv.get('exit', '?'),
            'seqs': parse_seqs(kv.get('seqs')),
        })
    return wins


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tx', required=True, help='burst log from the TRANSMITTING node')
    ap.add_argument('--rx', required=True, help='burst log from the RECEIVING node')
    ap.add_argument('--examples', type=int, default=8,
                    help='split bursts to print in full')
    args = ap.parse_args()

    tx = load_tx(args.tx)
    rx = load_rx(args.rx)
    if not tx:
        raise SystemExit(f'no [TXB] lines in {args.tx}')
    if not rx:
        raise SystemExit(f'no [RXW] lines in {args.rx}')

    # seq -> the burst it was transmitted in.  Retransmits reuse a seq; keep
    # the last, so a seq maps to the most recent burst that carried it.
    seq_burst = {}
    for bid, b in tx.items():
        for s in b['seqs']:
            seq_burst[s] = bid

    # seq -> the window(s) it was decoded in.
    burst_windows = defaultdict(set)   # burst id -> set of window indices
    window_bursts = defaultdict(set)   # window index -> set of burst ids
    decoded_seqs = set()
    unknown_seqs = 0
    for w in rx:
        for s in w['seqs']:
            decoded_seqs.add(s)
            bid = seq_burst.get(s)
            if bid is None:
                unknown_seqs += 1
                continue
            burst_windows[bid].add(w['i'])
            window_bursts[w['i']].add(bid)

    tx_frames = sum(len(b['seqs']) for b in tx.values())
    print('=' * 72)
    print('TX  bursts=%d frames=%d  mean frames/burst=%.2f  mean samples/burst=%.0f'
          % (len(tx), tx_frames, tx_frames / len(tx),
             sum(b['samples'] for b in tx.values()) / len(tx)))
    rx_frames = sum(w['frames'] for w in rx)
    print('RX  windows=%d frames=%d  mean frames/window=%.2f'
          % (len(rx), rx_frames, rx_frames / len(rx)))
    print('    decoded %d of %d transmitted seqs (%.1f%%); %d decoded seqs not in the TX log'
          % (len(decoded_seqs & set(seq_burst)), len(seq_burst),
             100.0 * len(decoded_seqs & set(seq_burst)) / max(1, len(seq_burst)),
             unknown_seqs))
    print()

    # ── THE question ────────────────────────────────────────────────────────
    heard = {b: ws for b, ws in burst_windows.items() if ws}
    spread = Counter(len(ws) for ws in heard.values())
    print('--- windows per TX burst (bursts with >=1 frame decoded) ---')
    total_heard = len(heard)
    if total_heard:
        for n in sorted(spread):
            print('  %2d window(s): %6d bursts  (%5.1f%%)%s'
                  % (n, spread[n], 100.0 * spread[n] / total_heard,
                     '   <-- SPLIT' if n > 1 else ''))
        split = sum(c for n, c in spread.items() if n > 1)
        print('  bursts split across >1 window: %d / %d = %.1f%%'
              % (split, total_heard, 100.0 * split / total_heard))

        # A split matters only if it happened INSIDE one capture buffer.
        # Split across two rxPull batches is a capture-boundary straddle:
        # the detector never saw the two halves together and no merge_gap
        # setting could have joined them.  Only a same-batch split is a
        # detection/merge failure.
        same_batch = cross_batch = 0
        for ws in heard.values():
            if len(ws) < 2:
                continue
            if len({rx[w]['batch'] for w in ws}) == 1:
                same_batch += 1
            else:
                cross_batch += 1
        print('    of those: %d inside ONE capture batch  <- detector/merge failure'
              % same_batch)
        print('              %d across capture batches    <- rxPull boundary straddle,'
              ' not a merge problem' % cross_batch)
    print()

    print('--- TX bursts per RX window (is one window covering several bursts?) ---')
    merged = Counter(len(bs) for bs in window_bursts.values())
    tot_w = sum(merged.values())
    for n in sorted(merged):
        print('  %2d burst(s): %6d windows (%5.1f%%)'
              % (n, merged[n], 100.0 * merged[n] / max(1, tot_w)))
    print()

    # ── Frames recovered per burst, for bursts heard at all ─────────────────
    print('--- frame recovery within a heard burst ---')
    got = Counter()
    for bid, ws in heard.items():
        sent = len(tx[bid]['seqs'])
        rec = len(set(tx[bid]['seqs']) & decoded_seqs)
        got[(sent, rec)] += 1
    tot_sent = sum(len(tx[b]['seqs']) for b in heard)
    tot_rec = sum(len(set(tx[b]['seqs']) & decoded_seqs) for b in heard)
    print('  of bursts that were heard at all: %d/%d frames recovered (%.1f%%)'
          % (tot_rec, tot_sent, 100.0 * tot_rec / max(1, tot_sent)))
    for (sent, rec), c in sorted(got.items())[:12]:
        print('    sent %2d -> recovered %2d : %d bursts' % (sent, rec, c))
    print()

    # ── Window geometry ─────────────────────────────────────────────────────
    gaps = [w['raw_gap_prev'] for w in rx if w['raw_gap_prev'] is not None]
    overl = [w['ext_overlap_prev'] for w in rx if w['ext_overlap_prev']]
    print('--- window geometry ---')
    if gaps:
        gaps_s = sorted(gaps)
        print('  raw gap to previous window (samples): n=%d min=%d p10=%d median=%d p90=%d'
              % (len(gaps_s), gaps_s[0], gaps_s[len(gaps_s) // 10],
                 gaps_s[len(gaps_s) // 2], gaps_s[len(gaps_s) * 9 // 10]))
    if overl:
        o = sorted(overl)
        print('  extended windows overlapping predecessor: %d (%.1f%% of windows)'
              ' median overlap=%d max=%d'
              % (len(o), 100.0 * len(o) / len(rx), o[len(o) // 2], o[-1]))
    print('  walk exit reasons: %s'
          % dict(Counter(w['exit'] for w in rx).most_common()))
    print()

    # ── Concrete examples of the split ──────────────────────────────────────
    ex = [(b, sorted(ws)) for b, ws in heard.items() if len(ws) > 1]
    if ex and args.examples:
        print('--- example split bursts ---')
        for bid, ws in sorted(ex)[:args.examples]:
            print('  TX burst %d: %d frames, %d samples, seqs=%s'
                  % (bid, tx[bid]['frames'], tx[bid]['samples'],
                     ','.join(str(s) for s in tx[bid]['seqs'])))
            for wi in ws:
                w = rx[wi]
                dash = lambda v: '-' if v is None else v
                print('      -> RX window %d (batch %d) raw_len=%d ext_len=%d '
                      'gap_prev=%s overlap_prev=%s frames=%d exit=%s seqs=%s'
                      % (wi, w['batch'], w['raw_len'], w['ext_len'],
                         dash(w['raw_gap_prev']), dash(w['ext_overlap_prev']),
                         w['frames'], w['exit'],
                         ','.join(str(s) for s in w['seqs'])))
        print()

    # ── Verdict ─────────────────────────────────────────────────────────────
    print('=' * 72)
    if total_heard:
        split_pct = 100.0 * sum(c for n, c in spread.items() if n > 1) / total_heard
        recov_pct = 100.0 * tot_rec / max(1, tot_sent)
        same_batch = sum(1 for ws in heard.values()
                         if len(ws) > 1 and len({rx[w]['batch'] for w in ws}) == 1)
        same_pct = 100.0 * same_batch / total_heard
        if same_pct >= 5.0:
            print('VERDICT: %.1f%% of heard bursts are split INSIDE one capture batch.'
                  % same_pct)
            print('         Window segmentation is a real loss path -- fix burst')
            print('         detection / merge (merge_gap, margin, hysteresis).')
        elif split_pct >= 10.0:
            print('VERDICT: %.1f%% of heard bursts span more than one window, but only'
                  % split_pct)
            print('         %.1f%% within a single capture batch -- these are rxPull'
                  % same_pct)
            print('         boundary straddles, not merge failures. merge_gap will not')
            print('         help; carrying window state across batches would.')
        else:
            print('VERDICT: windows track bursts (%.1f%% split).' % split_pct)
            print('         Only %.1f%% of frames in HEARD bursts are recovered, so the'
                  % recov_pct)
            print('         loss is inside burst acquisition / frame extraction,')
            print('         not in detection.')
    print('=' * 72)


if __name__ == '__main__':
    main()
