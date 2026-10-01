#!/usr/bin/env bash
set -euo pipefail
ROOT=${1:?repository root required}
PY=${2:-python3}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
"$PY" "$ROOT/tests/rs_decoder_ref.py" --self-check
"$PY" "$ROOT/tests/rs_decoder_ref.py" --syndrome-vectors 8 > "$WORK/vectors.hex"
csplit -z -f "$WORK/part" -s "$WORK/vectors.hex" '/^--$/'
grep -v '^--$' "$WORK/part00" > "$WORK/recv.hex"
grep -v '^--$' "$WORK/part01" > "$WORK/synd.hex"
iverilog -g2012 -Wall -o "$WORK/tb" "$ROOT/fpga/rtl/rs_syndrome.v" "$ROOT/tests/tb_rs_syndrome.v"
vvp "$WORK/tb" "+recv=$WORK/recv.hex" "+synd=$WORK/synd.hex"
