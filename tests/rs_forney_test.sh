#!/usr/bin/env bash
set -euo pipefail
ROOT=${1:?repository root required}
PY=${2:-python3}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
"$PY" "$ROOT/tests/rs_decoder_ref.py" --self-check
"$PY" "$ROOT/tests/rs_decoder_ref.py" --forney-vectors 12 > "$WORK/vectors.hex"
csplit -z -f "$WORK/part" -s "$WORK/vectors.hex" '/^--$/' '/^--$/' '/^--$/' '/^--$/' '/^--$/'
grep -v '^--$' "$WORK/part00" > "$WORK/coef.hex"
grep -v '^--$' "$WORK/part01" > "$WORK/lens.hex"
grep -v '^--$' "$WORK/part02" > "$WORK/synd.hex"
grep -v '^--$' "$WORK/part03" > "$WORK/cnts.hex"
grep -v '^--$' "$WORK/part04" > "$WORK/degs.hex"
grep -v '^--$' "$WORK/part05" > "$WORK/mags.hex"
iverilog -g2012 -Wall -o "$WORK/tb" "$ROOT/fpga/rtl/rs_forney.v" "$ROOT/tests/tb_rs_forney.v"
vvp "$WORK/tb" "+coef=$WORK/coef.hex" "+lens=$WORK/lens.hex" "+synd=$WORK/synd.hex" \
    "+cnts=$WORK/cnts.hex" "+degs=$WORK/degs.hex" "+mags=$WORK/mags.hex"
