#!/usr/bin/env bash
# Regenerate boot.dtb with /chosen matching the CURRENT ramdisk size. The initrd
# end address must track the file byte-for-byte: a stale value truncates the
# initramfs and the kernel cannot mount root, with no console to say so.
set -euo pipefail
SC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RD="$SC/rd612/rd-nostdcpp.cpio.gz"
RDSZ=$(stat -c%s "$RD")
dtc -I dtb -O dts -o "$SC/boot.dts" "$SC/rd612/part0.bin" 2>/dev/null
python3 - "$RDSZ" <<'PYEOF'
import re, sys
SC="/tmp/claude-1000/-home-amither-Documents-SDR/8c142fcc-7ead-4043-9775-e33be2b9485a/scratchpad"
rdsz=int(sys.argv[1]); start=0x02200000; end=start+rdsz
s=open(SC+"/boot.dts").read()
chosen = ('\tchosen {\n'
          '\t\tbootargs = "console=ttyPS0,115200 root=/dev/ram0 rw rootfstype=ramfs maxcpus=1";\n'
          '\t\tlinux,initrd-start = <0x%08X>;\n'
          '\t\tlinux,initrd-end = <0x%08X>;\n'
          '\t};\n') % (start, end)
s = re.sub(r"\n\tchosen \{.*?\n\t\};\n", "\n"+chosen, s, count=1, flags=re.S)
# The watchdog node carries status="okay" LATER in the block and the last wins.
m = re.search(r"(watchdog@f8005000 \{.*?\n\t\t\};)", s, re.S)
node = m.group(1); s = s.replace(node, node.replace('status = "okay";','status = "disabled";'), 1)
open(SC+"/boot.dts","w").write(s)
print("    initrd 0x%08X..0x%08X (%d B), maxcpus=1, watchdog disabled" % (start,end,rdsz))
PYEOF
dtc -I dts -O dtb -o "$SC/boot.dtb" "$SC/boot.dts" 2>/dev/null
dtc -I dtb -O dts "$SC/boot.dtb" 2>/dev/null | grep -E "initrd|bootargs" | sed 's/^/    /'
dtc -I dtb -O dts "$SC/boot.dtb" 2>/dev/null | grep -A9 "watchdog@f8005000" | grep status | sed 's/^/    watchdog /'
