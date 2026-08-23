#!/usr/bin/env python3
"""UDP receiver: counts unique datagrams and reports goodput."""
import socket, time, signal, sys

class Stop(Exception): pass
def _t(*a): raise Stop()
signal.signal(signal.SIGTERM, _t)
signal.signal(signal.SIGINT, _t)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", 9999)); s.settimeout(45)
seen, first, last, total = set(), None, None, 0
try:
    while True:
        d, _ = s.recvfrom(65535)
        now = time.time()
        if first is None: first = now
        last = now; total += len(d)
        try: seen.add(int(d[:8]))
        except Exception: pass
except (socket.timeout, Stop):
    pass
if len(seen) < 2 or not first or last <= first:
    print(f"  datagrams received : {len(seen)} -- too few to quote a rate")
else:
    dur = last - first
    print(f"  datagrams received : {len(seen)}")
    print(f"  counter range      : {min(seen)}..{max(seen)}  (span {max(seen)-min(seen)+1})")
    print(f"  duration           : {dur:.1f}s")
    print(f"  goodput            : {total*8/dur/1e3:.1f} kbps  ({len(seen)/dur:.1f} pkt/s)")
