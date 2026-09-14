#!/usr/bin/env bash
# Build the 6.12 product FIT, sized to stay inside the 14 MB mtd3 window.
#   kernel  : 6.12 trimmed (WiFi/sound/netfilter/NFS removed) -- 4.82 MB
#   ramdisk : tezuka 6.12 userspace, pruned by ELF dependency closure, with the
#             DATV scripts removed so nothing parks the TX LO off at boot
#   dt      : tezuka Pluto+   fpga: our Z7020 modem bitstream
set -euo pipefail
SC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT=/home/amither/Documents/SDR
TEZ="$ROOT/tezuka-plutoplus-v0.3.5-7cf6171/pluto.frm"
BIT="$ROOT/out/pluto-datalink-v1.4.0-dev5/fpga/system.bin"
OUT="${1:-$SC/pluto612.frm}"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT

dumpimage -T flat_dt -p 0 -o "$W/fdt.dtb" "$TEZ" >/dev/null 2>&1
cp "$SC/kbuild612/arch/arm/boot/zImage" "$W/kernel.img"
cp "$SC/rd612/rd-min.cpio.gz"            "$W/ramdisk.gz"
cp "$BIT"                                "$W/fpga.bin"
for f in fdt.dtb kernel.img ramdisk.gz fpga.bin; do
  [[ -s "$W/$f" ]] || { echo "ERROR: $f empty"; exit 1; }
  printf '    %-12s %9s B\n' "$f" "$(stat -c%s "$W/$f")"
done
# zImage, not uImage: the FIT entry carries its own header.
if [[ "$(xxd -p -s 36 -l 4 "$W/kernel.img")" != "1828 6f01" && "$(xxd -p -s 36 -l 4 "$W/kernel.img")" != "18286f01" ]]; then
  echo "ERROR: kernel.img is not a raw ARM zImage"; exit 1
fi
{
cat <<'HEAD'
/dts-v1/;
/ {
    description = "6.12 trimmed + Pluto+ dt + modem bitstream (no DATV scripts)";
    #address-cells = <1>;
    images {
        fdt@1 { description="pluto+"; type="flat_dt"; arch="arm"; compression="none";
                data = /incbin/("fdt.dtb"); hash@1 { algo="md5"; }; };
        fpga@1 { description="FPGA"; type="fpga"; arch="arm"; compression="none";
                 load=<0x0f000000>; data = /incbin/("fpga.bin"); hash@1 { algo="md5"; }; };
        linux_kernel@1 { description="Linux"; type="kernel"; arch="arm"; os="linux";
                 compression="none"; load=<0x00008000>; entry=<0x00008000>;
                 data = /incbin/("kernel.img"); hash@1 { algo="md5"; }; };
        ramdisk@1 { description="Ramdisk"; type="ramdisk"; arch="arm"; os="linux";
                 compression="gzip"; data = /incbin/("ramdisk.gz"); hash@1 { algo="md5"; }; };
    };
    configurations {
        default = "config@0";
HEAD
for i in $(seq 0 10); do
  printf '        config@%d { description="Linux with fpga"; kernel="linux_kernel@1"; fdt="fdt@1"; ramdisk="ramdisk@1"; fpga="fpga@1"; };\n' "$i"
done
cat <<'TAIL'
    };
};
TAIL
} > "$W/pluto.its"
( cd "$W" && mkimage -f pluto.its "$(basename "$OUT")" >/dev/null 2>&1 && mv "$(basename "$OUT")" "$OUT" )
SZ=$(stat -c%s "$OUT"); LIM=12395781
printf '    wrote %s: %s B (%.2f MB)\n' "$OUT" "$SZ" "$(echo "scale=2; $SZ/1048576" | bc)"
if [[ $SZ -gt $LIM ]]; then
  echo "    ERROR: over the proven-bootable fit_size ceiling (u-boot would read a truncated FIT)"; exit 1
fi
printf '    within the proven-bootable ceiling, margin %s B\n' "$((LIM-SZ))"
