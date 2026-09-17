# Diagnostic u-boot: the QSPI FIT programmer

`fpga/jtag/program_fit.sh` loads a u-boot built from this patch into DDR over
JTAG and drives it through a mailbox in memory. It exists because the flash on
these boards cannot be written from the kernel that is being replaced: on stock
5.10 every mtd access lands 16 MiB further into the chip than asked, and reads
and writes share the shift, so a readback compares equal and flashcp reports
success having changed nothing at the address it named.

Running the programmer from DDR sidesteps that entirely -- it never boots from
the flash it is rewriting, and it addresses the chip directly rather than
through the kernel's spi-nor layer.

## Rebuilding

Base commit: `90401ce9ce029e5563f4dface63914d42badf5bc`
(u-boot-xlnx; the tree this was developed against was not kept, since it is a
257 MB clone whose only local content is the patch beside this file.)

    git clone https://github.com/Xilinx/u-boot-xlnx.git
    cd u-boot-xlnx
    git checkout 90401ce9ce029e5563f4dface63914d42badf5bc
    git apply /path/to/uboot/patches/0001-pluto-qspi-fit-programmer.patch
    make zynq_pluto_defconfig
    make CROSS_COMPILE=arm-linux-gnueabihf- -j$(nproc)

The build output that `program_fit.sh` wants is the ELF `u-boot` (entry
0x04000000), not `u-boot.bin`.

## What the patch does

- `common/main.c` -- the programmer itself. Mailbox at 0x03000000, image at
  0x08000000, readback at 0x0A000000, target fixed at 0x00200000. It refuses
  any other target, refuses to erase unless the CRC of the image in DDR matches
  what the host computed, bounds the erase inside mtd3, and silences the
  watchdog (10 s, reset-on-timeout) so a reset cannot land mid-erase.
  Verification reads back through opcode 0x13, a different path from the write,
  so a shared addressing fault cannot make a bad write look good.
  `fit_len == 0` is a read-only inspection run.
- `drivers/mtd/spi/spi_flash.c` -- bank changes are refused unless explicitly
  opted into, so nothing silently moves the 16 MiB window mid-operation.
- `include/configs/zynq-common.h`, `arch/arm/Makefile` -- build plumbing.
