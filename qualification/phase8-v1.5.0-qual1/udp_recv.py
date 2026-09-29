#!/usr/bin/env python3
# Minimal UDP throughput receiver: counts unique sequence numbers to measure
# delivered rate and loss, independent of whatever the sender claims it sent.
import socket, struct, sys, time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 9999
duration = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("0.0.0.0", port))
s.settimeout(1.0)

seen = set()
bytes_total = 0
first_ts = None
last_ts = None
max_seq = -1

deadline = time.time() + duration + 3.0
while time.time() < deadline:
    try:
        data, addr = s.recvfrom(65535)
    except socket.timeout:
        if first_ts and time.time() - last_ts > 3.0:
            break
        continue
    if len(data) < 8:
        continue
    seq, = struct.unpack("!Q", data[:8])
    now = time.time()
    if first_ts is None:
        first_ts = now
    last_ts = now
    seen.add(seq)
    max_seq = max(max_seq, seq)
    bytes_total += len(data)

elapsed = (last_ts - first_ts) if (first_ts and last_ts and last_ts > first_ts) else 0.0
delivered = len(seen)
expected = max_seq + 1 if max_seq >= 0 else 0
lost = max(0, expected - delivered)
mbps = (bytes_total * 8 / 1e6 / elapsed) if elapsed > 0 else 0.0
print(f"RESULT delivered_unique={delivered} expected={expected} lost={lost} "
      f"loss_pct={(100.0*lost/expected if expected else 0):.2f} "
      f"bytes={bytes_total} elapsed_s={elapsed:.2f} delivered_mbps={mbps:.3f}")
