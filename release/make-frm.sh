#!/usr/bin/env bash
# make-frm.sh -- rebuild the Pluto firmware image with a given bitstream.
#   ./release/make-frm.sh <system.bin> <out.frm> [stock.frm]
#
# This is what makes a deployment survive a power cycle. u-boot boots with
#   bootm ${fit_load_address}#${fit_config}
# and the selected configuration names an `fpga` image that u-boot loads into
# the PL BEFORE the kernel -- the FIT's own description is "Configuration to
# load fpga before Kernel". Replacing that image is therefore the only way to
# have the modem PL present at power-on on this board:
#
#   /mnt/jffs2 (mtd2)         persistent, but 700 KB free vs a 2.5 MB bitstream
#   qspi-fsbl-uboot (mtd0)    1 MB, cannot hold a bitstream at all
#   qspi-linux (mtd3)         30 MB, holds this image  <- the only candidate
#
# The kernel, device tree and ramdisk are carried across UNCHANGED from the
# stock image. Nothing in this project rebuilds them, and silently shipping a
# different kernel than the one a release was validated against would make the
# bundle a worse record than no bundle at all.
set -euo pipefail
BIN="${1:-}"; OUT="${2:-}"
STOCK="${3:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/tezuka-plutoplus-v0.3.5-7cf6171/pluto.frm}"
[[ -n "$BIN" && -n "$OUT" ]] || { echo "usage: $0 <system.bin> <out.frm> [stock.frm]" >&2; exit 2; }
[[ -f "$BIN"   ]] || { echo "ERROR: no such bitstream: $BIN" >&2; exit 1; }
[[ -f "$STOCK" ]] || { echo "ERROR: no such stock firmware: $STOCK" >&2; exit 1; }
command -v mkimage   >/dev/null || { echo "ERROR: mkimage not found (apt install u-boot-tools)" >&2; exit 1; }
command -v dumpimage >/dev/null || { echo "ERROR: dumpimage not found (apt install u-boot-tools)" >&2; exit 1; }

W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
# 0 fdt, 1 fpga, 2 kernel, 3 ramdisk -- the order dumpimage -l reports.
dumpimage -T flat_dt -p 0 -o "$W/fdt.dtb"     "$STOCK" >/dev/null 2>&1
dumpimage -T flat_dt -p 2 -o "$W/kernel.img"  "$STOCK" >/dev/null 2>&1
dumpimage -T flat_dt -p 3 -o "$W/ramdisk.gz"  "$STOCK" >/dev/null 2>&1
for f in fdt.dtb kernel.img ramdisk.gz; do
  [[ -s "$W/$f" ]] || { echo "ERROR: could not extract $f from $STOCK" >&2; exit 1; }
done
cp "$BIN" "$W/fpga.bin"

# ── Remove maia-sdr from the ramdisk ──────────────────────────────────────
# NOT cosmetic, and not optional for a modem release. maia-sdr's kernel module
# claims the IIO buffers, so on a stock-firmware board the fabric modem's char
# devices are EBUSY *from boot*:
#
#     dd: can't open '/dev/iio:device2': Device or resource busy
#
# with no userspace process holding any /dev/iio fd -- the claim is in-kernel.
# It also reserves 128 MB as maia_sdr_recording. A radio flashed with maia
# present cannot transmit or receive through the fabric at all, which is the
# whole point of this image. `rmmod maia_sdr` is NOT a workaround: it hangs and
# wedges the board, observed on both units.
#
# /etc, /lib/modules and /root are all on the ramdisk, rebuilt from this image
# on every boot, so this is the only durable place to remove it -- autorun.sh
# runs at S98, long after S50 has loaded the module.
#
# Editing the ramdisk needs ownership and device nodes preserved, hence
# fakeroot. If the tooling is absent this FAILS rather than quietly shipping an
# image whose radio cannot work.
if [[ "${KEEP_MAIA:-0}" == "1" ]]; then
  echo "  ramdisk: maia-sdr KEPT (KEEP_MAIA=1) -- the fabric modem will not work" >&2
else
  command -v cpio     >/dev/null || { echo "ERROR: cpio not found; cannot strip maia-sdr" >&2; exit 1; }
  command -v fakeroot >/dev/null || { echo "ERROR: fakeroot not found (apt install fakeroot)" >&2; exit 1; }
  mkdir -p "$W/rd"
  gzip -dc "$W/ramdisk.gz" > "$W/ramdisk.cpio"
  ( cd "$W/rd" && fakeroot cpio -idm --quiet < "$W/ramdisk.cpio" ) \
    || { echo "ERROR: could not unpack the ramdisk" >&2; exit 1; }

  # The init script is what loads it; the .ko is belt and braces; the httpd
  # depends on the module and would only fail noisily at boot.
  MAIA_REMOVE=(
    etc/init.d/S50maia-kmod
    etc/init.d/S50maia-sdr-certificates
    etc/init.d/S60maia-httpd
    "lib/modules/$(ls "$W/rd/lib/modules" 2>/dev/null | head -1)/updates/maia-sdr.ko"
  )
  REMOVED=0
  for f in "${MAIA_REMOVE[@]}"; do
    if [[ -e "$W/rd/$f" ]]; then rm -f "$W/rd/$f"; REMOVED=$((REMOVED+1)); fi
  done
  (( REMOVED > 0 )) || { echo "ERROR: found no maia-sdr files to remove; has the stock image changed?" >&2; exit 1; }

  # `find` alone returns directory order, which varies between runs and makes
  # the artifact unreproducible -- the one property a recovery image must have.
  # Sorting fixes the member order so the same inputs give the same bytes.
  # --reproducible zeroes the inode and device numbers, which the SVR4 format
  # records and which differ on every extraction; sorting fixes member order.
  # Together they make the repack byte-identical from identical input.
  ( cd "$W/rd" && fakeroot sh -c 'find . | LC_ALL=C sort | cpio -o -H newc --reproducible --quiet' ) > "$W/ramdisk.new" \
    || { echo "ERROR: could not repack the ramdisk" >&2; exit 1; }
  # -n omits gzip's mtime and filename header fields, which otherwise change on
  # every run and make the artifact unreproducible even from identical input.
  gzip -9 -n -c "$W/ramdisk.new" > "$W/ramdisk.gz"
  # A ramdisk that lost far more than the handful of maia files means the
  # repack went wrong, and an unbootable rootfs is worse than maia.
  NBEFORE=$(cpio -t < "$W/ramdisk.cpio" 2>/dev/null | wc -l)
  NAFTER=$(cpio -t < "$W/ramdisk.new" 2>/dev/null | wc -l)
  (( NAFTER >= NBEFORE - 8 )) || {
    echo "ERROR: ramdisk lost $((NBEFORE-NAFTER)) entries (expected <= 8); refusing." >&2; exit 1; }
  echo "  ramdisk: removed $REMOVED maia-sdr files, $NAFTER of $NBEFORE entries kept" >&2
fi

# The number of configurations is matched to the stock image: different units
# select different ones via the fit_config environment variable, and a missing
# node would leave those boards unbootable. They are aliases -- every config in
# the stock image references the same four sub-images.
NCFG=$(fdtdump -s "$STOCK" 2>/dev/null | grep -c 'config@[0-9]* {' || echo 10)
(( NCFG > 0 )) || NCFG=10

{
  cat <<'HEAD'
/dts-v1/;
/ {
    description = "Configuration to load fpga before Kernel";
    magic = "ITB PlutoSDR (ADALM-PLUTO)";
    #address-cells = <1>;
    images {
        fdt@1 {
            description = "zynq-pluto-sdr";
            type = "flat_dt";
            arch = "arm";
            compression = "none";
            data = /incbin/("fdt.dtb");
            hash@1 { algo = "md5"; };
        };
        fpga@1 {
            description = "FPGA";
            type = "fpga";
            arch = "arm";
            compression = "none";
            load = <0x0f000000>;
            data = /incbin/("fpga.bin");
            hash@1 { algo = "md5"; };
        };
        linux_kernel@1 {
            description = "Linux";
            type = "kernel";
            arch = "arm";
            os = "linux";
            compression = "none";
            load = <0x00008000>;
            entry = <0x00008000>;
            data = /incbin/("kernel.img");
            hash@1 { algo = "md5"; };
        };
        ramdisk@1 {
            description = "Ramdisk";
            type = "ramdisk";
            arch = "arm";
            os = "linux";
            compression = "gzip";
            data = /incbin/("ramdisk.gz");
            hash@1 { algo = "md5"; };
        };
    };
    configurations {
        default = "config@0";
HEAD
  for ((i=0; i<NCFG; i++)); do
    cat <<CFG
        config@$i {
            description = "Linux with fpga";
            fdt = "fdt@1";
            kernel = "linux_kernel@1";
            ramdisk = "ramdisk@1";
            fpga = "fpga@1";
        };
CFG
  done
  printf '    };\n};\n'
} > "$W/pluto.its"

( cd "$W" && mkimage -f pluto.its "$(basename "$OUT")" >/dev/null ) \
  || { echo "ERROR: mkimage failed" >&2; exit 1; }
cp "$W/$(basename "$OUT")" "$OUT"

SZ=$(stat -c%s "$OUT")
# qspi-linux is 0x1E00000. Overflowing it would be discovered by flashcp
# truncating the image, i.e. after the board has already been made unbootable.
LIMIT=$((0x1E00000))
if (( SZ > LIMIT )); then
  echo "ERROR: firmware image is $SZ B but qspi-linux holds only $LIMIT B." >&2
  echo "       Refusing to produce an image that cannot be flashed." >&2
  rm -f "$OUT"; exit 1
fi
echo "wrote $OUT: $SZ B ($((100*SZ/LIMIT))% of the qspi-linux partition), $NCFG configurations"
