#!/usr/bin/env python3
# Minimal TCP goodput receiver: accepts one connection, reads until the
# sender closes, reports bytes and elapsed time.
import socket, sys, time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 9998
timeout = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", port))
srv.listen(1)
srv.settimeout(timeout)
conn, addr = srv.accept()
conn.settimeout(timeout)
total = 0
t0 = time.time()
while True:
    try:
        data = conn.recv(65536)
    except socket.timeout:
        break
    if not data:
        break
    total += len(data)
elapsed = time.time() - t0
mbps = (total * 8 / 1e6 / elapsed) if elapsed > 0 else 0.0
print(f"RESULT bytes={total} elapsed_s={elapsed:.2f} goodput_mbps={mbps:.3f} from={addr}")
conn.close()
srv.close()
