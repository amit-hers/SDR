# Pluto+ W25Q256 read-path investigation

## Executive finding

There is **no Linux 5.10 code-path boundary at `0x200000`**. For this DT, every ordinary array read from `0x000000` through `0xffffff` is issued in the same form: Winbond Quad Output Fast Read (`0x6b`), 3 address bytes on one wire, 8 dummy clocks, then data on four wires. The MTD partition layer only adds the partition's base offset; it does not select another opcode, protocol, bank, or controller mode.

Consequently, none of the concrete 5.10-versus-6.12 differences found can, by itself, explain both (A) wrong/high-entropy bytes below `0x200000` and (B) coherent bytes beginning exactly at `0x200000`. The EAR warning is real but is not proof of a bad EAR value. A latent 5.10 EAR-state bug exists, but its effect would cover the whole lower 16 MiB, including the start of `mtd3`, rather than stop at the partition boundary.

The best-supported present explanation is therefore that Linux is returning stable flash-array contents through a working read path, and that the first three logical regions do not contain the formats expected by the current software. How BootROM reached the running FSBL/U-Boot then remains a separate fact to resolve (for example, a valid boot header later in the search/fallback area, a different boot-image placement, or a boot history not established by the current dump). The single experiment at the end cleanly separates this explanation from a Linux-only read-path error without writing flash.

## Source identity and limits

The source identities used here are:

- ADI Linux 5.10 commit `9dfba10b795d0004ae90f2ab29dac0197c8a3b3e`, pinned by the local Pluto build script.
- Tezuka v0.3.5 commit `7cf6171ec540df11a4789df488399ca25397a145`; its Pluto+ defconfig fetches ADI Linux `01e1dcc848ad2b63ec32384c3846f686fae6652c`, then applies Tezuka's patch set.
- ADI U-Boot base commit `90401ce9ce029e5563f4dface63914d42badf5bc`, matching the `g90401ce9ce` fingerprint.

The running U-Boot identifies itself as `-dirty`. Therefore the exact base is proven, but uncommitted build-tree changes are unknowable from the fingerprint. The Tezuka board patches inspected do not alter the base U-Boot SPI-NOR/QSPI algorithms discussed below; they add board/DTS/config material. The effective Tezuka Linux tree *does* patch two EAR vendor tests, described below.

Primary source links: [ADI Linux 5.10 tree](https://github.com/analogdevicesinc/linux/tree/9dfba10b795d0004ae90f2ab29dac0197c8a3b3e), [ADI Linux 6.12 tree](https://github.com/analogdevicesinc/linux/tree/01e1dcc848ad2b63ec32384c3846f686fae6652c), [ADI U-Boot tree](https://github.com/analogdevicesinc/u-boot-xlnx/tree/90401ce9ce029e5563f4dface63914d42badf5bc), and [Tezuka v0.3.5 tree](https://github.com/F5OEO/tezuka_fw/tree/7cf6171ec540df11a4789df488399ca25397a145).

## Proven observations supplied for this investigation

- QSPI boot mode is selected and the machine reaches FSBL, U-Boot, and Linux.
- Linux identifies JEDEC `ef 40 19` as `W25Q256`, 32 MiB.
- Five Linux reads of `mtd0` are byte-identical. The supplied SHA-256 is `7df4a651b6c6ae3e31b36ee97dfabf85eedb3173e9565866f5866f906072c743`.
- That image is statistically compatible with high-entropy stored data, not intermittent signal noise.
- `mtd1` is not a valid current U-Boot environment, `mtd2` is not valid JFFS2, while `mtd3` contains a valid Linux FIT.
- The DT declares one flash/one CS, 50 MHz maximum, TX width 1, RX width 4, and the four stated contiguous partitions.

These facts prove repeatability and format mismatch. They do not alone prove what physical address BootROM used, whether the low data were intentionally encoded, or that Linux and BootROM issued the same read instruction.

## Exact Linux 5.10 path

### Probe and protocol selection

The involved files/functions are:

1. `drivers/spi/spi.c`: `of_register_spi_device()` decodes `spi-tx-bus-width` and `spi-rx-bus-width`. RX=4 sets `SPI_RX_QUAD`; TX=1 sets no dual/quad TX flag.
2. `drivers/mtd/spi-nor/core.c`: `spi_nor_probe()` → `spi_nor_scan()` → `spi_nor_setup()` → `spi_nor_default_setup()` → `spi_nor_spimem_adjust_hwcaps()` → `spi_nor_select_read()`.
3. `drivers/mtd/spi-nor/winbond.c`: the `w25q256` entry supplies JEDEC ID, 32 MiB geometry, dual/quad capability, and W25Q256 fixups.
4. `drivers/mtd/spi-nor/sfdp.c`: `spi_nor_parse_bfpt()` may replace defaults with the flash's SFDP opcode/dummy values.
5. `drivers/spi/spi-mem.c`: `spi_mem_default_supports_op()` rejects a phase whose bus width is absent from the SPI-device mode.

The decisive logic is:

```c
/* core.c: default W25Q256 1-1-4 settings */
spi_nor_set_read_settings(..., 0, 8,
                          SPINOR_OP_READ_1_1_4,
                          SNOR_PROTO_1_1_4);

/* spi-mem.c: a quad TX phase requires SPI_TX_QUAD */
case 4:
        if ((tx && (mode & SPI_TX_QUAD)) ||
            (!tx && (mode & SPI_RX_QUAD)))
                return 0;

/* core.c: choose highest remaining read capability */
best_match = fls(shared_hwcaps & SNOR_HWCAPS_READ_MASK) - 1;
```

Thus 1-4-4 is rejected because its address/dummy phase needs quad TX, while 1-1-4 is accepted because only its input data phase needs quad RX. SFDP describes the same protocol for this family. The selected normal read is therefore:

| Phase | Value | Wires |
|---|---:|---:|
| instruction | `0x6b` | 1 |
| address | 3 bytes, MSB first | 1 |
| dummy | 8 clocks = 1 byte | 1 |
| data | requested bytes | 4 |

This follows directly from [5.10 `core.c`](https://github.com/analogdevicesinc/linux/blob/9dfba10b795d0004ae90f2ab29dac0197c8a3b3e/drivers/mtd/spi-nor/core.c), [5.10 `sfdp.c`](https://github.com/analogdevicesinc/linux/blob/9dfba10b795d0004ae90f2ab29dac0197c8a3b3e/drivers/mtd/spi-nor/sfdp.c), and [5.10 `spi-mem.c`](https://github.com/analogdevicesinc/linux/blob/9dfba10b795d0004ae90f2ab29dac0197c8a3b3e/drivers/spi/spi-mem.c).

Quad enable is initialized by the SPI-NOR core according to the BFPT Quad Enable Requirement; the Winbond family uses the QE bit in status register 2. A failed QE setup would corrupt all `0x6b` reads, not only offsets below `0x200000`. The known-good FIT is strong operational evidence that instruction framing, dummy count, QE, and quad input are usable.

### Address width and EAR handling

`spi_nor_set_addr_width()` contains an ADI/Xilinx-specific exception:

```c
if (nor->addr_width == 3 && nor->mtd.size > 0x1000000) {
        if (parent compatible == "xlnx,zynq-qspi-1.0") {
                nor->addr_width = 3;
                nor->params->set_4byte_addr_mode(nor, false);
                status = read_ear(nor, nor->info);
                if (status < 0)
                        dev_warn(..., "failed to read ear reg\n");
                else
                        nor->curbank = status & EAR_SEGMENT_MASK;
        }
}
```

For Winbond, `set_4byte_addr_mode(false)` uses `EX4B` (`0xe9`). Its fixup then attempts to clear EAR because W25Q256FV can leave EAR=1 after EX4B:

```c
ret = spi_nor_write_enable(nor);
ret = spi_nor_write_ear(nor, 0);
return spi_nor_write_disable(nor);
```

However, `nor` was zero-allocated, so `curbank` initially equals zero. In 5.10, `spi_nor_write_ear()` returns without sending `WREAR` when requested EAR equals `curbank`. The intended post-EX4B clear can therefore be skipped before hardware EAR has been learned. This is a concrete latent bug.

Immediately afterward, generic `read_ear()` supports AMD via `BRRD` and ST/Micron, Macronix, and PMC via `RDEAR`, but **omits Winbond and returns `-EINVAL` without putting an EAR-read command on the wire**. That return produces the exact warning. Hence:

- **Code-derived fact:** the warning is deterministic for this 5.10 Winbond path.
- **Code-derived fact:** it does not report a failed electrical `0xc8` transaction; no Winbond `RDEAR` was attempted.
- **Hypothesis only:** EAR might nevertheless remain 1 because the intended clear can be optimized away.

For normal reads, `spi_nor_read()` calls `spi_nor_write_ear(nor, offset)` whenever address width is 3, splits only at a 16 MiB boundary, and calls `spi_nor_read_data()`. Below 16 MiB the computed EAR is zero; because software believes `curbank==0`, it sends no bank command. Above 16 MiB it uses `WREAR` (`0xc5`). The 5.10 Winbond branch does not explicitly issue WREN for that later bank switch, another upper-bank weakness. See [5.10 `core.c`](https://github.com/analogdevicesinc/linux/blob/9dfba10b795d0004ae90f2ab29dac0197c8a3b3e/drivers/mtd/spi-nor/core.c) and [5.10 `winbond.c`](https://github.com/analogdevicesinc/linux/blob/9dfba10b795d0004ae90f2ab29dac0197c8a3b3e/drivers/mtd/spi-nor/winbond.c).

### Controller address handling

The path then enters `drivers/spi/spi-zynq-qspi.c`:

- `zynq_qspi_exec_mem_op()` asserts CS once, calls `zynq_qspi_config_op()`, transmits instruction, address, dummy, and data as successive FIFO operations, then deasserts CS.
- The address loop emits `op->addr.nbytes` bytes MSB-first. With the Xilinx exception above this is exactly three bytes; high logical bits are represented only by EAR.
- `zynq_qspi_config_op()` chooses the smallest power-of-two divider that does not exceed the flash's requested 50 MHz.
- The controller uses I/O/FIFO mode, not an MTD memory-mapped alias. AMD's Zynq TRM likewise specifies explicit FIFO transfers and software removal of dummy-cycle receive data in I/O mode. [AMD UG585 I/O-mode sequence](https://docs.amd.com/r/en-US/ug585-zynq-7000-SoC-TRM/Example-I/O-Mode-Memory-Reads-and-Writes)

There is a real 5.10 implementation defect:

```c
xqspi->txbuf = (u8 *)&op->cmd.opcode;
... transmit command, advancing txbuf ...
for (i = 0; i < op->addr.nbytes; i++)
        xqspi->txbuf[i] = op->addr.val >> ...;
```

It uses storage adjacent to the command field as an address scratch buffer and writes through a pointer derived from a `const` operation. The 6.12 driver replaces this with a local `u8 opaddr[3]`. This is unsafe 5.10 code, but its address construction is identical for `0x000000`, `0x100000`, `0x120000`, and `0x200000`; it supplies no two-megabyte discriminator. See [5.10 Zynq QSPI driver](https://github.com/analogdevicesinc/linux/blob/9dfba10b795d0004ae90f2ab29dac0197c8a3b3e/drivers/spi/spi-zynq-qspi.c).

## Effective Tezuka Linux 6.12 comparison

Tezuka's base 6.12 implementation retains the same high-level path and the Xilinx 3-byte/EAR policy. Its important changes are:

| Area | ADI 5.10 | ADI 6.12 / effective Tezuka |
|---|---|---|
| address member | `addr_width` | `addr_nbytes` |
| controller limit | not formally advertised | `SPI_CONTROLLER_NO_4B`, maximum address width 3 |
| 4-byte SFDP table | can be parsed before Xilinx override | skipped when controller says `NO_4B` |
| controller address staging | writes after command pointer | dedicated `u8 opaddr[3]`; rejects >3 |
| LQSPI config | single-flash init clears it | config writes `0x6b` plus one dummy setting; TRM notes LQSPI config also affects non-linear operation |
| EAR write | older manufacturer-dependent WREN behavior | explicit WREN before generic EAR operation |
| Winbond EAR read | base ADI still omits Winbond | **Tezuka patch adds Winbond to `RDEAR` dispatch** |
| Winbond WREAR WREN | missing from 5.10 vendor test | **Tezuka patch adds Winbond** (in addition to newer surrounding enable logic) |

The effective Tezuka patch is `board/tezuka/common/patches/linux/01e1dcc848ad2b63ec32384c3846f686fae6652c/0004-ad5660.patch`; despite its name, it explicitly adds `CFI_MFR_WINBOND` to both `spi_nor_write_ear()` and `read_ear()`. Therefore Tezuka should actually send `RDEAR` (`0xc8`) and initialize `curbank` from the returned value instead of issuing the 5.10 warning.

These are genuine robustness fixes and make Tezuka a useful comparison reader. None introduces a special branch at 2 MiB. Relevant base sources: [6.12 `core.c`](https://github.com/analogdevicesinc/linux/blob/01e1dcc848ad2b63ec32384c3846f686fae6652c/drivers/mtd/spi-nor/core.c), [6.12 `sfdp.c`](https://github.com/analogdevicesinc/linux/blob/01e1dcc848ad2b63ec32384c3846f686fae6652c/drivers/mtd/spi-nor/sfdp.c), and [6.12 Zynq QSPI driver](https://github.com/analogdevicesinc/linux/blob/01e1dcc848ad2b63ec32384c3846f686fae6652c/drivers/spi/spi-zynq-qspi.c).

## U-Boot `g90401ce9ce` path

Relevant files/functions at the exact base commit are:

- `drivers/mtd/spi/sf_probe.c`: probe and bus claim.
- `drivers/mtd/spi/spi_flash.c`: W25Q256 table entry, `spi_flash_scan()`, `spi_flash_read_bar()`, `spi_flash_write_bar()`, and `spi_flash_cmd_read_ops()`.
- `drivers/mtd/spi/sf_ops.c`: `spi_flash_addr()` and common transfers (the tree contains parallel legacy copies; this configuration uses the enabled SPI-flash path).
- `drivers/spi/zynq_qspi.c`: `zynq_qspi_probe()`, `zynq_qspi_init_hw()`, `zynq_qspi_child_pre_probe()`, `zynq_qspi_xfer()`, and FIFO transfer helpers.
- `include/configs/zynq-common.h`: `read_sf` boot environment.
- `configs/zynq_pluto_defconfig`: enables `CONFIG_SPI_FLASH_BAR`, Winbond, and Zynq QSPI.

The W25Q256 entry is `ef4019`, 32 MiB, `RD_FULL`. The Zynq child setup advertises `SPI_OPM_RX_QOF`, so the fastest common command is Quad Output Fast Read `0x6b`; U-Boot assigns one dummy byte. With `CONFIG_SPI_FLASH_BAR` and the controller's default three-byte mode, it reads current EAR using `0xc8`, selects a bank with `0xc5` only when necessary, emits three address bytes, and splits at 16 MiB.

The Zynq U-Boot driver clears linear mode, uses FIFO/I/O transfers, holds CS across command-plus-data phases, and runs at the requested 50 MHz subject to its divider. Its boot script does:

```text
sf probe 0:0 50000000 0
sf read ${extraenv_load_address} 0xFF000 0x1000
sf read ${fit_load_address} 0x200000 ${fit_size}
iminfo ${fit_load_address}
```

Thus running U-Boot proves that the U-Boot path can retrieve the FIT from `0x200000`; it does **not** prove that U-Boot has validated bytes at offset zero. BootROM/FSBL executed the boot components, but their exact selected physical offsets cannot be inferred solely from the running U-Boot string. Sources: [U-Boot SPI flash core](https://github.com/analogdevicesinc/u-boot-xlnx/blob/90401ce9ce029e5563f4dface63914d42badf5bc/drivers/mtd/spi/spi_flash.c), [U-Boot Zynq QSPI driver](https://github.com/analogdevicesinc/u-boot-xlnx/blob/90401ce9ce029e5563f4dface63914d42badf5bc/drivers/spi/zynq_qspi.c), and [Pluto boot environment](https://github.com/analogdevicesinc/u-boot-xlnx/blob/90401ce9ce029e5563f4dface63914d42badf5bc/include/configs/zynq-common.h).

## Can a concrete code difference explain the split?

No, not from the established evidence.

For logical offsets `0`, `0x100000`, `0x120000`, and `0x200000`, Linux 5.10 computes EAR=0 and sends the same `6b + A23..A0 + 8 dummy clocks + quad data` operation. The first actual address/bank transition is 16 MiB, not 2 MiB. MTD fixed partitions are offset views of the same parent MTD. Neither a partition label nor filesystem probe changes the SPI-NOR read opcode.

The latent EAR=1 scenario deserves care: if EX4B left hardware EAR at 1 while software retained `curbank=0`, *all* logical reads below 16 MiB would actually come from physical `0x1000000 + logical_offset`. It could fit the observations only with an additional content-layout coincidence—specifically, physical `0x1200000` would need to contain the valid FIT observed at logical `0x200000`. That is testable, but it is not established and is weakened by coherent `mtd3`. It is not a mechanism that naturally changes behavior at 2 MiB.

Likewise, the unsafe address scratch buffer, LQSPI configuration difference, wrong dummy cycles, missing QE, excessive frequency, or generic quad framing would affect the FIT read in the same way as the low reads. Stable high entropy makes random bus noise particularly unlikely.

## Ranked explanations

1. **The low partitions contain the stable bytes Linux reports; their contents/layout do not match the expected current boot.bin/environment/JFFS2 images.** Strongest evidence: repeatability, entropy statistics, valid data through the same opcode/controller at `0x200000`, and absence of a 2 MiB code boundary. The unresolved BootROM fact means this is a leading explanation, not yet proof; a fallback/alternate boot image or boot-history detail must be located.
2. **Linux 5.10 starts with stale hardware EAR=1 while software assumes EAR=0.** There is a concrete initialization defect supporting this hypothesis. It predicts a +16 MiB alias for every logical address below 16 MiB, including `mtd3`'s beginning, so it explains both observations only if the upper-bank contents happen to provide the observed FIT at physical `0x1200000`. Tezuka's patched EAR read should expose/fix this state.
3. **A deterministic 5.10 Zynq-QSPI quad/address implementation defect.** The scratch-buffer defect and 6.12 LQSPI changes are concrete, but they are address-invariant over the region in question. They cannot independently explain the partition-aligned split.
4. **Electrical noise.** Poorly supported: five identical megabyte reads and a valid FIT through the same bus contradict it.

There is presently no evidence-based EAR/read-mode diagnosis that explains the split without an extra assumption about flash contents.

## One minimal read-only falsification experiment

At the already-running U-Boot prompt, hash a fresh U-Boot read of physical/logical offset zero using a safe RAM buffer:

```text
sf probe 0:0 50000000 0
sf read ${fit_load_address} 0x000000 0x100000
hash sha256 ${fit_load_address} 0x100000
```

This performs no erase/program/protect operation. Compare the printed digest with Linux's known `7df4a651b6c6ae3e31b36ee97dfabf85eedb3173e9565866f5866f906072c743`.

- **Same digest:** falsifies a Linux-only low-address read-path failure and strongly supports “those are the stored low-array bytes.” Investigation should then locate the actual BootROM-selected boot header/offset, still read-only.
- **Different digest:** falsifies the leading explanation and strongly elevates the Linux EAR/controller-state hypotheses. The exact U-Boot and Linux low dumps can then be tested for a 16 MiB alias without writing flash.

If this build lacks the `hash` command, the same experiment can use U-Boot `crc32` and a Linux CRC32 over the saved 1 MiB; SHA-256 is preferred because the Linux value is already available.

## Bottom line

The 5.10 EAR warning identifies missing Winbond dispatch, not demonstrated bank failure. Tezuka 6.12 repairs that dispatch and several controller/EAR details. Those repairs are worthwhile diagnostic differences, but the source trace contains no mechanism whose behavior changes at `0x200000`. Until U-Boot independently reads and hashes offset zero, attributing the observed split to EAR, quad mode, dummy cycles, or the Zynq address engine would overstate the evidence.
