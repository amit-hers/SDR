#!/bin/sh
set -eu
root=$1
out=${TMPDIR:-/tmp}/sdr-axi-version-id-$$
trap 'rm -f "$out"' EXIT HUP INT TERM
iverilog -g2012 -o "$out" \
    "$root/fpga/rtl/axi_version_id.v" \
    "$root/tests/tb_axi_version_id.v"
vvp "$out"
