#!/usr/bin/env bash
set -euo pipefail
ROOT=${1:?repository root required}
PY=${2:-python3}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
"$PY" "$ROOT/tests/rs_decoder_ref.py" --self-check
"$PY" "$ROOT/tests/rs_decoder_ref.py" --bm-vectors 10 > "$WORK/vectors.hex"
csplit -z -f "$WORK/part" -s "$WORK/vectors.hex" '/^--$/' '/^--$/'
grep -v '^--$' "$WORK/part00" > "$WORK/synd.hex"
grep -v '^--$' "$WORK/part01" > "$WORK/degree.hex"
grep -v '^--$' "$WORK/part02" > "$WORK/coef.hex"
iverilog -g2012 -Wall -o "$WORK/tb" "$ROOT/fpga/rtl/rs_berlekamp_massey.v" "$ROOT/tests/tb_rs_berlekamp_massey.v"
vvp "$WORK/tb" "+synd=$WORK/synd.hex" "+degree=$WORK/degree.hex" "+coef=$WORK/coef.hex"
