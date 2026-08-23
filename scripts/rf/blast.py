#!/usr/bin/env python3
"""Offered-load generator: fixed-size UDP datagrams at a fixed rate.

Rate must stay BELOW link capacity. Above it the channel saturates, duty
pins at 100%, the burst detector loses the gaps it needs, and results vary
several-fold between identical runs.
"""
import socket, sys, time
SIZE = int(sys.argv[1]) if len(sys.argv) > 1 else 1000
RATE = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
DUR  = float(sys.argv[3]) if len(sys.argv) > 3 else 30.0
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
end = time.time() + DUR
n = 0
while time.time() < end:
    body = b"%08d" % n + b"x" * max(0, SIZE - 8)
    try: s.sendto(body, ("10.99.0.2", 9999))
    except OSError: pass
    n += 1
    time.sleep(1.0 / RATE)
print("sent", n)
