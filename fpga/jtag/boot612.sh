#!/usr/bin/env bash
# Boot the 6.12 kernel on a Pluto over JTAG, into RAM, without touching flash.
#
#   fpga/jtag/boot612.sh [attempts]
#
# WHY THIS EXISTS: nothing written from Linux changes what these boards boot --
# ADI's own USB-drive update path was tried and the board still came up on 5.10
# (see the linux-flash-access-is-broken note). Until DFU or the u-boot console
# is available, this is the only way to run 6.12 on the hardware.
#
# NOT PERSISTENT. A power cycle returns the board to whatever is in flash.
#
# Requires: the board freshly power-cycled and running its flash kernel (a
# failed attempt leaves it wedged, and booting from a wedged state always
# fails), plus the Xilinx JTAG cable.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SC="${SDR_SCRATCH:?set SDR_SCRATCH to the directory holding kbuild612/ and rd612/}"
ATTEMPTS="${1:-4}"
OCD="$ROOT/fpga/jtag/zynq.cfg"

KERNEL="$SC/kbuild612/arch/arm/boot/zImage"
RD="$SC/rd612/rd-nostdcpp.cpio.gz"
DTB="$SC/boot.dtb"
for f in "$KERNEL" "$RD" "$DTB"; do
  [[ -s "$f" ]] || { echo "missing: $f" >&2; exit 1; }
done

# ftdi_sio claims all four FT4232H channels; openocd needs channel A.
if [[ -e /dev/ttyUSB0 ]]; then
  echo 1-7:1.0 | sudo tee /sys/bus/usb/drivers/ftdi_sio/unbind >/dev/null 2>&1 || true
fi

RDEND=$(( 0x02200000 + $(stat -c%s "$RD") ))
printf 'kernel %s B, initrd %s B -> 0x02200000..0x%08X\n' \
  "$(stat -c%s "$KERNEL")" "$(stat -c%s "$RD")" "$RDEND"

SEQ=$(mktemp); trap 'rm -f "$SEQ"' EXIT
cat > "$SEQ" <<OCDEOF
init
adapter speed 6000
targets zynq.cpu0
halt
# MMU off FIRST: with it on, openocd reads go through Linux's page tables and
# the SLCR/watchdog registers below are unmapped there (dfsr=5).
set s [arm mrc 15 0 1 0 0]
arm mcr 15 0 1 0 0 [expr {\$s & ~((1<<0)|(1<<2)|(1<<12))}]
# Watchdog: 10 s, reset-on-timeout. Nothing feeds it once Linux is halted.
mww 0xF8005000 0x00ABC000
# L2C-310: u-boot cleans and disables it before entering Linux.
mww 0xF8F0207C 0x0000FFFF
mww 0xF8F02100 0x00000000
load_image $KERNEL 0x00008000 bin
load_image $DTB    0x02000000 bin
load_image $RD     0x02200000 bin
echo "  kernel=[format 0x%08X [lindex [read_memory 0x00008000 32 1] 0]] dtb=[format 0x%08X [lindex [read_memory 0x02000000 32 1] 0]] initrd=[format 0x%08X [lindex [read_memory 0x02200000 32 1] 0]]"
reg cpsr 0x000001d3
reg r0 0
reg r1 0xffffffff
reg r2 0x02000000
reg pc 0x00008000
resume
sleep 20000
shutdown
OCDEOF

for a in $(seq 1 "$ATTEMPTS"); do
  echo "=== attempt $a/$ATTEMPTS ==="
  sudo openocd -f "$OCD" -f "$SEQ" 2>&1 | grep -E "kernel=|Error: (data|Target)" | sed 's/^/  /'
  for i in $(seq 1 10); do
    sleep 4
    if [[ -d /sys/bus/usb/devices/1-8 || -d /sys/bus/usb/devices/1-6 ]]; then
      echo "  BOOTED -- board enumerated"
      echo "  find it with: scripts/pluto_netns.sh setup, then ssh root@192.168.2.17"
      echo "  (6.12 derives a UNIQUE MAC from the SPI-NOR serial, so the"
      echo "   duplicate-MAC namespace workaround is not needed on it)"
      exit 0
    fi
  done
  echo "  no enumeration"
  # A failed attempt leaves the CPU in the decompressor's halt loop or an abort;
  # booting again from there fails, so say so rather than silently retrying.
  echo "  NOTE: the board is now dirty. A power cycle is required before the"
  echo "        next attempt has a real chance of succeeding."
done
echo "FAILED after $ATTEMPTS attempts"
exit 1
