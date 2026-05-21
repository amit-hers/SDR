#!/usr/bin/env bash
# =============================================================================
# HamGeek Pluto+ (Zynq-7020) Full QSPI Recovery Script
# =============================================================================
# Usage:
#   ./recover_plutoplus.sh --working-ip 192.168.2.17
#   ./recover_plutoplus.sh --working-ip 192.168.2.17 --dfu-only
#   ./recover_plutoplus.sh --dfu-only   (if partitions already extracted)
#
# What it does:
#   Phase 1 — Extract all 4 MTD partitions from a working Pluto+ over SSH
#   Phase 2 — Build the ARM bare-metal QSPI flasher (requires arm gcc)
#   Phase 3 — Flash mtd0 (FSBL+U-Boot) via JTAG using OpenOCD
#   Phase 4 — Wait for broken device to enter DFU mode
#   Phase 5 — Flash mtd1 (uboot-env) + mtd3 (Linux) via dfu-util
#
# Requirements:
#   arm-linux-gnueabihf-gcc   openocd   dfu-util   sshpass
#   FT4232H JTAG adapter connected to broken device (for Phase 3)
#   Working Pluto+ reachable over SSH (for Phase 1)
# =============================================================================

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ── Colour helpers ────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'
info()    { echo -e "${CYAN}[*]${NC} $*"; }
ok()      { echo -e "${GREEN}[✓]${NC} $*"; }
warn()    { echo -e "${YELLOW}[!]${NC} $*"; }
die()     { echo -e "${RED}[✗]${NC} $*" >&2; exit 1; }
banner()  { echo -e "\n${BOLD}=== $* ===${NC}"; }

# ── Defaults ──────────────────────────────────────────────────────────────────
WORKING_IP=""
WORKING_PASS="root"
JTAG_CFG="$SCRIPT_DIR/zynq7020_ft4232.cfg"
RECOVERY_TCL="$SCRIPT_DIR/full_recovery.tcl"
FLASHER_C="$SCRIPT_DIR/qspi_flasher.c"
DFU_ONLY=0
EXTRACT_ONLY=0
SKIP_EXTRACT=0

MTD0="$SCRIPT_DIR/flash_good.bin"   # boot (FSBL+U-Boot, 1MB)
MTD1="$SCRIPT_DIR/uboot_env.bin"    # uboot-env (128KB)
MTD2="$SCRIPT_DIR/nvmfs.bin"        # nvmfs (896KB)
MTD3="$SCRIPT_DIR/linux.bin"        # Linux kernel+rootfs (30MB)

PLUTO_VID_PID="0456:b674"
DFU_TIMEOUT=120   # seconds to wait for DFU mode after JTAG flash

# ── Argument parsing ──────────────────────────────────────────────────────────
usage() {
    cat <<EOF
Usage: $0 [OPTIONS]

  --working-ip IP     IP of a working Pluto+ to extract firmware from
  --working-pass PASS SSH password (default: root)
  --jtag-cfg FILE     OpenOCD JTAG config (default: zynq7020_ft4232.cfg)
  --dfu-only          Skip JTAG phase, only do DFU flashing
  --extract-only      Only extract partitions from working device, then stop
  --skip-extract      Use existing partition files, skip extraction
  -h, --help          Show this help

Examples:
  # Full recovery (JTAG + DFU):
  sudo $0 --working-ip 192.168.2.17

  # If boot partition already OK, device in DFU, just reflash Linux:
  sudo $0 --working-ip 192.168.2.17 --dfu-only

  # Just extract firmware from working device for backup:
  $0 --working-ip 192.168.2.17 --extract-only
EOF
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --working-ip)    WORKING_IP="$2";   shift 2 ;;
        --working-pass)  WORKING_PASS="$2"; shift 2 ;;
        --jtag-cfg)      JTAG_CFG="$2";     shift 2 ;;
        --dfu-only)      DFU_ONLY=1;        shift   ;;
        --extract-only)  EXTRACT_ONLY=1;    shift   ;;
        --skip-extract)  SKIP_EXTRACT=1;    shift   ;;
        -h|--help)       usage ;;
        *) die "Unknown option: $1" ;;
    esac
done

# ── Prerequisite checks ───────────────────────────────────────────────────────
banner "Checking prerequisites"

check_cmd() {
    if command -v "$1" &>/dev/null; then ok "$1 found"; else
        if [[ "${2:-required}" == "required" ]]; then
            die "$1 not found — install it first"
        else
            warn "$1 not found (optional)"
        fi
    fi
}

check_cmd sshpass optional
if [[ -z "$WORKING_IP" || $SKIP_EXTRACT -eq 1 ]]; then
    info "Skipping sshpass check (no extraction needed)"
fi

if [[ $DFU_ONLY -eq 0 ]]; then
    check_cmd arm-linux-gnueabihf-gcc required
    check_cmd arm-linux-gnueabihf-objcopy required
    check_cmd openocd required
fi
check_cmd dfu-util required

if [[ $EUID -ne 0 && $EXTRACT_ONLY -eq 0 ]]; then
    warn "Not running as root — dfu-util may fail without udev rules"
    warn "Re-run with: sudo $0 $*"
fi

# ── Phase 1: Extract partitions from working device ───────────────────────────
if [[ $SKIP_EXTRACT -eq 0 ]]; then
    banner "Phase 1 — Extracting firmware from working device ($WORKING_IP)"

    [[ -z "$WORKING_IP" ]] && die "--working-ip is required for extraction (or use --skip-extract)"

    # Check SSH connectivity
    info "Testing SSH connection to $WORKING_IP..."
    if ! sshpass -p "$WORKING_PASS" ssh -o StrictHostKeyChecking=no \
            -o ConnectTimeout=10 root@"$WORKING_IP" "echo ok" &>/dev/null; then
        die "Cannot SSH to $WORKING_IP — check connection and password"
    fi
    ok "SSH connected to $WORKING_IP"

    MTD_INFO=$(sshpass -p "$WORKING_PASS" ssh -o StrictHostKeyChecking=no \
        root@"$WORKING_IP" "cat /proc/mtd")
    info "MTD layout on working device:"
    echo "$MTD_INFO" | sed 's/^/    /'

    dump_mtd() {
        local dev="$1" outfile="$2" label="$3"
        info "Copying $label ($dev) → $(basename "$outfile")..."
        sshpass -p "$WORKING_PASS" ssh -o StrictHostKeyChecking=no \
            root@"$WORKING_IP" "dd if=$dev bs=4096 2>/dev/null" > "$outfile"
        local sz
        sz=$(du -h "$outfile" | cut -f1)
        ok "$label → $(basename "$outfile") ($sz)"
    }

    dump_mtd /dev/mtd0 "$MTD0" "mtd0 FSBL+U-Boot"
    dump_mtd /dev/mtd1 "$MTD1" "mtd1 uboot-env"
    dump_mtd /dev/mtd2 "$MTD2" "mtd2 nvmfs"
    dump_mtd /dev/mtd3 "$MTD3" "mtd3 Linux"

    echo ""
    info "Partition checksums (save these for verification):"
    md5sum "$MTD0" "$MTD1" "$MTD2" "$MTD3"

    [[ $EXTRACT_ONLY -eq 1 ]] && { ok "Extract complete. Files saved to $SCRIPT_DIR"; exit 0; }
else
    banner "Phase 1 — Skipping extraction (using existing files)"
    for f in "$MTD0" "$MTD3"; do
        [[ -f "$f" ]] || die "Missing required file: $f (run without --skip-extract first)"
        ok "Found $(basename "$f") ($(du -h "$f" | cut -f1))"
    done
fi

# ── Phase 2: Build QSPI flasher binary ───────────────────────────────────────
if [[ $DFU_ONLY -eq 0 ]]; then
    banner "Phase 2 — Building ARM QSPI flasher"

    [[ -f "$FLASHER_C" ]] || die "Missing $FLASHER_C"

    arm-linux-gnueabihf-gcc -O0 -mthumb -march=armv7-a -mfloat-abi=soft \
        -nostdlib -nostartfiles -ffunction-sections -fdata-sections \
        -Wl,--gc-sections,-Ttext=0x00020000,--entry=main \
        -o "$SCRIPT_DIR/qspi_flasher.elf" "$FLASHER_C"

    arm-linux-gnueabihf-objcopy -O binary --only-section=.text \
        "$SCRIPT_DIR/qspi_flasher.elf" "$SCRIPT_DIR/qspi_flasher.bin"

    SZ=$(wc -c < "$SCRIPT_DIR/qspi_flasher.bin")
    ok "qspi_flasher.bin built ($SZ bytes)"

    # Verify program_chunk entry point
    PC=$(arm-linux-gnueabihf-nm "$SCRIPT_DIR/qspi_flasher.elf" 2>/dev/null | \
         grep -w program_chunk | awk '{print "0x"$1}' || echo "unknown")
    info "program_chunk entry: $PC"
fi

# ── Phase 3: Flash mtd0 via JTAG ─────────────────────────────────────────────
if [[ $DFU_ONLY -eq 0 ]]; then
    banner "Phase 3 — Flashing FSBL+U-Boot (mtd0) via JTAG"

    [[ -f "$JTAG_CFG" ]]     || die "Missing JTAG config: $JTAG_CFG"
    [[ -f "$RECOVERY_TCL" ]] || die "Missing recovery TCL: $RECOVERY_TCL"

    info "Starting OpenOCD — this takes ~10 minutes for 32 chunks"
    info "Log: /tmp/recovery_jtag.log"
    echo ""

    openocd -f "$JTAG_CFG" \
        -c "source $RECOVERY_TCL; init; after 1000; qspi_recover; shutdown" \
        2>&1 | tee /tmp/recovery_jtag.log

    if grep -q "DONE! flash_good.bin written" /tmp/recovery_jtag.log; then
        ok "JTAG flash complete — mtd0 written successfully"
    else
        die "JTAG flash failed — check /tmp/recovery_jtag.log"
    fi

    echo ""
    info "Waiting for device to reboot into DFU mode..."
    info "The device should appear as 'USB download gadget' within 30 seconds"
fi

# ── Phase 4: Wait for DFU device ─────────────────────────────────────────────
banner "Phase 4 — Waiting for DFU device ($PLUTO_VID_PID)"

ELAPSED=0
while ! dfu-util -l 2>/dev/null | grep -q "$PLUTO_VID_PID"; do
    if [[ $ELAPSED -ge $DFU_TIMEOUT ]]; then
        die "Timed out waiting for DFU device after ${DFU_TIMEOUT}s.
    Make sure the broken device is powered and USB is connected.
    If the device is already in DFU mode, re-run with --dfu-only."
    fi
    printf "\r    Waiting... %ds / %ds" "$ELAPSED" "$DFU_TIMEOUT"
    sleep 2
    ELAPSED=$((ELAPSED + 2))
done
echo ""

# Show available DFU targets
DFU_LIST=$(dfu-util -l 2>/dev/null | grep "$PLUTO_VID_PID")
ok "DFU device found:"
echo "$DFU_LIST" | sed 's/^/    /'

# Identify alt numbers by name
get_alt() {
    echo "$DFU_LIST" | grep "\"$1\"" | grep -o 'alt=[0-9]*' | grep -o '[0-9]*' | head -1
}

ALT_BOOT=$(get_alt "boot.dfu")
ALT_FW=$(get_alt "firmware.dfu")
ALT_ENV=$(get_alt "uboot-env.dfu")
ALT_XENV=$(get_alt "uboot-extra-env.dfu")

info "DFU alt mapping: boot=${ALT_BOOT:-?} firmware=${ALT_FW:-?} uboot-env=${ALT_ENV:-?} uboot-extra-env=${ALT_XENV:-?}"

# ── Phase 5: DFU flash ────────────────────────────────────────────────────────
banner "Phase 5 — DFU flashing partitions"

dfu_flash() {
    local alt="$1" file="$2" label="$3"
    [[ -z "$alt" ]] && { warn "Alt number unknown for $label — skipping"; return; }
    [[ -f "$file" ]] || { warn "Missing file $file — skipping $label"; return; }
    local sz
    sz=$(du -h "$file" | cut -f1)
    info "Flashing $label (alt=$alt, $sz)..."
    if dfu-util -a "$alt" -D "$file" 2>&1 | tee /tmp/dfu_${alt}.log | \
            grep -qE "Download done|state\(7\)"; then
        ok "$label flashed OK"
    else
        warn "$label may have issues — check /tmp/dfu_${alt}.log"
    fi
}

# Flash Linux firmware (largest, most important)
dfu_flash "$ALT_FW"   "$MTD3" "Linux (firmware.dfu, 30MB)"

# Flash U-Boot environment
dfu_flash "$ALT_ENV"  "$MTD1" "uboot-env (128KB)"

# Flash nvmfs if present
if [[ -f "$MTD2" && -n "${ALT_XENV:-}" ]]; then
    warn "Flashing nvmfs — this contains calibration data from the DONOR device"
    warn "If broken device had valid calibration, skip this (Ctrl-C now, 5s...)"
    sleep 5
    dfu_flash "$ALT_XENV" "$MTD2" "nvmfs/uboot-extra-env (896KB)"
fi

# ── Reset device ──────────────────────────────────────────────────────────────
banner "Phase 6 — Resetting device"
info "Sending DFU detach to reboot..."
dfu-util -a 0 -e 2>/dev/null || true
ok "Reset command sent"

# ── Done ──────────────────────────────────────────────────────────────────────
echo ""
echo -e "${GREEN}${BOLD}=================================================================${NC}"
echo -e "${GREEN}${BOLD}  Recovery complete!${NC}"
echo -e "${GREEN}${BOLD}=================================================================${NC}"
echo ""
echo "  The device is rebooting. In ~30 seconds it should appear as:"
echo "    • USB network interface  (IP: 192.168.2.1)"
echo "    • USB serial port        (/dev/ttyACM0)"
echo ""
echo "  Verify with:"
echo "    ping 192.168.2.1"
echo "    ssh root@192.168.2.1     (password: analog)"
echo ""
echo "  Partition backups saved in: $SCRIPT_DIR"
echo "    flash_good.bin  $(du -h "$MTD0" 2>/dev/null | cut -f1)   FSBL+U-Boot"
echo "    uboot_env.bin   $(du -h "$MTD1" 2>/dev/null | cut -f1)  U-Boot environment"
[[ -f "$MTD2" ]] && echo "    nvmfs.bin       $(du -h "$MTD2" 2>/dev/null | cut -f1)  nvmfs (calibration)"
echo "    linux.bin       $(du -h "$MTD3" 2>/dev/null | cut -f1)  Linux kernel+rootfs"
