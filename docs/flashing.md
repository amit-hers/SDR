# Flashing a board over JTAG with OpenOCD

This is the procedure that puts the golden 6.12 image onto a board. It is the
only procedure that works on a board that has not been cloned yet, and the
reason why is worth understanding before you run anything.

---

## 1. Why JTAG, and not `flashcp`

A stock board runs kernel 5.10, and **5.10 cannot write its own flash
correctly**.

The chip is a Winbond W25Q256 (JEDEC `EF 40 19`, 32 MB). Addressing beyond
16 MiB uses a volatile bank-select register, the EAR (Extended Address
Register). On these boards the EAR powers up at **1**, so the hardware is
already pointing at the upper 16 MiB. The kernel's `read_ear()` returns
`-EINVAL` for Winbond parts, so `nor->curbank` stays at its initialised value
of 0, and `spi_nor_write_ear()` then short-circuits:

```c
if (ear == nor->curbank)     /* believes bank 0 is already selected */
        return 0;            /* ... so it never writes the EAR */
```

The result: every mtd read and write lands **16 MiB further into the chip than
the address you named**.

The trap is that this is nearly invisible. Reads and writes share the same
shift, so a write to offset X followed by a read of offset X both land on
X+16 MiB and agree perfectly. `flashcp` verifies, prints success, and has
changed nothing whatsoever at the address it claims to have written. A whole
session can be spent "successfully" flashing a board that never changes.

The fix is in this kernel tree
(`kernel/patches/0001-spi-nor-normalise-EAR-bank-for-Winbond.patch`), which
forces `curbank` to an impossible value so the EAR write cannot be skipped. But
that fix only helps *after* 6.12 is running — which is exactly the thing you are
trying to install. Hence JTAG.

The programmer runs **in u-boot, loaded into DDR over JTAG**. It never boots
from the flash it is rewriting and it addresses the chip directly rather than
through the kernel's spi-nor layer, so the EAR bug cannot reach it.

---

## 2. What you need

| Item | Detail |
|---|---|
| JTAG adapter | FT4232H, enumerates as `Xilinx Debugger` |
| OpenOCD | 0.12.0 (older versions have not been tried here) |
| Programmer ELF | `uboot/u-boot.elf`, entry `0x04000000` (rebuild: `uboot/README.md`) |
| Image | `release/golden/pluto-datalink-6.12-golden.frm` |
| OpenOCD config | `fpga/jtag/zynq.cfg` |
| Driver | `arm-linux-gnueabihf-` toolchain only if rebuilding u-boot |

The SoC is an **XC7Z020**, not the Z7010 the on-board strings claim. Every
identification string on the device lies about this; the authoritative source is
`PSS_IDCODE` at `0xF8000530`, which reads `0x23727093` (rev 3.0). OpenOCD
confirms it independently at the JTAG tap: `0x23727093 (mfg: 0x049 Xilinx, part:
0x3727)`.

---

## 3. Memory and flash map

The programmer's contract, defined in `uboot/patches/0001-pluto-qspi-fit-programmer.patch`:

| Address | Meaning |
|---|---|
| `0x03000000` | Mailbox — host writes inputs, target writes results |
| `0x04000000` | u-boot load address and entry point |
| `0x08000000` | The FIT image, staged in DDR |
| `0x0A000000` | Readback buffer for verification |
| `0x00200000` | **Target in flash.** The only value accepted. |
| `0x02000000` | End of mtd3; the erase is bounded inside it |

Flash partitions (identical on every unit):

```
mtd0: 00100000 "qspi-fsbl-uboot"     1 MB    boot loader
mtd1: 00020000 "qspi-uboot-env"    128 KB    u-boot environment
mtd2: 000e0000 "qspi-nvmfs"        896 KB    jffs2, the only writable persistent storage
mtd3: 01e00000 "qspi-linux"         30 MB    the FIT  <-- what gets written
```

`mtd3` begins at physical `0x00200000`, which is why that is the target.

Mailbox layout (little-endian u32 unless noted):

```
0x00 magic0        0x24 erase_size     0x44 jedec_id[6]
0x04 magic1        0x28 sector_size    0x4A ear_raw, pad
0x08 version       0x2C src_crc32      0x4C old_head[32]
0x0C in_fit_len    0x30 erase_len      0x6C new_head[32]
0x10 in_target     0x34 erase_result
0x14 in_expect_crc 0x38 write_result
0x18 status        0x3C read_result
0x1C probe_result  0x40 rb_crc32
0x20 flash_size
```

---

## 4. Procedure

### 4.1 Identify the board before you erase it

`program_fit.sh inspect` is read-only with respect to flash, but it **halts the
target and resumes it into u-boot** — it stops whichever board the ribbon is on.
Getting this wrong means erasing a working unit.

```bash
fpga/jtag/program_fit.sh inspect
```

Then check which board fell off the network. With two units connected, one
should stop answering and the other should keep running:

```bash
ping -I <ifaceA> -c1 192.168.2.17    # the other unit: still up
ping -I <ifaceB> -c1 192.168.2.1     # the target: now down
```

Read the result:

- `probe_result 0` — the flash probed. Anything else means stop.
- `flash_size 0x02000000` — 32 MB, as expected.
- `jedec+ear` — low three bytes are the JEDEC id `EF 40 19`; the byte at
  offset `0x4A` is the raw EAR. **`ear_raw = 1` on an un-cloned board**, which
  is the bug above, observed directly.
- `old_head` — first two words at the target. Word 0 is `0xEDFE0DD0`
  (`d00dfeed` byte-swapped) for any FIT. Word 1 is the FDT `totalsize`, **big
  endian**, and is what distinguishes images: the stock vendor image reads
  `0x00BD2505` (12,395,269 B), the golden image `0x00B5B105` (11,907,333 B).

### 4.2 Program

```bash
fpga/jtag/program_fit.sh program release/golden/pluto-datalink-6.12-golden.frm
```

It takes roughly ten minutes: most of that is transferring 11.9 MB over JTAG,
then the erase and write.

Gates, all enforced on the target and none skippable from the host:

1. `target` must equal `0x00200000`.
2. The CRC32 of the image in DDR must equal what the host computed, or the
   erase is refused. This catches a corrupted or partial JTAG transfer before
   anything is destroyed.
3. The erase length is rounded up to the erase-block size and must stay inside
   mtd3.
4. The watchdog is silenced first (`ZMR = 0x00ABC000` at `0xF8005000`).
5. Verification reads back through opcode `0x13` — a **different code path**
   from the write — so a shared addressing fault cannot make a bad write look
   good.

### 4.3 Read the verdict

```
mailbox in: fit_len=11907333 target=0x00200000 expect_crc=0xE5498D4F
ddr head  : 0xEDFE0DD0 (expect 0xEDFE0DD0)
status       0x0000D00D      <- success
src_crc32    0xE5498D4F
rb_crc32     0xE5498D4F      <- must equal src_crc32
erase_result 0
write_result 0
read_result  0
```

`status 0xD00D` with `src_crc32 == rb_crc32` is a good write. Then **power-cycle
the board** — do not just reboot it.

### 4.4 Verify from the board itself

Once it boots 6.12, the EAR bug is fixed and the board's own flash reads are
trustworthy:

```bash
ssh root@192.168.2.1 'head -c 11907333 /dev/mtd3 | sha256sum'
sha256sum release/golden/pluto-datalink-6.12-golden.frm
```

These must match. A unit that boots 6.12 also reports a **non-empty USB
serial**, derived from the SPI-NOR UniqueID — an empty serial means 5.10 is
still running, and is the fastest way to tell the two apart.

---

## 5. Pitfalls

Every one of these was hit in practice.

**Use the FDT `totalsize`, never `filesize - 33`.** A `.frm` *may* be a FIT
followed by a 33-byte ASCII-md5 trailer, but the bare artifacts here are not —
their file size already equals `totalsize`. Subtracting 33 anyway truncates the
FDT. It still writes and verifies perfectly, because the programmer faithfully
stores whatever length it is handed, and the result probably still boots since
the lost bytes are trailing padding. It is simply not a byte-identical clone.
The length is a big-endian u32 at offset 4 of the image.

**A clean `0xD00D` does not mean the right image was written.** The programmer
verifies against the length and CRC it was *given*. If a mailbox input fails to
land, it runs to a flawless success against stale values. `program_fit.sh` now
echoes `fit_len`, `target` and `expect_crc` back out of target memory after
writing them and before the erase — check that line, not just the final status.

**Power-cycle between JTAG runs.** A second run against a board still sitting in
u-boot from a previous session fails at flash probe with `probe_result -1`,
`status 0xE001`. Nothing is erased, but nothing is written either.

**Never write past 16 MiB.** Beyond that boundary the EAR problem corrupts the
write. Keep images under ~14 MB. The golden image spans
`0x00200000..0x00D5D000`, safely below.

**Do not trust `flashcp` output — but do not discard it either.** On 5.10 it is
actively misleading (above). On 6.12 it is correct, and its verification output
must be read rather than sent to `/dev/null`; a silent failure there is
indistinguishable from success.

**`ftdi_sio` claims the adapter.** All four FT4232H channels get bound to the
serial driver; OpenOCD needs channel A. The script unbinds it, resolving the USB
port **by product string** rather than a hardcoded number — the debugger and the
radios have swapped ports more than once, and a stale port unbinds nothing while
appearing to succeed.

**Turn the MMU off before touching SLCR.** With it on, OpenOCD's reads go through
Linux's page tables, where the watchdog and SLCR registers are unmapped, and you
get `dfsr=5` instead of a register.

**Do not disable the L2 cache.** Cleaning and disabling L2C-310 looks correct and
is what u-boot does, but the one boot that actually succeeded did not do it, and
every attempt after adding it failed. Evidence over theory.

**u-boot reads exactly `fit_size` bytes.** A byte-perfect `mtd3` write can still
boot the old kernel if that variable disagrees with the image.

**Do not run `fw_setenv` on these boards.** `mtd1` fails its CRC, and writing the
environment bricks them.

**`reboot` is a no-op; use `reboot -f`.** And after flashing, prefer a real power
cycle: warm reboots have been observed wedging the board in the BootROM.

**OpenOCD output is block-buffered through a pipe.** Without `stdbuf`, a
ten-minute run shows an empty log the whole time, which is indistinguishable
from a hang. The script handles this; if you build your own pipeline, do the
same.

**Beware `pkill -f <name>` on a pattern that appears in your own command line.**
It matches the searching shell and kills it. This bit repeatedly, both on the
board and on the host. Resolve processes by `/proc/PID/exe`, or use `pkill -x`.

---

## 6. Recovering a board

If a unit stops booting, the flash is still reachable over JTAG — the programmer
does not depend on anything in flash. Attach the ribbon, run `inspect` to confirm
the chip probes, and reprogram. There is no state in mtd3 that can prevent this,
which is why `mtd0`, `mtd1` and `mtd2` are left strictly alone: the boot loader
and environment are what make recovery possible, and the programmer refuses any
target but `0x00200000` specifically so a mistake cannot reach them.
