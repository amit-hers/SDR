#!/usr/bin/env python3
"""Send one file to U-Boot's loady command using YMODEM-1K."""

import argparse
import pathlib
import serial
import sys
import time

SOH, STX, EOT, ACK, NAK, CAN, CRC = 0x01, 0x02, 0x04, 0x06, 0x15, 0x18, 0x43


def crc16(data):
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def frame(number, data, size):
    data = data.ljust(size, b"\0")
    crc = crc16(data)
    return bytes((SOH if size == 128 else STX, number & 0xFF, 0xFF - (number & 0xFF))) + data + crc.to_bytes(2, "big")


def wait_for(port, wanted, timeout=30):
    deadline = time.monotonic() + timeout
    seen = bytearray()
    while time.monotonic() < deadline:
        chunk = port.read(256)
        if chunk:
            seen.extend(chunk)
            sys.stdout.buffer.write(chunk)
            sys.stdout.buffer.flush()
            for byte in chunk:
                if byte in wanted:
                    return byte
                if byte == CAN:
                    raise RuntimeError("receiver cancelled transfer")
    raise TimeoutError(f"timeout waiting for {wanted}; tail={bytes(seen[-120:])!r}")


def send_with_retry(port, payload, expected=(ACK,), attempts=20):
    for _ in range(attempts):
        port.write(payload)
        answer = wait_for(port, set(expected) | {NAK}, timeout=10)
        if answer in expected:
            return answer
    raise RuntimeError("receiver repeatedly rejected block")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("device")
    parser.add_argument("file", type=pathlib.Path)
    parser.add_argument("--address", default="0x08000000")
    args = parser.parse_args()
    data = args.file.read_bytes()
    port = serial.Serial(args.device, 115200, timeout=0.1, write_timeout=10)
    port.reset_input_buffer()
    port.write(f"loady {args.address}\r".encode())
    wait_for(port, {CRC}, timeout=15)

    name = args.file.name.encode()
    header = name + b"\0" + str(len(data)).encode() + b"\0"
    send_with_retry(port, frame(0, header, 128))
    wait_for(port, {CRC}, timeout=10)

    started = time.monotonic()
    total_blocks = (len(data) + 1023) // 1024
    for index in range(total_blocks):
        chunk = data[index * 1024:(index + 1) * 1024]
        send_with_retry(port, frame(index + 1, chunk, 1024))
        if (index + 1) % 512 == 0 or index + 1 == total_blocks:
            sent = min((index + 1) * 1024, len(data))
            elapsed = time.monotonic() - started
            print(f"\n{sent}/{len(data)} bytes ({100*sent/len(data):.1f}%, {sent/elapsed/1024:.1f} KiB/s)", flush=True)

    port.write(bytes((EOT,)))
    answer = wait_for(port, {ACK, NAK}, timeout=10)
    if answer == NAK:
        port.write(bytes((EOT,)))
        wait_for(port, {ACK}, timeout=10)
    wait_for(port, {CRC}, timeout=10)
    send_with_retry(port, frame(0, b"", 128))
    print("\nYMODEM transfer complete", flush=True)
    time.sleep(1)
    tail = port.read(4096)
    sys.stdout.buffer.write(tail)
    port.close()


if __name__ == "__main__":
    main()
