#!/usr/bin/env bash
# build-release.sh -- assemble one deployable bundle for the Pluto+ datalink.
#
#   ./release/build-release.sh <version> [dev|rc|release|golden]
#   ./release/build-release.sh v1.3.0 release
#
# Produces  out/pluto-datalink-<version>/          the bundle tree
#           out/pluto-datalink-<version>.tar.zst   the artifact
#           out/pluto-datalink-<version>.sha256    its checksum
#
# The whole point is that a bundle can be restored to a board years later with
# no access to this machine, this toolchain, or this Vivado install. So it
# carries the entire stack -- boot image, kernel, device tree, rootfs, FPGA,
# software, config and the scripts to install them -- plus enough metadata to
# say exactly which sources produced it.
#
# GIT PROVENANCE IS NOT EDITABLE. The commit fields come from `git rev-parse`
# and nothing else; there is no flag to override them. A release whose recorded
# revision does not match its contents is worse than no metadata at all,
# because it will be trusted.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

VERSION="${1:-}"
BUILD_TYPE="${2:-dev}"
if [[ -z "$VERSION" ]]; then
  echo "usage: $0 <version> [dev|rc|release|golden]" >&2; exit 2
fi
case "$BUILD_TYPE" in dev|rc|release|golden) ;; *)
  echo "ERROR: build type must be dev, rc, release or golden" >&2; exit 2 ;;
esac

# shellcheck source=versions.env
source "$ROOT/release/versions.env"

# ── Provenance, derived and never supplied ────────────────────────────────
SW_COMMIT="$(git rev-parse --short=7 HEAD)"
SW_COMMIT_FULL="$(git rev-parse HEAD)"
FPGA_COMMIT="$(git log -1 --format=%h --abbrev=7 -- fpga/ || echo "$SW_COMMIT")"
BUILD_DATE="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
BUILD_EPOCH="$(date -u +%s)"
GIT_TAG="$(git describe --exact-match --tags 2>/dev/null || echo '')"
DIRTY=""
if ! git diff-index --quiet HEAD -- 2>/dev/null; then DIRTY="+dirty"; fi

# A permanent artifact must be reproducible from an immutable ref. A tree with
# uncommitted edits, or no tag, cannot be rebuilt by anyone else -- which is the
# one property a recovery image has to have.
if [[ "$BUILD_TYPE" == "release" || "$BUILD_TYPE" == "golden" ]]; then
  if [[ -n "$DIRTY" ]]; then
    echo "ERROR: working tree is dirty; a $BUILD_TYPE build must be reproducible from git." >&2
    echo "       Commit or stash, then tag." >&2
    exit 1
  fi
  if [[ -z "$GIT_TAG" ]]; then
    echo "ERROR: HEAD is not at an annotated tag; a $BUILD_TYPE build must come from one." >&2
    echo "       git tag -a $VERSION -m 'Validated Pluto datalink release $VERSION'" >&2
    exit 1
  fi
  if [[ "$GIT_TAG" != "$VERSION" ]]; then
    echo "ERROR: HEAD is tagged '$GIT_TAG' but '$VERSION' was requested." >&2
    exit 1
  fi
fi

NAME="pluto-datalink-${VERSION}"
OUT="$ROOT/out"
BUNDLE="$OUT/$NAME"
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE"/{fpga,boot,rootfs,software/supporting-tools,config,scripts}

say() { printf '  %-34s %s\n' "$1" "$2"; }
need() { [[ -e "$1" ]] || { echo "ERROR: missing required input: $1" >&2; exit 1; }; }

echo "Building $NAME ($BUILD_TYPE)"
echo "  software commit $SW_COMMIT$DIRTY   fpga commit $FPGA_COMMIT"

# ── FPGA ──────────────────────────────────────────────────────────────────
HDL="${SDR_HDL_DIR:-$HOME/Documents/adi-hdl/projects/libre}"
BIT="$HDL/libre.runs/impl_1/system_top.bit"
need "$BIT"
cp "$BIT" "$BUNDLE/fpga/system.bit";                 say "fpga/system.bit" "$(stat -c%s "$BIT") B"
python3 "$ROOT/fpga/probe/bit2bin.py" "$BIT" "$BUNDLE/fpga/system.bin" >/dev/null
say "fpga/system.bin" "$(stat -c%s "$BUNDLE/fpga/system.bin") B (fpga_manager format)"
XSA="$HDL/libre.sdk/system_top.xsa"
[[ -f "$XSA" ]] && cp "$XSA" "$BUNDLE/fpga/system.xsa" && say "fpga/system.xsa" "present"

# BOOT.BIN carries the bitstream that the FSBL loads at POWER-ON. Without it the
# board comes up on the stock PL and every 0x43Cxxxxx access bus-errors, which
# is exactly the state a recovery image has to be able to fix.
FW="$ROOT/tezuka-plutoplus-v0.3.5-7cf6171"
if [[ -x "${BOOTGEN:-}" || -x "$(command -v bootgen 2>/dev/null || echo /nonexistent)" ]]; then
  BG="${BOOTGEN:-$(command -v bootgen)}"
  WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
  # FSBL and u-boot are lifted out of the stock boot image rather than rebuilt:
  # their sources are not in this tree, and they are not what we are changing.
  # Offsets and lengths come from the partition table, in 32-bit words.
  dd if="$FW/sdimg/BOOT.bin" of="$WORK/fsbl.bin"  bs=1 skip=$((0x5c0*4))   count=$((0x6002*4))  status=none
  dd if="$FW/sdimg/BOOT.bin" of="$WORK/uboot.bin" bs=1 skip=$((0x4ecd0*4)) count=$((0x277f6*4)) status=none
  cp "$BIT" "$WORK/system_top.bit"
  cat > "$WORK/boot.bif" <<BIF
the_ROM_image:
{
    [bootloader, load=0x00000000, startup=0x00000000] fsbl.bin
    system_top.bit
    [load=0x04000000, startup=0x04000000] uboot.bin
}
BIF
  if (cd "$WORK" && "$BG" -image boot.bif -arch zynq -o BOOT.BIN -w on >/dev/null 2>&1); then
    cp "$WORK/BOOT.BIN" "$BUNDLE/fpga/BOOT.BIN"
    say "fpga/BOOT.BIN" "$(stat -c%s "$BUNDLE/fpga/BOOT.BIN") B (loads the PL at power-on)"
  else
    echo "  WARNING: bootgen failed; bundle will carry the STOCK BOOT.bin." >&2
    cp "$FW/sdimg/BOOT.bin" "$BUNDLE/fpga/BOOT.BIN"
    say "fpga/BOOT.BIN" "STOCK -- modem PL will NOT load at power-on"
  fi
else
  echo "  WARNING: no bootgen on PATH; bundle will carry the STOCK BOOT.bin." >&2
  cp "$FW/sdimg/BOOT.bin" "$BUNDLE/fpga/BOOT.BIN"
  say "fpga/BOOT.BIN" "STOCK -- modem PL will NOT load at power-on"
fi

# ── Boot / kernel / rootfs ────────────────────────────────────────────────
need "$FW/sdimg/uImage"; need "$FW/sdimg/devicetree.dtb"; need "$FW/sdimg/uramdisk.image.gz"
cp "$FW/sdimg/uImage"          "$BUNDLE/boot/uImage";          say "boot/uImage" "$(stat -c%s "$FW/sdimg/uImage") B"
cp "$FW/sdimg/devicetree.dtb"  "$BUNDLE/boot/devicetree.dtb";  say "boot/devicetree.dtb" "ok"
cp "$FW/sdimg/uEnv.txt"        "$BUNDLE/boot/uEnv.txt" 2>/dev/null || true
[[ -f "$FW/boot.frm" ]] && cp "$FW/boot.frm" "$BUNDLE/boot/boot.frm" && say "boot/boot.frm" "ok"
cp "$FW/sdimg/uramdisk.image.gz" "$BUNDLE/rootfs/rootfs.image.gz"; say "rootfs/rootfs.image.gz" "$(stat -c%s "$FW/sdimg/uramdisk.image.gz") B"

# The firmware image is what gives POWER-ON persistence: u-boot loads the PL
# from its `fpga` sub-image before the kernel. Without this the bundle can only
# be deployed at runtime, because a reboot rebuilds the ramdisk and takes the
# bitstream with it. Kernel, device tree and ramdisk are carried across from the
# stock image unchanged -- nothing here rebuilds them, and shipping a different
# kernel than the one a release was validated against would make the bundle a
# worse record than none.
if command -v mkimage >/dev/null && command -v dumpimage >/dev/null; then
  if "$ROOT/release/make-frm.sh" "$BUNDLE/fpga/system.bin" "$BUNDLE/boot/pluto.frm" >/dev/null 2>&1; then
    say "boot/pluto.frm" "$(stat -c%s "$BUNDLE/boot/pluto.frm") B (power-on persistence)"
  else
    echo "  WARNING: make-frm.sh failed; --persist will be unavailable for this bundle." >&2
  fi
else
  echo "  WARNING: mkimage/dumpimage not found; --persist will be unavailable." >&2
  echo "           apt-get install u-boot-tools" >&2
fi

# ── Software ──────────────────────────────────────────────────────────────
# Host-side: this daemon drives the Pluto over IIO; it is not an ARM binary.
if [[ -x "$ROOT/build/src/daemon/sdr-datalink" ]]; then
  cp "$ROOT/build/src/daemon/sdr-datalink" "$BUNDLE/software/datalink"; say "software/datalink" "host x86-64"
else
  echo "  WARNING: build/src/daemon/sdr-datalink not built; software/datalink omitted." >&2
fi
if [[ -x "$ROOT/build/tests/sdr-tests" ]]; then
  cp "$ROOT/build/tests/sdr-tests" "$BUNDLE/software/modem_test"; say "software/modem_test" "host x86-64"
fi
# Device-side: the bring-up and measurement scripts ARE the on-board software.
cp "$ROOT"/fpga/scripts/*.sh "$BUNDLE/software/supporting-tools/"
cp "$ROOT/fpga/tools/framed_link_test.cpp" "$BUNDLE/software/supporting-tools/"
cp "$ROOT/fpga/probe/bit2bin.py" "$BUNDLE/software/supporting-tools/"
say "software/supporting-tools" "$(ls "$BUNDLE/software/supporting-tools" | wc -l) files"

# ── Config ────────────────────────────────────────────────────────────────
cp "$ROOT/config.json" "$BUNDLE/config/modem.conf"
cp "$ROOT/release/versions.env" "$BUNDLE/config/versions.env"
cat > "$BUNDLE/config/ad936x.conf" <<CONF
# AD936x / modem defaults for this release. Consumed by supporting-tools.
SAMPLE_RATE=17280000
TX_LO=434000000
RX_LO=434000000
RF_BANDWIDTH=4000000
RX_GAIN_MODE=slow_attack
TX_HARDWAREGAIN=0
ADC_CHAN_FORMAT=0x51
DAC_DATARATE=1
DIFF_MODE=1
PKT_BYTES=32768
CONF
say "config/" "modem.conf ad936x.conf versions.env"

# ── Scripts ───────────────────────────────────────────────────────────────
cp "$ROOT/release/templates/flash.sh"    "$BUNDLE/scripts/flash.sh"
cp "$ROOT/release/templates/verify.sh"   "$BUNDLE/scripts/verify.sh"
cp "$ROOT/release/templates/rollback.sh" "$BUNDLE/scripts/rollback.sh"
chmod +x "$BUNDLE/scripts/"*.sh
say "scripts/" "flash.sh verify.sh rollback.sh"

# ── Manifest ──────────────────────────────────────────────────────────────
python3 - "$BUNDLE" <<PY
import json, sys, os
b = sys.argv[1]
m = {
  "product": "pluto-datalink",
  "release": "${VERSION}",
  "build_type": "${BUILD_TYPE}",
  "software_commit": "${SW_COMMIT}",
  "software_commit_full": "${SW_COMMIT_FULL}",
  "fpga_commit": "${FPGA_COMMIT}",
  "git_tag": "${GIT_TAG}",
  "tree_dirty": bool("${DIRTY}"),
  "build_date_utc": "${BUILD_DATE}",
  "build_epoch": ${BUILD_EPOCH},
  "board": "Pluto+ XC7Z020",
  "fpga_abi": ${SDR_FPGA_ABI},
  "software_abi": ${SDR_SW_ABI},
  "register_map_version": ${SDR_REGMAP_VER},
  "fpga_magic": "${SDR_MAGIC}",
  "fpga_id_base": "${SDR_ID_BASE}",
  "fpga_version": "${SDR_FPGA_VERSION}",
  "fpga_clock_mhz": 35,
  "default_sample_rate": 17280000,
  "default_modulation": "QPSK differential",
  "retention": {"dev":"14d","rc":"90d","release":"permanent","golden":"permanent"}["${BUILD_TYPE}"],
}
json.dump(m, open(os.path.join(b,"manifest.json"),"w"), indent=2)
print("  manifest.json                      %s / abi %s / regmap %s" % (m["release"], m["fpga_abi"], m["register_map_version"]))
PY

# ── Checksums ─────────────────────────────────────────────────────────────
# Every file except the checksum list itself. Sorted and NUL-delimited so the
# result is stable regardless of directory order or exotic filenames.
( cd "$BUNDLE" && find . -type f ! -name checksums.sha256 -print0 \
    | sort -z | xargs -0 sha256sum > checksums.sha256 )
say "checksums.sha256" "$(wc -l < "$BUNDLE/checksums.sha256") files"

# ── Package ───────────────────────────────────────────────────────────────
( cd "$OUT" && tar --zstd -cf "$NAME.tar.zst" "$NAME" )
( cd "$OUT" && sha256sum "$NAME.tar.zst" > "$NAME.sha256" )
say "$NAME.tar.zst" "$(stat -c%s "$OUT/$NAME.tar.zst") B"

# Retention marker, read by release/retention.sh. A permanent class is recorded
# in the artifact itself so pruning cannot depend on remembering.
echo "$BUILD_TYPE" > "$OUT/$NAME.class"

echo
echo "Bundle:   $BUNDLE"
echo "Artifact: $OUT/$NAME.tar.zst"
echo "Class:    $BUILD_TYPE ($(python3 -c "print({'dev':'14 days','rc':'90 days','release':'permanent','golden':'permanent + secondary copy'}['$BUILD_TYPE'])"))"
