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
