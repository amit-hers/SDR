#!/usr/bin/env bash
set -euo pipefail
ROOT=${1:?repository root required}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
iverilog -g2012 -Wall -o "$WORK/tb" "$ROOT/fpga/rtl/qam64_mapper.v" \
    "$ROOT/fpga/rtl/qam64_demapper.v" "$ROOT/tests/tb_qam64.v"
if [[ -n "${2:-}" ]]; then
    "$2" --rtl-vectors > "$WORK/vectors.hex"
    vvp "$WORK/tb" "+vectors=$WORK/vectors.hex"
else
    vvp "$WORK/tb"
fi
