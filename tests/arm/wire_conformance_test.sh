#!/usr/bin/env bash
# The wire format must not depend on the machine that produced it.
#
# Two checks, and the second only runs where the tooling exists:
#
#  1. GOLDEN VECTORS. The encoded bytes are compared against a checked-in
#     reference, so an accidental change to a header field, a scrambler seed or
#     the CRC is caught here rather than as "the link stopped decoding".
#  2. CROSS-ARCHITECTURE. The same source is built for ARMv7 and run under qemu.
#     The two ends of this link will not be the same machine -- the daemon runs
#     on x86, the bridge is destined for the Pluto's ARMv7 -- and ARM differs on
#     things a wire format can easily depend on by accident: `long` and `size_t`
#     are 4 bytes rather than 8, and plain `char` is UNSIGNED. A format that
#     differed across them would fail as "the radio does not decode", which is
#     indistinguishable from an RF fault and would be chased as one.
set -uo pipefail
SRC_DIR="${1:?usage: wire_conformance_test.sh <source-dir>}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
GOLDEN="$SRC_DIR/tests/arm/wire_vectors.golden"

SOURCES=(
  "$SRC_DIR/tests/arm/wire_vectors.cpp"
  "$SRC_DIR/src/core/framing/Framer.cpp"
  "$SRC_DIR/src/core/telemetry/StatePacket.cpp"
  "$SRC_DIR/tests/arm/nodep_stubs.cpp"
)

g++ -O2 -std=c++17 -I "$SRC_DIR/include" -o "$TMP/host" "${SOURCES[@]}" \
  || { echo "FAIL: host build"; exit 1; }
"$TMP/host" | grep -E '^(statepacket|frame|scrambler)' > "$TMP/host.txt"

if ! diff -u "$GOLDEN" "$TMP/host.txt"; then
  echo "FAIL: encoded bytes differ from the checked-in reference."
  echo "      If the wire format was changed deliberately, regenerate:"
  echo "        tests/arm/wire_vectors.golden"
  echo "      and bump StatePacket::PROTO_VERSION if the change is not additive."
  exit 1
fi
echo "  host bytes match the golden vectors"

XC=arm-linux-gnueabihf-g++
if ! command -v "$XC" >/dev/null || ! command -v qemu-arm-static >/dev/null; then
  echo "  SKIP cross-architecture check (need $XC and qemu-arm-static)"
  echo "PASS"; exit 0
fi

"$XC" -O2 -std=c++17 -static -I "$SRC_DIR/include" -o "$TMP/arm" "${SOURCES[@]}" \
  || { echo "FAIL: ARM build"; exit 1; }
qemu-arm-static "$TMP/arm" | grep -E '^(statepacket|frame|scrambler)' > "$TMP/arm.txt" \
  || { echo "FAIL: ARM binary did not run"; exit 1; }

if ! diff -u "$TMP/host.txt" "$TMP/arm.txt"; then
  echo "FAIL: x86 and ARM produce different bytes for the same input."
  exit 1
fi
echo "  ARMv7 produces identical bytes (long/size_t are 4 bytes there, char is unsigned)"
echo "PASS"
