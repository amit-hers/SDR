# Kernel patches

## 0001-spi-nor-normalise-EAR-bank-for-Winbond.patch

Applies to the ADI 6.12 tree (`adi-6.12.0`), `drivers/mtd/spi-nor/core.c`.

**Symptom.** Every Linux QSPI read and write lands 16 MiB higher than the
address asked for. `flashcp` verifies byte-for-byte and the board goes on
booting the previous image, because the readback is shifted by the same amount
and so agrees with the write. jffs2 never mounts, /mnt/jffs2 never persists,
and mtd0 reads as high-entropy data with no Zynq boot header.

**Proven, not inferred.** Dedicated 4-byte reads (opcode 0x13) over JTAG:

    Linux mtd0 @0 == physical 0x01000000   sha256 7df4a651...
    Linux mtd3 @0 == physical 0x01200000   sha256 08b01e96...

and the two banks held exactly what that predicts -- the running 5.10 image at
physical 0x00200000, and the 6.12 image we thought we had flashed sitting
unused at 0x01200000. Hardware EAR read 0x01. Chip is a Winbond W25Q256,
JEDEC EF 40 19.

**Cause.** `read_ear()` dispatches on manufacturer -- Spansion, Micron,
Macronix, PMC -- and returns `-EINVAL` for anything else without addressing the
chip, so the warning `spi-nor: failed to read ear reg` is not a comms failure.
`nor->curbank` therefore keeps its zero-initialised value while the hardware
EAR is 1, and `spi_nor_write_ear()` opens with
`if (ear == nor->curbank) return 0;` -- so for any address below 16 MiB the
register is never programmed. The write is skipped precisely because the stale
cache is believed.

Note `CFI_MFR_WINBOND` is 0x00DA (the CFI code) while the JEDEC manufacturer
byte is 0xEF, so adding that constant to the list would still not match.

**Fix.** Do not trust a read that cannot succeed. At probe, force the hardware
into bank 0 and let the cache follow, by invalidating `curbank` and calling
`spi_nor_write_ear(nor, 0)`.

ADI 5.10 has the same defect plus a second one: its `spi_nor_write_ear()` only
issues `spi_nor_write_enable()` for ST/Macronix/PMC, so even when it does reach
the write, a Winbond part receives `WREAR` with no `WREN` and silently ignores
it -- while `curbank` is updated as though it had worked. 6.12 already calls
write-enable unconditionally, so only the `curbank` half needs fixing there.

**Consequence for the board.** Nothing was damaged. The lower bank still holds
the working image, so it boots normally; with EAR=0 a normal write lands at
physical 0x200000, where u-boot reads.
