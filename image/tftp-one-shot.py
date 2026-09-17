#!/usr/bin/env python3
"""Serve one file once over TFTP, for a RAM-only U-Boot validation load."""

import argparse
import pathlib
import socket
import struct
import time


def packet(opcode, block=0, payload=b""):
    return struct.pack("!HH", opcode, block & 0xFFFF) + payload


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("file", type=pathlib.Path)
    parser.add_argument("--bind", default="67.186.1.88")
    parser.add_argument("--port", type=int, default=1069)
    args = parser.parse_args()
    data = args.file.read_bytes()

    listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    listener.bind((args.bind, args.port))
    print(f"serving {args.file} ({len(data)} bytes) on {args.bind}:{args.port}", flush=True)
    request, peer = listener.recvfrom(4096)
    if len(request) < 4 or request[:2] != b"\x00\x01":
        raise SystemExit(f"unexpected request from {peer}: {request[:32]!r}")
    fields = request[2:].rstrip(b"\0").split(b"\0")
    print(f"RRQ from {peer[0]}:{peer[1]} for {fields[0].decode(errors='replace')}", flush=True)

    transfer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    transfer.connect(peer)
    transfer.settimeout(0.5)
    block_size = 512
    block = 1
    offset = 0
    started = time.monotonic()
    while True:
        chunk = data[offset:offset + block_size]
        message = packet(3, block, chunk)
        for attempt in range(20):
            transfer.send(message)
            try:
                ack = transfer.recv(64)
            except socket.timeout:
                continue
            if len(ack) >= 4 and ack[:2] == b"\x00\x04" and struct.unpack("!H", ack[2:4])[0] == (block & 0xFFFF):
                break
        else:
            raise SystemExit(f"no ACK for block {block}")
        offset += len(chunk)
        if block % 2048 == 0 or len(chunk) < block_size:
            elapsed = time.monotonic() - started
            print(f"{offset}/{len(data)} bytes ({offset / elapsed / 1024:.1f} KiB/s)", flush=True)
        if len(chunk) < block_size:
            break
        block = (block + 1) & 0xFFFF
    print("transfer complete", flush=True)


if __name__ == "__main__":
    main()
