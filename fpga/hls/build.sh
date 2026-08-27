#!/bin/bash
# Build one or all HLS cores for the PlutoSDR's Zynq-7000.
#
#   fpga/hls/build.sh                 # every core
#   fpga/hls/build.sh qpsk_modem      # just one
#
# 2026.1 retired the standalone `vivado_hls` command; HLS now runs through
# `vitis-run --mode hls`. Point XILINX_VITIS at a different install to use one.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
# Default to the COMPLETE install. There are two 2026.1 trees on this machine
# and only the nested one carries Vivado; sourcing the other leaves HLS without
# a Vivado to pair with and csynth_design fails with a bare "compilation
# failed" and no diagnostics, which is a long way from the actual cause.
VITIS="${XILINX_VITIS:-/home/amither/Documents/vivado/vivado/2026.1/Vitis}"

[ -f "$VITIS/settings64.sh" ] || {
  echo "build.sh: no Vitis at $VITIS (set XILINX_VITIS)" >&2; exit 1; }
# shellcheck disable=SC1091
. "$VITIS/settings64.sh"

# HLS needs to find Vivado for IP packaging. This install puts Vivado in a
# sibling tree the Vitis settings script does not know about, so without this
# every run prints "Default location for XILINX_VIVADO not found" and export
# would fail later, well past the point the cause is obvious.
if [ -z "${XILINX_VIVADO:-}" ]; then
  for cand in "$(dirname "$VITIS")/Vivado" \
              "$(dirname "$(dirname "$VITIS")")/vivado/2026.1/Vivado"; do
    [ -x "$cand/bin/vivado" ] && { export XILINX_VIVADO="$cand"; break; }
  done
fi
[ -n "${XILINX_VIVADO:-}" ] && echo "build.sh: XILINX_VIVADO=$XILINX_VIVADO"

command -v vitis-run >/dev/null || {
  echo "build.sh: vitis-run not on PATH after sourcing settings64.sh" >&2; exit 1; }

# Two things this build needs that a bare install does not provide, and which
# fail deep inside synthesis with unhelpful messages if they are missing.
# Checking here turns both into one clear sentence.
PARTS="$(dirname "$VITIS")/../vivado/2026.1/Vivado/data/parts/xilinx"
if [ -d "$PARTS" ] && ! ls "$PARTS" | grep -qi zynq; then
  echo "build.sh: WARNING Zynq-7000 device data is not installed." >&2
  echo "          Re-run the AMD installer and tick Devices > SoCs > Zynq-7000," >&2
  echo "          or synthesis for ${SDR_HLS_PART:-xc7z020clg400-2} will fail." >&2
fi
if [ -z "${XILINXD_LICENSE_FILE:-}" ] && [ -z "${LM_LICENSE_FILE:-}" ] \
   && ! ls "$HOME"/.Xilinx/*.lic >/dev/null 2>&1; then
  echo "build.sh: WARNING no licence found (~/.Xilinx/*.lic, XILINXD_LICENSE_FILE)." >&2
  echo "          Zynq-7020 is covered by the free Vivado ML Standard licence." >&2
fi

cores="${*:-rssi_meter gain_block sync_detector qpsk_modem}"
n_total=$(echo $cores | wc -w)
n_done=0
rc=0
t_all=$(date +%s)

# A sentinel, written once at the very end. Polling for "is a process still
# alive" is unreliable here: `pgrep -f` matches against full command lines and
# will happily match the polling command itself, so the watcher never exits.
# A file that appears exactly once is unambiguous.
DONE="${SDR_HLS_DONE:-/tmp/hls_build.done}"
rm -f "$DONE"

for c in $cores; do
  d="$HERE/$c"
  n_done=$((n_done + 1))
  [ -f "$d/hls_build.tcl" ] || { echo "build.sh: no such core '$c'" >&2; rc=1; continue; }
  t0=$(date +%s)
  printf '\n=== [%d/%d] %s ===\n' "$n_done" "$n_total" "$c"
  log="$d/build.log"
  # vitis-run resolves relative add_files against the working directory, so
  # run from the core's own directory the way the old flow did.
  #
  # Progress: HLS prints one line per phase. Echoing just those keeps the
  # console useful without the thousands of instruction-count lines.
  ( cd "$d" && vitis-run --mode hls --tcl hls_build.tcl 2>&1 | tee "$log" \
      | grep --line-buffered -E \
        "Running: (csynth_design|export_design|set_top|open_solution)|Finished (Source Code Analysis|Compiling Optim)|Estimated Fmax|Final II|Created IP archive|ERROR" \
      | sed -u "s/^INFO: \[[A-Z_0-9 -]*\] //; s/^/    /" ) || rc=1
  t1=$(date +%s)
  printf '    --- %s finished in %ss (full log: %s)\n' "$c" "$((t1 - t0))" "$log"
done

printf '\n=== all done in %ss, rc=%s ===\n' "$(( $(date +%s) - t_all ))" "$rc"
echo "$rc" > "$DONE"
exit $rc
