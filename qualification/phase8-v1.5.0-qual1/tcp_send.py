#!/usr/bin/env python3
# Minimal TCP goodput sender: connects, sends N MB of data, closes.
import socket, sys, time

host = sys.argv[1]
port = int(sys.argv[2])
mb = float(sys.argv[3]) if len(sys.argv) > 3 else 20.0

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect((host, port))
chunk = b"Q" * 65536
target = int(mb * 1e6)
sent = 0
t0 = time.time()
while sent < target:
    n = s.send(chunk)
    sent += n
s.shutdown(socket.SHUT_WR)
elapsed = time.time() - t0
mbps = (sent * 8 / 1e6 / elapsed) if elapsed > 0 else 0.0
print(f"SENT bytes={sent} elapsed_s={elapsed:.2f} offered_mbps={mbps:.3f}")
s.close()
