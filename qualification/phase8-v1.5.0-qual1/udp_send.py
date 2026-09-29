#!/usr/bin/env python3
# Minimal UDP throughput sender: fixed offered rate (Mbps) and packet size,
# for a fixed duration. Sequence-numbers each packet so the receiver can
# measure UNIQUE delivered count independent of what this side claims.
import socket, struct, sys, time

host = sys.argv[1]
port = int(sys.argv[2])
mbps = float(sys.argv[3])
size = int(sys.argv[4]) if len(sys.argv) > 4 else 1200
duration = float(sys.argv[5]) if len(sys.argv) > 5 else 10.0

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
payload_pad = b"Q" * max(0, size - 8)
pps = (mbps * 1e6 / 8) / size
interval = 1.0 / pps if pps > 0 else 0.001

seq = 0
sent_bytes = 0
t_start = time.time()
next_send = t_start
end = t_start + duration
while time.time() < end:
    now = time.time()
    if now < next_send:
        continue
    pkt = struct.pack("!Q", seq) + payload_pad
    try:
        s.sendto(pkt, (host, port))
        sent_bytes += len(pkt)
    except OSError:
        pass
    seq += 1
    next_send += interval

elapsed = time.time() - t_start
offered_mbps = sent_bytes * 8 / 1e6 / elapsed if elapsed > 0 else 0
print(f"SENT seq_count={seq} bytes={sent_bytes} elapsed_s={elapsed:.2f} offered_mbps={offered_mbps:.3f}")
