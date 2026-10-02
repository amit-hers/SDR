#!/usr/bin/env bash
set -euo pipefail
ROOT=${1:?repository root required}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
iverilog -g2012 -Wall -o "$WORK/tb" \
    "$ROOT/fpga/rtl/rs_encoder.v" "$ROOT/fpga/rtl/rs_syndrome.v" "$ROOT/fpga/rtl/rs_chien_search.v" \
    "$ROOT/fpga/rtl/rs_forney.v" "$ROOT/fpga/rtl/rs_berlekamp_massey.v" \
    "$ROOT/fpga/rtl/axi_rs_test_harness_hw.v" \
    "$ROOT/tests/tb_axi_rs_test_harness_hw.v"
vvp "$WORK/tb"
