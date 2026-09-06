#!/usr/bin/env bash
# verify.sh -- post-deployment verification. Safe to run at any time.
#   ./scripts/verify.sh <device> <release-dir>
#
# Checks the device against the bundle's OWN manifest rather than against
# constants baked in here, so a bundle can still be verified years later when
# the expected values have moved on.
set -uo pipefail
DEV_ARG="${1:-}"; BUNDLE="${2:-.}"
[[ -n "$DEV_ARG" ]] || { echo "usage: $0 <device> <release-dir>" >&2; exit 2; }
case "$DEV_ARG" in
  UNIT-A|unit-a) DEV=192.168.2.17 ;;
  UNIT-B|unit-b) DEV=192.168.2.1  ;;
  *)             DEV="$DEV_ARG"   ;;
esac
PW="${PLUTO_PW:-root}"
SSHO=(-o PubkeyAuthentication=no -o PreferredAuthentications=password
      -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10)
ssh_d() { sshpass -p "$PW" ssh "${SSHO[@]}" "root@$DEV" "$@" 2>/dev/null; }

J() { python3 -c "import json;print(json.load(open('$BUNDLE/manifest.json'))['$1'])"; }
WANT_REL=$(J release); WANT_ABI=$(J fpga_abi); WANT_REG=$(J register_map_version)
WANT_SWABI=$(J software_abi); WANT_MAGIC=$(J fpga_magic)

FAILED=0
row() {
  local label="$1" value="$2" ok="$3"
  if [[ "$ok" == "PASS" ]]; then printf '%-20s %-12s %s\n' "$label" "$value" "PASS"
  else printf '%-20s %-12s %s\n' "$label" "$value" "FAIL"; FAILED=1; fi
}

echo
ssh_d true >/dev/null || { echo "Device $DEV unreachable over SSH."; echo; echo "DEPLOYMENT RESULT: FAIL"; exit 1; }

GOT_REL=$(ssh_d 'cat /mnt/jffs2/sdr-release 2>/dev/null')
row "Release:" "${GOT_REL:-none}" "$([[ "$GOT_REL" == "$WANT_REL" ]] && echo PASS || echo FAIL)"

FPGA_STATE=$(ssh_d 'cat /sys/class/fpga_manager/fpga0/state 2>/dev/null')
row "FPGA image:" "${FPGA_STATE:-unknown}" "$([[ "$FPGA_STATE" == "operating" ]] && echo PASS || echo FAIL)"

MAGIC=$(ssh_d 'devmem 0x43C50000 32')
# Compare numerically: devmem prints 0xXXXXXXXX, the manifest may not be padded.
if [[ -n "$MAGIC" ]] && (( $((MAGIC)) == $((WANT_MAGIC)) )); then M=PASS; else M=FAIL; fi
row "FPGA magic:" "${MAGIC:-absent}" "$M"

ABI=$(ssh_d 'devmem 0x43C50008 32')
row "FPGA ABI:" "$([[ -n "$ABI" ]] && echo $((ABI)) || echo absent)" \
    "$([[ -n "$ABI" ]] && (( $((ABI)) == WANT_ABI )) && echo PASS || echo FAIL)"
row "Software ABI:" "$WANT_SWABI" "$([[ "$WANT_SWABI" == "$WANT_ABI" ]] && echo PASS || echo FAIL)"

REG=$(ssh_d 'devmem 0x43C5000C 32')
row "Register map:" "$([[ -n "$REG" ]] && echo $((REG)) || echo absent)" \
    "$([[ -n "$REG" ]] && (( $((REG)) == WANT_REG )) && echo PASS || echo FAIL)"

IIO=$(ssh_d 'ls /sys/bus/iio/devices/ 2>/dev/null | wc -l')
PHY=$(ssh_d 'cat /sys/bus/iio/devices/iio:device0/name 2>/dev/null')
row "IIO/AD936x:" "${PHY:-absent}" "$([[ "$PHY" == ad9361-phy ]] && echo PASS || echo FAIL)"

# A modem sanity check that costs nothing: the demodulator's control window must
# respond. A stock PL bus-errors here, which is the single most common way a
# board looks "deployed" while running the wrong bitstream entirely.
DEMOD=$(ssh_d 'devmem 0x43C00000 32')
row "Modem core:" "${DEMOD:-no response}" "$([[ -n "$DEMOD" ]] && echo PASS || echo FAIL)"

WD=$(ssh_d 'ps | grep -c "[w]atchdog -t"')
row "Watchdog:" "$([[ "${WD:-0}" -gt 0 ]] && echo running || echo absent)" \
    "$([[ "${WD:-0}" -gt 0 ]] && echo PASS || echo FAIL)"

echo
if (( FAILED )); then echo "DEPLOYMENT RESULT: FAIL"; exit 1; fi
echo "DEPLOYMENT RESULT: PASS"
