#!/usr/bin/env bash
# Program a FIT into QSPI over JTAG, via the diagnostic u-boot's mailbox.
#
# WHY NOT flashcp: on the stock 5.10 kernel every mtd read and write lands
# 16 MiB further into the chip than asked. read_ear() returns -EINVAL for
# Winbond, so curbank stays 0 while the hardware EAR is 1, and
# spi_nor_write_ear() then short-circuits because ear == nor->curbank. Reads
# and writes share the same shift, so a readback compares equal and flashcp
# reports success having changed nothing at the address it named. 6.12 with
# the EAR patch is fine; 5.10 is not, which is exactly the state a unit is in
# before it has been cloned.
#
# The programmer runs in u-boot, loaded into DDR over JTAG -- it never boots
# from the flash it is rewriting. Contract in diagnostic-u-boot/common/main.c:
# it refuses any target but 0x00200000, refuses to erase unless the CRC of the
# image in DDR matches what the host expects, and bounds the erase inside mtd3.
#
# Usage:
#   program_fit.sh inspect            read-only: geometry, JEDEC, current head
#   program_fit.sh program IMAGE.frm  erase, write, verify
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OCD="$ROOT/fpga/jtag/zynq.cfg"
# The built programmer ELF. The 257 MB u-boot clone it came out of is not kept
# (see uboot/README.md for the patch and how to rebuild); this copy is.
UBOOT="$ROOT/uboot/u-boot.elf"
[[ -s "$UBOOT" ]] || UBOOT="$ROOT/diagnostic-u-boot-build/u-boot"

MB=0x03000000          # mailbox
FITA=0x08000000        # image staged here
TARGET=0x00200000      # start of mtd3; the programmer accepts nothing else
MB_LEN=140             # sizeof(struct pluto_prog_mailbox)

MODE="${1:-inspect}"
IMG="${2:-}"

[[ -s "$UBOOT" ]] || { echo "missing $UBOOT" >&2; exit 1; }
[[ -s "$OCD"   ]] || { echo "missing $OCD"   >&2; exit 1; }

FIT_LEN=0
FIT_CRC=0
RAW=""
if [[ "$MODE" == program ]]; then
    [[ -s "$IMG" ]] || { echo "usage: $0 program IMAGE.frm" >&2; exit 1; }
    # How long is the image? Ask the FIT, do not assume.
    #
    # A .frm MAY be a FIT followed by a 33-byte md5 trailer, and subtracting 33
    # unconditionally is wrong: the images built here are bare FITs whose file
    # size already equals totalsize, so the subtraction silently chopped the
    # last 33 bytes off the FDT. It still wrote and verified cleanly -- the
    # programmer faithfully stores whatever it is handed -- and the lost bytes
    # were trailing padding, so the result would probably have booted while not
    # being a byte-for-byte clone of the reference unit.
    #
    # totalsize is a big-endian u32 at offset 4 of the FDT header and is the
    # authoritative length. Anything past it is a trailer and is not written:
    # u-boot reads exactly fit_size bytes, so a trailer in flash is dead weight
    # past the image.
    TOTAL=$(stat -c%s "$IMG")
    head -c 4 "$IMG" | od -An -tx1 | tr -d ' \n' | grep -qi '^d00dfeed$' \
        || { echo "not a FIT (no d00dfeed magic at offset 0): $IMG" >&2; exit 1; }
    FIT_LEN=$(( 16#$(od -An -tx1 -j4 -N4 "$IMG" | tr -d ' \n') ))
    if (( FIT_LEN > TOTAL )); then
        echo "FIT totalsize $FIT_LEN exceeds file size $TOTAL: $IMG" >&2; exit 1
    fi
    RAW=$(mktemp /tmp/fit.XXXXXX.bin)
    trap 'rm -f "$RAW"' EXIT
    head -c "$FIT_LEN" "$IMG" > "$RAW"
    FIT_CRC=$(python3 -c "import zlib,sys;print(zlib.crc32(open(sys.argv[1],'rb').read())&0xffffffff)" "$RAW")
    printf 'image   : %s\n' "$IMG"
    printf 'fit_len : %d B from FDT totalsize (%d trailing B not written)\n' "$FIT_LEN" "$(( TOTAL - FIT_LEN ))"
    printf 'crc32   : 0x%08X\n' "$FIT_CRC"
fi

# ftdi_sio claims all four FT4232H channels; openocd needs channel A. The port
# is resolved, not hardcoded -- the debugger and the radios have swapped USB
# ports more than once, and a stale port number silently unbinds nothing.
FTDI_PORT=""
for p in /sys/bus/usb/devices/[0-9]*-[0-9]*; do
    [[ -f "$p/product" ]] || continue
    if grep -qi 'debug' "$p/product" 2>/dev/null; then FTDI_PORT="$(basename "$p")"; break; fi
done
if [[ -n "$FTDI_PORT" && -e "/sys/bus/usb/drivers/ftdi_sio/${FTDI_PORT}:1.0" ]]; then
    echo "unbinding ftdi_sio from ${FTDI_PORT}:1.0"
    echo "${FTDI_PORT}:1.0" | sudo -A tee /sys/bus/usb/drivers/ftdi_sio/unbind >/dev/null 2>&1 || true
fi

SEQ=$(mktemp); trap 'rm -f "$SEQ" ${RAW:-}' EXIT
{
cat <<OCDEOF
init
adapter speed 6000
targets zynq.cpu0
halt
# MMU off before touching SLCR: with it on, reads go through Linux's page
# tables and these registers are unmapped there (dfsr=5).
set s [arm mrc 15 0 1 0 0]
arm mcr 15 0 1 0 0 [expr {\$s & ~((1<<0)|(1<<2)|(1<<12))}]
# The watchdog is 10 s, reset-on-timeout, and nothing feeds it once Linux is
# halted. An erase takes longer than that: a reset landing mid-erase is how a
# unit ends up with a partially erased mtd3 and no bootable image.
mww 0xF8005000 0x00ABC000
# Clear the mailbox. Results from a previous run are still in DDR and read back
# as though this run produced them.
OCDEOF
for ((i=0;i<MB_LEN;i+=4)); do printf 'mww 0x%08X 0\n' $(( MB + i )); done
echo "load_image $UBOOT"
if [[ "$MODE" == program ]]; then
    echo "load_image $RAW $FITA bin"
    # Prove the image survived the JTAG transfer before anything is erased.
    echo "echo \"  ddr head: [format 0x%08X [lindex [read_memory $FITA 32 1] 0]] (expect 0xEDFE0DD0)\""
fi
cat <<OCDEOF
mww $(printf '0x%08X' $(( MB + 0x0C ))) $FIT_LEN
mww $(printf '0x%08X' $(( MB + 0x10 ))) $TARGET
mww $(printf '0x%08X' $(( MB + 0x14 ))) $FIT_CRC
OCDEOF
# Read the inputs BACK before resuming. A mailbox write that does not land
# leaves whatever the previous run put there, and the programmer then runs to
# a clean 0xD00D success against the WRONG length -- which is indistinguishable
# from a correct run unless the inputs are echoed here, before anything is
# erased.
cat <<OCDEOF
echo "  mailbox in: fit_len=[format %d [lindex [read_memory $(printf '0x%08X' $(( MB + 0x0C ))) 32 1] 0]] target=[format 0x%08X [lindex [read_memory $(printf '0x%08X' $(( MB + 0x10 ))) 32 1] 0]] expect_crc=[format 0x%08X [lindex [read_memory $(printf '0x%08X' $(( MB + 0x14 ))) 32 1] 0]]"
reg cpsr 0x000001d3
reg pc 0x04000000
resume
OCDEOF
# Erase and write of ~12 MB over QSPI is minutes, not seconds.
if [[ "$MODE" == program ]]; then echo "sleep 420000"; else echo "sleep 25000"; fi
cat <<OCDEOF
halt
echo "  ---- mailbox ----"
echo "  version      [format 0x%08X [lindex [read_memory $(printf '0x%08X' $(( MB + 0x08 ))) 32 1] 0]]"
echo "  status       [format 0x%08X [lindex [read_memory $(printf '0x%08X' $(( MB + 0x18 ))) 32 1] 0]]"
echo "  probe_result [format %d     [lindex [read_memory $(printf '0x%08X' $(( MB + 0x1C ))) 32 1] 0]]"
echo "  flash_size   [format 0x%08X [lindex [read_memory $(printf '0x%08X' $(( MB + 0x20 ))) 32 1] 0]]"
echo "  erase_size   [format 0x%08X [lindex [read_memory $(printf '0x%08X' $(( MB + 0x24 ))) 32 1] 0]]"
echo "  src_crc32    [format 0x%08X [lindex [read_memory $(printf '0x%08X' $(( MB + 0x2C ))) 32 1] 0]]"
echo "  erase_len    [format 0x%08X [lindex [read_memory $(printf '0x%08X' $(( MB + 0x30 ))) 32 1] 0]]"
echo "  erase_result [format %d     [lindex [read_memory $(printf '0x%08X' $(( MB + 0x34 ))) 32 1] 0]]"
echo "  write_result [format %d     [lindex [read_memory $(printf '0x%08X' $(( MB + 0x38 ))) 32 1] 0]]"
echo "  read_result  [format %d     [lindex [read_memory $(printf '0x%08X' $(( MB + 0x3C ))) 32 1] 0]]"
echo "  rb_crc32     [format 0x%08X [lindex [read_memory $(printf '0x%08X' $(( MB + 0x40 ))) 32 1] 0]]"
echo "  jedec+ear    [format 0x%08X [lindex [read_memory $(printf '0x%08X' $(( MB + 0x44 ))) 32 1] 0]] [format 0x%08X [lindex [read_memory $(printf '0x%08X' $(( MB + 0x48 ))) 32 1] 0]]"
echo "  old_head     [format 0x%08X [lindex [read_memory $(printf '0x%08X' $(( MB + 0x4C ))) 32 1] 0]] [format 0x%08X [lindex [read_memory $(printf '0x%08X' $(( MB + 0x50 ))) 32 1] 0]]"
echo "  new_head     [format 0x%08X [lindex [read_memory $(printf '0x%08X' $(( MB + 0x6C ))) 32 1] 0]] [format 0x%08X [lindex [read_memory $(printf '0x%08X' $(( MB + 0x70 ))) 32 1] 0]]"
shutdown
OCDEOF
} > "$SEQ"

echo "== openocd =="
stdbuf -oL -eL openocd -f "$OCD" -f "$SEQ" 2>&1 | stdbuf -oL sed 's/^/  /'
