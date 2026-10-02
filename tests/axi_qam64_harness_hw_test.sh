#!/usr/bin/env bash
set -euo pipefail
ROOT=${1:?repository root required}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
iverilog -g2012 -Wall -o "$WORK/tb" \
    "$ROOT/fpga/rtl/qam64_mapper.v" "$ROOT/fpga/rtl/qam64_demapper.v" \
    "$ROOT/fpga/rtl/axi_qam64_test_harness_hw.v" \
    "$ROOT/tests/tb_axi_qam64_test_harness_hw.v"
vvp "$WORK/tb"
