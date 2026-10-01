#!/usr/bin/env bash
set -euo pipefail
ROOT=${1:?repository root required}
PY=${2:-python3}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
"$PY" "$ROOT/tests/rs_encoder_ref.py" --self-check
"$PY" "$ROOT/tests/rs_encoder_ref.py" --vectors 5 > "$WORK/vectors.hex"
csplit -z -f "$WORK/part" -s "$WORK/vectors.hex" '/^--$/'
grep -v '^--$' "$WORK/part00" > "$WORK/data.hex"
grep -v '^--$' "$WORK/part01" > "$WORK/expected.hex"
iverilog -g2012 -Wall -o "$WORK/tb" "$ROOT/fpga/rtl/rs_encoder.v" "$ROOT/tests/tb_rs_encoder.v"
vvp "$WORK/tb" "+data=$WORK/data.hex" "+expected=$WORK/expected.hex"
