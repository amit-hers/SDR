# Pluto+ QSPI boot-source contradiction

Date: 2026-09-15  
Scope: read-only investigation; no code, flash-array, EAR, or hardware modification performed.

## 1. Proven facts

- The sampled Zynq `BOOT_MODE` value is `0x1`. In the Zynq-7000 encoding, bits `[3:0]=0001` select QSPI boot. This proves the BootROM entered its QSPI boot path; it does **not** prove that the accepted boot header was at flash byte 0.[1]
- BootROM reached an FSBL, then the observed U-Boot, then Linux. The running banner identifies the U-Boot base as ADI commit `90401ce9ce029e5563f4dface63914d42badf5bc`, with a dirty build. The uncommitted delta cannot be reconstructed from the banner.
- Linux identifies one Winbond W25Q256 (32 MiB). Repeated Linux reads are stable: `mtd0` is byte-identical across five reads and has SHA-256 `7df4a651b6c6ae3e31b36ee97dfabf85eedb3173e9565866f5866f906072c743`; `mtd3` is coherent and contains a valid Linux FIT. Stability is strong evidence against random electrical bus noise.
- Linux's partition labels are policy, not evidence of what BootROM selected. `mtd0` being named `qspi-fsbl-uboot` does not force the currently executing FSBL/U-Boot to reside at offset zero.
- The exact ADI U-Boot normal QSPI command reads its FIT from flash offset `0x200000`. Thus coherent `mtd3` explains Linux startup after U-Boot is running, but says nothing by itself about where BootROM found the FSBL.[8]

## 2. New source-code and documentation findings

### BootROM and boot-header rules

The documented Zynq-7000 QSPI search is:

1. At POR, `XDCFG.MULTIBOOT_ADDR` resets to zero.
2. BootROM forms a candidate byte address as `MULTIBOOT_ADDR << 15`; one unit is `0x8000` (32 KiB).
3. It checks the candidate boot header. Relevant fixed fields include the bus-width detection word `0xAA995566` at header offset `0x20`, image-identification word `0x584C4E58` (`XLNX`) at `0x24`, and boot-header checksum at `0x48`.[2][3]
4. If identification/checksum validation fails, BootROM increments the multiboot index and tries the next 32 KiB boundary.
5. In single/stacked x4 QSPI mode it searches the first 16 MiB: indices `0x000..0x1ff`, candidate offsets `0x000000..0xff8000`. The documented algorithm does not wrap an address and does not scan arbitrary byte boundaries.[2]

Therefore, “QSPI strapped” and “Linux offset zero is not a boot image” are not contradictory if BootROM accepted a valid header at some later `N * 0x8000` address.

### Standard Xilinx FSBL

The Xilinx FSBL sources use `XDCFG_MULTIBOOT_ADDR_OFFSET` to derive the selected image base and make subsequent partition offsets relative to it in `LoadBootImage()` / `GetImageHeaderStartAddr()`. On fallback, `FsblFallback()` / `Update_MultiBootRegister()` advance the multiboot location, mark failure in `REBOOT_STATUS`, and initiate a non-POR reset.[4][5]

The Tezuka Pluto+ repository contains prebuilt, unstripped `fsbl.elf` artifacts, not a pinned source tree for that binary. Symbols include `LoadBootImage`, `GetImageHeaderStartAddr`, `NextValidImageCheck`, `FsblFallback`, `InitQspi`, and `QspiAccess`; debug strings show Vitis 2023.1/XQspiPs. This supports lineage from Xilinx FSBL, but it does **not** prove the exact instructions in the FSBL currently stored on this board. The running U-Boot's `-dirty` suffix creates the same limitation for local modifications.

### Exact observed U-Boot base

Relevant files/functions at ADI commit `90401ce9ce` are:

- `configs/zynq_pluto_defconfig`: enables Zynq QSPI, Winbond SPI-NOR, BAR support, FIT and FIT signatures.[7]
- `board/xilinx/zynq/board.c::board_late_init()`: converts QSPI straps into `modeboot=qspiboot`.[9]
- `include/configs/zynq-common.h`: places the environment at `0x100000`, imports an optional extra environment at `0xff000`, and reads the FIT at `0x200000` (first 9 MiB, then up to 30 MiB on validation failure).[8]
- `drivers/mtd/spi/spi_flash.c` and `drivers/spi/zynq_qspi.c`: probe the W25Q256 and perform controller-mediated SPI reads.[10][11]

No Zynq `MULTIBOOT_ADDR` write exists in this exact U-Boot base. The normal `qspiboot` path invokes `sf probe`, then reads `0xff000` and `0x200000`; it is not a hidden alternate boot source. A dirty local change remains unknowable without the binary/source.

The previously traced Linux 5.10 path uses opcode `0x6b`, 3-byte addresses below 16 MiB, eight dummy clocks, quad data, and controller-generated addresses. There is no special branch at `0x200000`. The W25Q256 EAR warning is real, but no Linux code boundary at 2 MiB explains “wrong below 2 MiB, coherent from 2 MiB.” The 6.12/Tezuka implementation changes spi-mem integration and vendor handling, not that fundamental conclusion. See the companion [read-path report](./plutoplus-qspi-read-path-report.md).

## 3. BootROM reconstruction

The most defensible reconstruction is:

```text
POR / reset
  -> sample BOOT_MODE = QSPI
  -> candidate = MULTIBOOT[12:0] << 15 (zero after a true POR)
  -> initialize QSPI0 and read candidate boot header
  -> valid width word + XLNX ID + checksum?
       yes: load that FSBL and execute it
       no:  candidate += 0x8000; repeat through the documented range
  -> FSBL reads selected multiboot base and loads U-Boot partitions relative to it
  -> U-Boot probes QSPI and reads Linux FIT at absolute 0x200000
```

This mechanism can explain the observations if a valid boot container exists at an unknown 32 KiB-aligned offset, while the Linux partition table incorrectly assumes the boot container starts at zero. It does not require EAR aliasing.

Important qualification: Linux's current `MULTIBOOT` value is evidence of the last selected/search location only if later software did not overwrite it. Exact upstream U-Boot does not; the exact running dirty FSBL/U-Boot binaries have not been audited bit-for-bit.

## 4. Register interpretation

Read these **now, before reboot**, from Linux:

```sh
sudo devmem 0xF800025C 32   # SLCR.BOOT_MODE
sudo devmem 0xF8000258 32   # SLCR.REBOOT_STATUS
sudo devmem 0xF800702C 32   # DEVCFG.MULTIBOOT_ADDR
```

These are reads only. Do not use `devmem` with a value argument.

- `BOOT_MODE` is at `0xF800025C`; low nibble `1` means QSPI.[1]
- `REBOOT_STATUS` is at `0xF8000258`; it persists across non-POR reset and reports POR/SRST/watchdog/BootROM status. Its upper bits are also used by standard FSBL fallback bookkeeping, so it is supporting evidence, not a direct flash offset.[6]
- `MULTIBOOT_ADDR` is at `0xF800702C`; decode `N = raw & 0x1fff`, `candidate_offset = N << 15`.[5]

Interpretation:

- `N != 0`: strong evidence that BootROM/FSBL selected or attempted a later 32 KiB candidate. Immediately inspect that offset independently.
- `N == 0`: on the documented POR path, BootROM accepted candidate zero. If Linux still reads no valid header there, leading alternatives become a Linux/U-Boot read discrepancy, later undocumented register modification, or non-POR history. Capture `REBOOT_STATUS` and independently read zero.
- A POR clears `MULTIBOOT`; a warm reset does not. Consequently, reading it before any new power cycle preserves the most information.[5]

## 5. Independent-read options, ranked

1. **U-Boot over 3.3 V TTL UART — best information/risk ratio.** It uses firmware and a driver independent of Linux MTD, and one reboot can compare low flash, the selected candidate, and `0x200000`. Required: correctly identified board UART, common ground, adapter RX connected to board TX, adapter TX to board RX, 115200 8N1. Never attach RS-232 voltage or 5 V TTL.
2. **JTAG halt/load plus U-Boot read.** Strong independence and can expose registers even when Linux is unavailable. It needs a supported Xilinx JTAG adapter, correct Pluto+ JTAG pinout, and an ELF matching the hardware initialization. Risk is medium: generic “recovery” scripts commonly include erase/program commands and must not be run wholesale.
3. **Current Linux register capture.** Zero power cycles and essentially zero risk; decisive for locating a BootROM multiboot candidate, but not an independent flash-byte read.
4. **External SPI programmer.** Electrically most independent only when the Zynq is prevented from driving the bus. An in-circuit clip with both masters active risks contention; powering through the clip risks back-powering the board. Use only with a verified schematic/isolation method and a programmer locked to read-only operations.
5. **DFU or Linux USB-storage reads.** DFU still depends on U-Boot configuration and exposes write-capable operations; Linux mass-storage ultimately reuses the Linux MTD path. Neither is as clean as UART U-Boot for this question.

`sf probe` deserves a precise warning: it does not erase/program the flash array, but SPI-NOR probing may issue reset, mode, QE, BAR/EAR, or status-register commands. Thus it is “array read-only,” not guaranteed state-neutral. If the requirement is literally no volatile flash-state change, stop after register capture and use an externally isolated reader instead.

## 6. Exact decisive experiment

Before reboot, save the three register values above and Linux reference hashes:

```sh
sha256sum /dev/mtd0 /dev/mtd1 /dev/mtd2 /dev/mtd3
```

Then connect the 3.3 V UART, perform one controlled reboot, interrupt autoboot, and run only:

```text
md.l f8000258 1
md.l f800025c 1
md.l f800702c 1
sf probe 0:0 50000000 0
sf read 02080000 000000 100000
crc32 02080000 100000
sf read 02080000 100000 020000
crc32 02080000 020000
sf read 02080000 120000 0e0000
crc32 02080000 0e0000
sf read 02080000 200000 100000
crc32 02080000 100000
```

Do **not** run `saveenv`, `sf write`, `sf erase`, `sf update`, `protect`, `mw`, `mm`, or a recovery script. `02080000` is the normal ADI FIT load region in DDR; each new read deliberately overwrites the previous temporary buffer. Compare the CRCs with Linux values computed using a compatible CRC-32 tool; if this build exposes `hash`, `hash sha256 02080000 <length>` can instead be compared directly with Linux `sha256sum` over an identically sized dump.

If Linux `MULTIBOOT=N != 0`, add one U-Boot read of `N << 15` (rounded command shown after substituting the calculated hexadecimal offset):

```text
sf read 02080000 <candidate_offset> 001000
md.b 02080020 30
```

At buffer offsets `0x20`, `0x24`, and `0x48`, look for the width word, `XLNX` identification word, and a plausible checksum/header. This is a read/display operation only.

## 7. Outcome matrix

| Result | Meaning |
|---|---|
| U-Boot and Linux match at low offsets; `MULTIBOOT != 0`; valid header at that candidate | **H1 favored:** partition/map assumption is wrong; BootROM found a later boot image. |
| U-Boot differs from Linux below 2 MiB but matches Linux at `0x200000` | **H2 favored:** Linux and U-Boot/controller setup produce different low-address reads. Capture exact bytes/opcodes next; EAR alone is still not proven. |
| U-Boot and Linux match everywhere, `MULTIBOOT=0`, and offset zero lacks a valid header | **H3 favored:** the currently running chain did not result from the assumed last POR/flash state, or an unobserved stage altered state/registers. A POR UART boot transcript or isolated programmer becomes necessary. |
| Both readers match, `MULTIBOOT != 0`, but selected candidate has no valid header | The register was modified after BootROM or the tested U-Boot read is not reproducing BootROM's view. Audit the dirty FSBL/U-Boot binary or use JTAG/external read. |
| Reads change between repetitions | Electrical integrity, flash state, or controller-state sensitivity becomes primary; the present stable-data premise no longer holds. |

No identified Linux 5.10, Linux 6.12, or exact upstream U-Boot code path by itself explains a hard split exactly at `0x200000`. The BootROM 32 KiB search is the strongest newly documented reconciliation, but only the register plus independent-read experiment can establish it on this board.

## 8. One recommended action

**Read and record `BOOT_MODE`, `REBOOT_STATUS`, and especially `MULTIBOOT_ADDR` now; then use one 3.3 V UART reboot to interrupt U-Boot and execute the bounded reads/CRCs in section 6.** This preserves the current boot evidence, tests Linux against an independent firmware path, covers both the anomalous low 2 MiB and coherent `0x200000` region, and consumes only one power cycle. Stop before `sf probe` if even volatile mode-register changes are forbidden.

## Sources

1. AMD, *Zynq-7000 TRM — BOOT_MODE register*: https://docs.amd.com/r/en-US/ug585-zynq-7000-SoC-TRM/Register-BOOT_MODE-Details
2. AMD, *Zynq-7000 TRM — MultiBoot / Quad-SPI Boot*: https://docs.amd.com/r/en-US/ug585-zynq-7000-SoC-TRM/MultiBoot and https://docs.amd.com/r/en-US/ug585-zynq-7000-SoC-TRM/Quad-SPI-Boot
3. AMD, *Zynq-7000 SoC Boot Header*: https://docs.amd.com/r/2021.2-English/ug1400-vitis-embedded/Zynq-7000-SoC-Boot-Header
4. AMD/Xilinx Wiki, *Zynq-7000 AP SoC Boot — Multiboot Tech Tip*: https://xilinx-wiki.atlassian.net/wiki/spaces/A/pages/18842174/zynq-70000%2BAP%2BSoC%2BBoot%2B-%2BMultiboot%2BTech%2BTip
5. AMD, *XDCFG_MULTIBOOT_ADDR register*: https://docs.amd.com/r/en-US/ug585-zynq-7000-SoC-TRM/Register-devcfg-XDCFG_MULTIBOOT_ADDR_OFFSET
6. AMD, *REBOOT_STATUS register*: https://docs.amd.com/r/en-US/ug585-zynq-7000-SoC-TRM/Register-REBOOT_STATUS-Details
7. ADI U-Boot `zynq_pluto_defconfig` at `90401ce9ce`: https://github.com/analogdevicesinc/u-boot-xlnx/blob/90401ce9ce029e5563f4dface63914d42badf5bc/configs/zynq_pluto_defconfig
8. ADI U-Boot `zynq-common.h` at `90401ce9ce`: https://github.com/analogdevicesinc/u-boot-xlnx/blob/90401ce9ce029e5563f4dface63914d42badf5bc/include/configs/zynq-common.h
9. ADI U-Boot Zynq board initialization at `90401ce9ce`: https://github.com/analogdevicesinc/u-boot-xlnx/blob/90401ce9ce029e5563f4dface63914d42badf5bc/board/xilinx/zynq/board.c
10. ADI U-Boot SPI-NOR core at `90401ce9ce`: https://github.com/analogdevicesinc/u-boot-xlnx/blob/90401ce9ce029e5563f4dface63914d42badf5bc/drivers/mtd/spi/spi_flash.c
11. ADI U-Boot Zynq QSPI driver at `90401ce9ce`: https://github.com/analogdevicesinc/u-boot-xlnx/blob/90401ce9ce029e5563f4dface63914d42badf5bc/drivers/spi/zynq_qspi.c
12. Xilinx embeddedsw FSBL `image_mover.c`: https://github.com/Xilinx/embeddedsw/blob/master/lib/sw_apps/zynq_fsbl/src/image_mover.c
13. Xilinx embeddedsw FSBL `main.c`: https://github.com/Xilinx/embeddedsw/blob/master/lib/sw_apps/zynq_fsbl/src/main.c
