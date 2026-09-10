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
"$XC" -O2 -std=c++17 -Wall -Wextra -static -I "$ROOT/include" -pthread \
    -o "$OUT" "${SOURCES[@]}"
file "$OUT" 2>/dev/null || true
echo "   ok: $OUT"
