#!/usr/bin/env bash
set -euo pipefail
ROOT=${1:?repository root required}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

iverilog -g2012 -o "$WORK/tb" \
  "$ROOT/fpga/rtl/axis_packetizer.v" \
  "$ROOT/tests/tb_axis_packetizer_4k.v"
vvp "$WORK/tb"
