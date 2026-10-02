#!/usr/bin/env bash
set -euo pipefail
ROOT=${1:?repository root required}
PY=${2:-python3}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
"$PY" "$ROOT/tests/rs_decoder_ref.py" --self-check
"$PY" "$ROOT/tests/rs_decoder_ref.py" --chien-vectors 12 > "$WORK/vectors.hex"
csplit -z -f "$WORK/part" -s "$WORK/vectors.hex" '/^--$/' '/^--$/' '/^--$/'
grep -v '^--$' "$WORK/part00" > "$WORK/coef.hex"
grep -v '^--$' "$WORK/part01" > "$WORK/lens.hex"
grep -v '^--$' "$WORK/part02" > "$WORK/cnts.hex"
grep -v '^--$' "$WORK/part03" > "$WORK/degs.hex"
iverilog -g2012 -Wall -o "$WORK/tb" "$ROOT/fpga/rtl/rs_chien_search.v" "$ROOT/tests/tb_rs_chien_search.v"
vvp "$WORK/tb" "+coef=$WORK/coef.hex" "+lens=$WORK/lens.hex" "+cnts=$WORK/cnts.hex" "+degs=$WORK/degs.hex"
