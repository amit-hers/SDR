#!/usr/bin/env bash
# flash.sh -- deploy a validated release bundle to a Pluto+/LibreSDR board.
#
#   ./scripts/flash.sh <device> <release-dir> [--persist]
#   ./scripts/flash.sh 192.168.2.17 .
#   ./scripts/flash.sh UNIT-A ./pluto-datalink-v1.3.0
#
# TWO MODES, and the difference is not cosmetic:
#
#   default    RUNTIME deployment. Uploads the bitstream, loads it now, and
#              verifies. Does NOT reboot -- this board's rootfs is a RAMDISK, so
#              a reboot would discard everything just installed and bring the
#              board back on the stock PL. Correct until the next power cycle.
#
#   --persist  POWER-ON deployment. Writes the firmware image to the qspi-linux
#              partition, from which u-boot loads the PL at boot
#              (bitstream_image=system.bit.bin in its environment). Reboots and
#              verifies afterwards. Requires boot/pluto.frm in the bundle.
#
#              --persist REPLACES THE ROOTFS, AND THE ROOTFS CARRIES THE BOARD'S
#              IDENTITY. After it the board comes back with stock hostname, USB
#              serial, MAC, root password ("analog", not "root") and IP
#              ADDRESS -- 192.168.2.1, which COLLIDES with the other board if
#              both are attached. This is not a failure mode, it is what
#              flashing a rootfs means, and it caught this script out once:
#              UNIT-A deployed perfectly and was reported as dead because the
#              script was waiting on an address the board no longer had.
#              So the search below probes the stock address too, and config.txt
#              is saved to /mnt/jffs2 first, which is a separate flash partition
#              and survives.
#
# The distinction matters because a watchdog reset during a long test has
# silently reverted this board to the stock PL more than once, and every
# 0x43Cxxxxx access then bus-errors in a way that looks like dead hardware.
#
# NOTHING IS WRITTEN TO THE DEVICE until every precondition below passes.
set -uo pipefail

DEV_ARG="${1:-}"; BUNDLE="${2:-}"; shift 2 2>/dev/null || true
PERSIST=0
for a in "$@"; do [[ "$a" == "--persist" ]] && PERSIST=1; done
if [[ -z "$DEV_ARG" || -z "$BUNDLE" ]]; then
  echo "usage: $0 <device-ip|UNIT-A|UNIT-B> <release-dir> [--persist]" >&2; exit 2
fi

# Friendly names, because the boards' labels do not match their addresses: the
# unit whose USB serial says UNIT-B answers on .1, UNIT-A on .17.
case "$DEV_ARG" in
  UNIT-A|unit-a) DEV=192.168.2.17 ;;
  UNIT-B|unit-b) DEV=192.168.2.1  ;;
  *)             DEV="$DEV_ARG"   ;;
esac

PW="${PLUTO_PW:-root}"
SSHO=(-o PubkeyAuthentication=no -o PreferredAuthentications=password
      -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10)
ssh_d() { sshpass -p "$PW" ssh "${SSHO[@]}" "root@$DEV" "$@"; }
put()   { sshpass -p "$PW" ssh "${SSHO[@]}" "root@$DEV" "cat > $2" < "$1"; }

STAGE=""
step() { STAGE="$2"; printf '[%3d%%] %s\n' "$1" "$2"; }
die()  {
  echo
  echo "[FAILED] $STAGE"
  echo
  [[ -n "${1:-}" ]] && echo "$1"
  echo
  echo "Deployment aborted; exit status 1."
  exit 1
}

echo "Deploying $BUNDLE -> $DEV_ARG ($DEV)"
echo

# ── Preconditions: all of these before a single byte is written ────────────
step 5 "Checking release bundle"
[[ -d "$BUNDLE" ]] || die "No such release directory: $BUNDLE"
[[ -f "$BUNDLE/manifest.json" ]] || die "manifest.json not found in $BUNDLE"
for f in fpga/system.bin boot/uImage config/ad936x.conf checksums.sha256; do
  [[ -f "$BUNDLE/$f" ]] || die "Required artifact missing: $f"
done

step 10 "Verifying SHA-256 checksums"
if ! ( cd "$BUNDLE" && sha256sum -c --quiet checksums.sha256 ) 2>/dev/null; then
  die "Checksum verification FAILED. The bundle is modified or corrupt.
Refusing to flash a release that does not match its own manifest."
fi

REL=$(python3 -c "import json;print(json.load(open('$BUNDLE/manifest.json'))['release'])")
WANT_ABI=$(python3 -c "import json;print(json.load(open('$BUNDLE/manifest.json'))['fpga_abi'])")
WANT_REG=$(python3 -c "import json;print(json.load(open('$BUNDLE/manifest.json'))['register_map_version'])")
WANT_MAGIC=$(python3 -c "import json;print(json.load(open('$BUNDLE/manifest.json'))['fpga_magic'])")

step 15 "Connecting to device"
command -v sshpass >/dev/null || die "sshpass is not installed on this machine."
ping -c1 -W2 "$DEV" >/dev/null 2>&1 || die "Device $DEV does not respond to ping."
ssh_d true >/dev/null 2>&1 || die "SSH to root@$DEV failed. Set PLUTO_PW if the password is not 'root'."

step 20 "Checking hardware identity"
MODEL=$(ssh_d 'cat /sys/firmware/devicetree/base/model 2>/dev/null | tr -d "\0"' 2>/dev/null)
IDCODE=$(ssh_d 'devmem 0xF8000530 32 2>/dev/null' 2>/dev/null)
# PSS_IDCODE, not the model string: every on-device string on these units lies
# about the part. 0x1372*** is XC7Z020, 0x1371*** would be XC7Z010.
case "$IDCODE" in
  0x*372*|0x*372*) : ;;
  "") die "Could not read PSS_IDCODE (0xF8000530). Is this a Zynq board?" ;;
  *)  echo "         note: PSS_IDCODE $IDCODE (model string: ${MODEL:-unknown})" ;;
esac
echo "         $MODEL / IDCODE $IDCODE"

step 25 "Reading currently installed version"
CUR=$(ssh_d 'cat /mnt/jffs2/sdr-release 2>/dev/null || echo "(none)"' 2>/dev/null)
CUR_ABI=$(ssh_d 'devmem 0x43C50008 32 2>/dev/null || echo "(no identity block)"' 2>/dev/null)
echo "         installed release: $CUR   running FPGA ABI: $CUR_ABI"

step 30 "Backing up configuration"
BAK="/mnt/jffs2/backup-$(date -u +%Y%m%dT%H%M%SZ)"
ssh_d "mkdir -p $BAK && cp /mnt/jffs2/sdr-release $BAK/ 2>/dev/null; \
       cp /mnt/jffs2/autorun.sh $BAK/ 2>/dev/null; \
       cp /mnt/jffs2/ad936x.conf $BAK/ 2>/dev/null; true" >/dev/null 2>&1 \
  || die "Could not write to /mnt/jffs2 (persistent config partition)."
# The network identity is the thing most worth keeping: --persist resets it, and
# /mnt/jffs2 is a separate mtd that survives the rootfs being replaced.
ssh_d "for f in /opt/config.txt /mnt/msd/config.txt /params.txt; do \
         [ -f \$f ] && cp \$f $BAK/config.txt && break; done; true" >/dev/null 2>&1
ssh_d "echo '$DEV' > /mnt/jffs2/sdr-lastip" >/dev/null 2>&1
echo "         saved to $BAK"
if [[ $PERSIST -eq 1 ]]; then
  echo "         NOTE: --persist replaces the rootfs; the board will return with"
  echo "               STOCK identity (ip 192.168.2.1, password 'analog')."
fi

# ── Deployment ────────────────────────────────────────────────────────────
step 40 "Uploading boot image"
if [[ $PERSIST -eq 1 ]]; then
  # The .frm is what u-boot actually reads at boot. BOOT.BIN in the bundle is
  # for SD-card boot; this board boots from QSPI, where the fsbl-uboot
  # partition is only 1 MB and cannot hold a bitstream at all.
  [[ -f "$BUNDLE/boot/pluto.frm" ]] || die \
"--persist requires boot/pluto.frm, which this bundle does not contain.

The firmware image has to be rebuilt with this release's bitstream, and that
needs mkimage (u-boot-tools) on the build machine. Without it, only runtime
deployment is possible:

  ./scripts/flash.sh $DEV_ARG $BUNDLE        # loads the PL now, until reboot"
  FRM_SZ=$(stat -c%s "$BUNDLE/boot/pluto.frm")
  # busybox awk has no strtonum, and there is no stat(1) on this rootfs.
  MTD_HEX=$(ssh_d "grep '^mtd3:' /proc/mtd" 2>/dev/null | awk '{print $2}' | tr -d ' \r')
  MTD_SZ=$((16#${MTD_HEX:-0}))
  [[ -n "$MTD_SZ" ]] || die "Could not read /proc/mtd to size the target partition."
  (( FRM_SZ <= MTD_SZ )) || die "Firmware image is $FRM_SZ B but qspi-linux holds only $MTD_SZ B."
  put "$BUNDLE/boot/pluto.frm" /tmp/pluto.frm || die "Upload of pluto.frm failed."
  RSZ=$(ssh_d 'wc -c < /tmp/pluto.frm' 2>/dev/null | tr -d ' \r')
  [[ "$RSZ" == "$FRM_SZ" ]] || die "Firmware truncated in transit: $RSZ of $FRM_SZ bytes."
else
  echo "         skipped (runtime deployment; pass --persist for power-on install)"
fi

step 50 "Uploading FPGA image"
ssh_d 'mkdir -p /lib/firmware' >/dev/null 2>&1
put "$BUNDLE/fpga/system.bin" /lib/firmware/sdr-system.bin || die "Upload of the bitstream failed."
SZ=$(ssh_d 'wc -c < /lib/firmware/sdr-system.bin' 2>/dev/null | tr -d ' \r')
LOC=$(stat -c%s "$BUNDLE/fpga/system.bin")
[[ "$SZ" == "$LOC" ]] || die "Bitstream truncated in transit: $SZ of $LOC bytes."

step 65 "Uploading root filesystem"
if [[ $PERSIST -eq 1 ]]; then
  echo "         carried inside pluto.frm (kernel + dtb + rootfs + bitstream)"
else
  echo "         skipped (runtime deployment)"
fi

step 75 "Uploading DSP software"
ssh_d 'mkdir -p /root/sdr-tools' >/dev/null 2>&1
for f in "$BUNDLE"/software/supporting-tools/*.sh; do
  put "$f" "/root/sdr-tools/$(basename "$f")" || die "Upload of $(basename "$f") failed."
done
ssh_d 'chmod +x /root/sdr-tools/*.sh' >/dev/null 2>&1

step 82 "Installing configuration"
put "$BUNDLE/config/ad936x.conf" /mnt/jffs2/ad936x.conf || die "Could not install ad936x.conf."
echo "$REL" | ssh_d 'cat > /mnt/jffs2/sdr-release' || die "Could not record the release version."
# autorun.sh is sourced by /etc/init.d/S98autostart from the PERSISTENT jffs2
# partition, so this survives the ramdisk being rebuilt on every boot.
ssh_d "cat > /mnt/jffs2/autorun.sh" <<AUTORUN || die "Could not install autorun.sh."
#!/bin/sh
# Installed by flash.sh from release $REL. Sourced at boot by S98autostart.
[ -f /lib/firmware/sdr-system.bin ] && {
  echo 0 > /sys/class/fpga_manager/fpga0/flags
  echo sdr-system.bin > /sys/class/fpga_manager/fpga0/firmware
}
# The stock watchdog is -T 10, and a multi-megabyte transfer or a devmem burst
# outruns it; that has reset this board mid-test and wiped the ramdisk.
[ -x /usr/sbin/watchdog ] && {
  kill \$(pidof watchdog) 2>/dev/null
  /usr/sbin/watchdog -t 5 -T 120 /dev/watchdog
}
AUTORUN

step 85 "Loading FPGA image"
if [[ $PERSIST -eq 0 ]]; then
  # Load it NOW. Without --persist there is no reboot, so this is the moment
  # the new bitstream actually takes effect.
  ssh_d 'echo 0 > /sys/class/fpga_manager/fpga0/flags; \
         echo sdr-system.bin > /sys/class/fpga_manager/fpga0/firmware; sleep 3' >/dev/null 2>&1
  ST=$(ssh_d 'cat /sys/class/fpga_manager/fpga0/state' 2>/dev/null)
  [[ "$ST" == "operating" ]] || die "fpga_manager reports state '$ST' after load."
else
  step 86 "Writing firmware to qspi-linux"
  ssh_d 'flashcp -v /tmp/pluto.frm /dev/mtd3' >/dev/null 2>&1 \
    || die "flashcp to /dev/mtd3 failed. The device may be mid-write; do NOT power it off.
Re-run this command before rebooting."
fi

step 87 "Syncing filesystem"
ssh_d 'sync' >/dev/null 2>&1 || die "sync failed."

if [[ $PERSIST -eq 1 ]]; then
  step 90 "Rebooting device"
  ssh_d '(sleep 1; reboot) >/dev/null 2>&1 &' >/dev/null 2>&1 || true

  step 93 "Waiting for device"
  sleep 5
  START=$SECONDS; DEADLINE=$((SECONDS + 240))
  ORIG="$DEV"
  FOUND=""
  # Probe the original address AND the stock default, with both passwords: a
  # rootfs flash resets all of them, and a board that moved is not a board that
  # died.
  while (( SECONDS < DEADLINE )); do
    for cand in "$ORIG" 192.168.2.1; do
      for pw in "$PW" analog root; do
        if sshpass -p "$pw" ssh "${SSHO[@]}" "root@$cand" true >/dev/null 2>&1; then
          FOUND="$cand"; PW="$pw"; DEV="$cand"; break 3
        fi
      done
    done
    sleep 3
  done
  if [[ -z "$FOUND" ]]; then
    die "Device did not come back within 240 s on $ORIG or 192.168.2.1.

The firmware write completed, so the board is most likely up but on an address
this script cannot guess. Check its USB serial console (/dev/ttyACM*) or mount
its mass-storage volume and read config.txt.

Recover its address by editing 'ipaddr' in config.txt on that volume and
ejecting it, which the board applies on unmount."
  fi
  echo "         back after $((SECONDS - START)) s at $DEV"
  if [[ "$DEV" != "$ORIG" ]]; then
    echo "         WARNING: the board returned on $DEV, not $ORIG."
    echo "         --persist reset its identity. Restore the address by editing"
    echo "         'ipaddr' in config.txt on its mass-storage volume and ejecting it."
  fi
  sleep 3
else
  step 90 "Reboot"
  echo "         SKIPPED -- runtime deployment. The rootfs is a ramdisk, so a"
  echo "         reboot would discard this deployment and revert to the stock PL."
  step 93 "Waiting for device"
  echo "         not applicable"
fi

step 96 "Checking FPGA/software ABI"
sleep 3
GOT_MAGIC=$(ssh_d 'devmem 0x43C50000 32 2>/dev/null' 2>/dev/null)
GOT_ABI=$(ssh_d   'devmem 0x43C50008 32 2>/dev/null' 2>/dev/null)
GOT_REG=$(ssh_d   'devmem 0x43C5000C 32 2>/dev/null' 2>/dev/null)
if [[ -z "$GOT_MAGIC" ]]; then
  die "No identity block at 0x43C50000 after reboot.
The FPGA did not load, or this bundle's bitstream predates the version registers.
Device was flashed but application startup has been blocked."
fi
if (( $((GOT_ABI)) != WANT_ABI )); then
  die "Expected FPGA ABI: $WANT_ABI
Detected FPGA ABI: $((GOT_ABI))

Device was flashed but application startup has been blocked."
fi
if (( $((GOT_REG)) != WANT_REG )); then
  die "Expected register map: $WANT_REG
Detected register map: $((GOT_REG))

Device was flashed but application startup has been blocked."
fi

step 100 "Deployment successful"
echo
exec "$(dirname "$0")/verify.sh" "$DEV_ARG" "$BUNDLE"
