# Golden image — Pluto+ / LibreSDR datalink, kernel 6.12

Two artifacts, one image. They contain the **same FIT**; they differ only in
whether the 33-byte updater trailer is appended.

| File | Size | Use |
|---|---|---|
| `pluto-datalink-6.12-golden.frm` | 11,907,333 B | Bare FIT. For `fpga/jtag/program_fit.sh` (JTAG), and for direct flash writes. |
| `pluto-datalink-6.12-golden.dfu.frm` | 11,907,366 B | Same FIT plus the ASCII-md5 trailer the DFU / mass-storage updater checks the download against. |

Checksums are in `SHA256SUMS` and `MD5SUMS`. The FIT's own `totalsize` field is
11,907,333 — use that as the length, never `filesize - 33`: the bare artifact
has no trailer, and subtracting one anyway silently truncates the FDT.

## What is verified

Both boards run this image, built `#2 SMP PREEMPT Tue Sep 15 14:14:44 IDT 2026`:

- **UNIT-A** — flash byte-identical to the bare artifact over all 11,907,333
  bytes (`88f43352…`), read back from the board's own `/dev/mtd3`. Boots, and
  the appliance is serving 18 s after power-on.
- **UNIT-B** — cloned over JTAG. First 11,907,300 bytes identical; the final 33
  bytes read `0xFF` rather than the FDT's trailing `0x00` padding, because an
  earlier run wrote the truncated length. It boots 6.12 and runs correctly,
  which is what demonstrates those bytes are inert padding — but it is **not**
  hash-identical, so verify that unit by prefix, not by whole-file hash, until
  it is rewritten.

## Why this image exists

Stock 5.10 cannot write its own flash correctly. `read_ear()` returns `-EINVAL`
for Winbond, so `curbank` stays 0 while the hardware EAR is 1, and
`spi_nor_write_ear()` then short-circuits on `ear == nor->curbank`. Every mtd
access lands 16 MiB further into the chip than asked. Reads and writes share
the shift, so a readback compares equal and `flashcp` reports success having
changed nothing at the address it named. The fix is in this kernel
(`kernel/patches/0001-spi-nor-normalise-EAR-bank-for-Winbond.patch`).

## Flashing

**A unit still on 5.10** cannot be flashed from its own Linux, for the reason
above. Use JTAG:

    fpga/jtag/program_fit.sh inspect                 # read-only: geometry, JEDEC, current head
    fpga/jtag/program_fit.sh program release/golden/pluto-datalink-6.12-golden.frm

`inspect` resumes the target into u-boot, so it stops whatever board it is
attached to — confirm the ribbon is on the unit you mean to overwrite before
running either mode. The programmer refuses any target but `0x00200000`,
refuses to erase unless the image in DDR matches the host's CRC, and bounds the
erase inside mtd3. Power-cycle the unit between attempts: a second run against
a board still sitting in u-boot from the previous one fails at flash probe
(`probe_result -1`, `status 0xE001`).

**A unit already on 6.12** has a working flash path and does not need JTAG.

Never write past 16 MiB. The erase for this image spans
`0x00200000..0x00D5D000`, comfortably below it.
