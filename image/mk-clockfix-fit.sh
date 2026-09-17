#!/usr/bin/env bash
# Repack the proven 6.12 FIT with only the Zynq PS reference-clock declaration
# corrected for Pluto+ hardware. The source FIT is never modified.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE="${1:-$ROOT/pluto612c.frm}"
OUTPUT="${2:-$ROOT/pluto612c-clockfix.frm}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

for tool in dumpimage dtc mkimage; do
  command -v "$tool" >/dev/null || {
    echo "ERROR: required tool not found: $tool" >&2
    exit 1
  }
done
[[ -s "$SOURCE" ]] || { echo "ERROR: source FIT not found: $SOURCE" >&2; exit 1; }
[[ "$SOURCE" != "$OUTPUT" ]] || { echo "ERROR: output must differ from source" >&2; exit 1; }

dumpimage -T flat_dt -p 0 -o "$WORK/fdt.original.dtb" "$SOURCE" >/dev/null
dumpimage -T flat_dt -p 1 -o "$WORK/fpga.bin"         "$SOURCE" >/dev/null
dumpimage -T flat_dt -p 2 -o "$WORK/kernel.img"       "$SOURCE" >/dev/null
dumpimage -T flat_dt -p 3 -o "$WORK/ramdisk.gz"       "$SOURCE" >/dev/null

dtc -I dtb -O dts -o "$WORK/fdt.dts" "$WORK/fdt.original.dtb" 2>/dev/null
python3 - "$WORK/fdt.dts" <<'PY'
import pathlib
import re
import sys

path = pathlib.Path(sys.argv[1])
text = path.read_text()
old = "ps-clk-frequency = <0x1fca055>;"
new = "ps-clk-frequency = <0x2faf080>;"
count = text.count(old)
if count != 1:
    raise SystemExit(f"ERROR: expected exactly one 33.333333 MHz PS clock, found {count}")
path.write_text(text.replace(old, new))
PY
dtc -I dts -O dtb -o "$WORK/fdt.dtb" "$WORK/fdt.dts" 2>/dev/null

cat >"$WORK/pluto.its" <<'ITS'
/dts-v1/;
/ {
    description = "6.12 Pluto+ modem image; PS clock corrected to 50 MHz";
    #address-cells = <1>;
    images {
        fdt@1 { description="pluto+ clockfix"; type="flat_dt"; arch="arm";
                compression="none"; data=/incbin/("fdt.dtb");
                hash@1 { algo="md5"; }; };
        fpga@1 { description="FPGA"; type="fpga"; arch="arm";
                 compression="none"; load=<0x0f000000>;
                 data=/incbin/("fpga.bin"); hash@1 { algo="md5"; }; };
        linux_kernel@1 { description="Linux"; type="kernel"; arch="arm";
                 os="linux"; compression="none"; load=<0x00008000>;
                 entry=<0x00008000>; data=/incbin/("kernel.img");
                 hash@1 { algo="md5"; }; };
        ramdisk@1 { description="Ramdisk"; type="ramdisk"; arch="arm";
                 os="linux"; compression="gzip"; data=/incbin/("ramdisk.gz");
                 hash@1 { algo="md5"; }; };
    };
    configurations {
        default = "config@0";
        config@0 { description="Linux with fpga"; kernel="linux_kernel@1";
                   fdt="fdt@1"; ramdisk="ramdisk@1"; fpga="fpga@1"; };
        config@1 { description="Linux with fpga"; kernel="linux_kernel@1";
                   fdt="fdt@1"; ramdisk="ramdisk@1"; fpga="fpga@1"; };
        config@2 { description="Linux with fpga"; kernel="linux_kernel@1";
                   fdt="fdt@1"; ramdisk="ramdisk@1"; fpga="fpga@1"; };
        config@3 { description="Linux with fpga"; kernel="linux_kernel@1";
                   fdt="fdt@1"; ramdisk="ramdisk@1"; fpga="fpga@1"; };
        config@4 { description="Linux with fpga"; kernel="linux_kernel@1";
                   fdt="fdt@1"; ramdisk="ramdisk@1"; fpga="fpga@1"; };
        config@5 { description="Linux with fpga"; kernel="linux_kernel@1";
                   fdt="fdt@1"; ramdisk="ramdisk@1"; fpga="fpga@1"; };
        config@6 { description="Linux with fpga"; kernel="linux_kernel@1";
                   fdt="fdt@1"; ramdisk="ramdisk@1"; fpga="fpga@1"; };
        config@7 { description="Linux with fpga"; kernel="linux_kernel@1";
                   fdt="fdt@1"; ramdisk="ramdisk@1"; fpga="fpga@1"; };
        config@8 { description="Linux with fpga"; kernel="linux_kernel@1";
                   fdt="fdt@1"; ramdisk="ramdisk@1"; fpga="fpga@1"; };
        config@9 { description="Linux with fpga"; kernel="linux_kernel@1";
                   fdt="fdt@1"; ramdisk="ramdisk@1"; fpga="fpga@1"; };
        config@10 { description="Linux with fpga"; kernel="linux_kernel@1";
                    fdt="fdt@1"; ramdisk="ramdisk@1"; fpga="fpga@1"; };
    };
};
ITS

(cd "$WORK" && mkimage -f pluto.its "$OUTPUT" >/dev/null)

# Prove that the non-DT payloads survived byte-for-byte.
for index in 1 2 3; do
  dumpimage -T flat_dt -p "$index" -o "$WORK/check.$index" "$OUTPUT" >/dev/null
  cmp "$WORK/check.$index" "$WORK/$(case "$index" in 1) echo fpga.bin;; 2) echo kernel.img;; 3) echo ramdisk.gz;; esac)"
done

size=$(stat -c%s "$OUTPUT")
limit=12395781
(( size <= limit )) || { echo "ERROR: FIT exceeds proven boot ceiling" >&2; exit 1; }
printf 'source: %s\noutput: %s\nsize: %d bytes\nsha256: ' "$SOURCE" "$OUTPUT" "$size"
sha256sum "$OUTPUT" | awk '{print $1}'
