#!/usr/bin/env bash
# Build sdr_bridge for the Pluto's ARMv7, and for the host as a compile check.
#
# STATIC, and with no liquid-dsp or OpenSSL. The bridge frames with FEC and
# cipher both nullptr, so the only references to Reed-Solomon and AES are the
# ones the compiler needs to resolve, not to run -- tests/arm/nodep_stubs.cpp
# satisfies those and aborts loudly if a path ever reaches them. Linking the
# real ones would drag both libraries onto the target, which is a packaging
# problem the wire path does not have.
#
# Static also because the Pluto's rootfs is a RAMDISK built against its own
# libstdc++; a dynamically linked binary is one firmware image away from not
# starting, and the failure appears at run time on the board.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${1:-$ROOT/build/sdr_bridge}"
OUT_BENCH="$(dirname "$OUT")/bridge_bench"

SOURCES=(
  "$ROOT/fpga/tools/sdr_bridge.cpp"
  "$ROOT/src/core/framing/Framer.cpp"
  "$ROOT/src/core/framing/Deframer.cpp"
  "$ROOT/tests/arm/nodep_stubs.cpp"
)

mkdir -p "$(dirname "$OUT")"

echo "== host build (compile check) =="
g++ -O2 -std=c++17 -Wall -Wextra -I "$ROOT/include" -pthread \
    -o "$OUT.host" "${SOURCES[@]}"
echo "   ok: $OUT.host"

XC=arm-linux-gnueabihf-g++
if ! command -v "$XC" >/dev/null; then
  echo "== ARM build SKIPPED: $XC not installed =="
  echo "   install with: sudo apt install g++-arm-linux-gnueabihf"
  exit 0
fi

echo "== ARMv7 build (the one that ships) =="
# Dynamic glibc, static libstdc++ -- NOT fully static.
#
# The only writable persistent storage on the board is mtd2, a 896 KB jffs2.
# A fully static binary is 542 KB, and because jffs2 keeps obsolete nodes until
# it can garbage-collect, rewriting one repeatedly leaves far less free space
# than the file listing suggests -- the write then stops short and leaves a
# TRUNCATED binary behind with no error from cat or cp. Linking glibc
# dynamically brings this to 120 KB, which fits with room to spare.
#
# libstdc++ stays static because the board's rootfs does not ship it; libc.so.6
# (Buildroot glibc 2.41) and ld-linux-armhf.so.3 are both present. Keep the
# toolchain's glibc no newer than the board's, since the symbol versions a
# binary records must exist at run time.
"$XC" -O2 -std=c++17 -Wall -Wextra -static-libstdc++ -static-libgcc \
    -I "$ROOT/include" -pthread \
    -o "$OUT" "${SOURCES[@]}"
file "$OUT" 2>/dev/null || true
echo "   ok: $OUT"

# The benchmark ships alongside it: whether the target CPU can sustain the
# four-offset decode is a property of the target, not of the developer's
# workstation, so it has to be measurable on the board.
BENCH_SOURCES=(
  "$ROOT/fpga/tools/bridge_bench.cpp"
  "$ROOT/src/core/framing/Framer.cpp"
  "$ROOT/src/core/framing/Deframer.cpp"
  "$ROOT/tests/arm/nodep_stubs.cpp"
)
"$XC" -O2 -std=c++17 -static -I "$ROOT/include" -o "$OUT_BENCH" "${BENCH_SOURCES[@]}"
echo "   ok: $OUT_BENCH"
