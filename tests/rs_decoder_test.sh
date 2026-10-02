#!/usr/bin/env bash
set -euo pipefail
ROOT=${1:?repository root required}
PY=${2:-python3}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
"$PY" "$ROOT/tests/rs_decoder_ref.py" --self-check
"$PY" "$ROOT/tests/rs_decoder_ref.py" --decoder-vectors 12 > "$WORK/vectors.hex"
csplit -z -f "$WORK/part" -s "$WORK/vectors.hex" '/^--$/' '/^--$/' '/^--$/'
grep -v '^--$' "$WORK/part00" > "$WORK/recv.hex"
grep -v '^--$' "$WORK/part01" > "$WORK/exp.hex"
grep -v '^--$' "$WORK/part02" > "$WORK/fail.hex"
grep -v '^--$' "$WORK/part03" > "$WORK/errcnt.hex"
iverilog -g2012 -Wall -o "$WORK/tb" \
    "$ROOT/fpga/rtl/rs_syndrome.v" "$ROOT/fpga/rtl/rs_berlekamp_massey.v" \
    "$ROOT/fpga/rtl/rs_chien_search.v" "$ROOT/fpga/rtl/rs_forney.v" \
    "$ROOT/fpga/rtl/rs_decoder.v" "$ROOT/tests/tb_rs_decoder.v"
vvp "$WORK/tb" "+recv=$WORK/recv.hex" "+exp=$WORK/exp.hex" \
    "+fail=$WORK/fail.hex" "+errcnt=$WORK/errcnt.hex"
