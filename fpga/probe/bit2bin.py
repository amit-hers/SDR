#!/usr/bin/env python3
"""Convert a Vivado .bit into the byte-swapped .bin the Zynq FPGA manager wants.

A .bit carries a small tagged header (fields a..d = design/part/date strings)
before field 'e', which holds the raw configuration data. The Zynq fpga-mgr
expects that raw data with each 32-bit word byte-swapped.
"""
import struct
import sys


def bit2bin(src, dst):
    d = open(src, "rb").read()
    p = 0
    hlen = struct.unpack(">H", d[p:p + 2])[0]; p += 2 + hlen
    p += 2                                            # 0x0001
    meta = {}
    while p < len(d):
        key = chr(d[p]); p += 1
        if key == "e":
            n = struct.unpack(">I", d[p:p + 4])[0]; p += 4
            raw = d[p:p + n]
            break
        n = struct.unpack(">H", d[p:p + 2])[0]; p += 2
        meta[key] = d[p:p + n].rstrip(b"\x00").decode("ascii", "replace"); p += n
    else:
        raise SystemExit("no 'e' field: not a .bit file")

    if len(raw) % 4:
        raise SystemExit(f"bitstream length {len(raw)} is not a multiple of 4")
    swapped = b"".join(raw[i:i + 4][::-1] for i in range(0, len(raw), 4))
    # sanity: the Xilinx sync word must be present once byte-swapped
    if swapped.find(b"\x66\x55\x99\xaa") < 0:
        raise SystemExit("sync word AA995566 not found after swap -- bad parse")
    open(dst, "wb").write(swapped)
    print(f"design={meta.get('a')} part={meta.get('b')} "
          f"date={meta.get('c')} {meta.get('d')}")
    print(f"wrote {dst}: {len(swapped)} bytes")


if __name__ == "__main__":
    bit2bin(sys.argv[1], sys.argv[2])
