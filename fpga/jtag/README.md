# Booting a kernel over JTAG, without touching the flash

The Xilinx Debugger (FT4232H, `0403:6011`) on this bench reaches the Zynq DAP,
which makes it possible to load a kernel straight into DDR and run it. That
matters because writing mtd3 does NOT reliably change what these boards boot
(see the `uboot-fit-size` and `unit-a-ignores-mtd3` notes), and this path
sidesteps the flash entirely.

    JTAG chain: zynq_pl.bs 0x23727093 (XC7Z020, silicon rev 3.0)
                zynq.cpu   0x4ba00477 (Cortex-A9 DAP)

`ftdi_sio` claims all four FT4232H channels, so release channel A first:

    echo 1-7:1.0 | sudo tee /sys/bus/usb/drivers/ftdi_sio/unbind

## Five things that each cost a boot attempt

1. **The MMU must go off BEFORE any physical-address access.** With Linux
   running, openocd reads go through its page tables and `0xF8005000` is
   unmapped -- `dfsr=0x00000005`, a translation fault. Clear SCTLR M/C/I first
   (`arm mrc/mcr 15 0 1 0 0`), then physical addressing works.

2. **The SCU address filter starts at 0x00100000.** Everything below 1 MB is
   routed to OCM, so loading a kernel at 0x00008000 aborts with `dfsr=0x1808`
   (synchronous external abort). Zero `0xF8F00040`. If you also remap OCM high
   (`0xF8000910 = 0xF`) you MUST do both, or low memory decodes to nothing.

3. **The watchdog resets the board mid-boot.** `cdns,wdt-r1p2` at 0xF8005000,
   `timeout-sec = 10`, `reset-on-timeout`. Nothing feeds it once Linux is
   halted, so the PS resets at ~10 s and the BootROM -- which cannot re-read a
   flash Linux left in 4-byte address mode -- wedges in OCM at PC 0xffffff34.
   Clearing ZMR (`0x00ABC000`) is not enough: the kernel's own driver re-enables
   it on probe. Set `status = "disabled"` on the node in the DTB, and note the
   node already carries `status = "okay"` LATER in the block -- the last one
   wins, so patch that property rather than prepending a new one.

4. **CPU1 stays halted.** `targets zynq.cpu0; resume` resumes only core 0, so
   SMP bring-up hangs forever at `CPU1: thread -1, cpu 1`. Boot with
   `maxcpus=1`.

5. **Sourced scripts do not echo command results.** `mdw`, `load_image` and
   `pld load` print nothing when run from a `-f` file (only `-c` prints return
   values), so verify loads explicitly with `read_memory` + `echo`.

## Reading the kernel log with no console

The debugger cable carries JTAG only -- all four UART channels read zero bytes
even during a boot -- so there is no console. Instead dump the printk buffer
out of DDR. Get its address from the kernel you built:

    grep __log_buf build/System.map        # e.g. c0cb6250

Physical = virtual - PAGE_OFFSET (0xC0000000). Read it with the MMU OFF, at the
physical address; reading the virtual address through a live MMU returns
`dfsr=0x00000008` even with `cortex_a dacrfixup on`.

Beware: the zImage decompressor relocates itself to just above the decompressed
image, which on this kernel lands around 0x00CB0000 -- the same region. If the
dump is full of strings like "invalid distance too far back", you are reading
the decompressor, not the log, and the kernel has not started yet.

## Result

A locally built 6.12 kernel DOES boot on this hardware:

    Linux version 6.12.0-g39414bc4038b-dirty (amither@...)
    (arm-linux-gnueabi-gcc 13.3.0) #1 SMP PREEMPT

reaching L2 cache init, Zynq clocks, timers and SMP bring-up. That disproves
the theory that our kernel builds were bad -- the fault is in the flash boot
path, not the build.

## boot612.sh

`fpga/jtag/boot612.sh` wraps the whole sequence. Set `SDR_SCRATCH` to the
directory holding `kbuild612/` and `rd612/`, power-cycle the board first, and
run it. It is NOT persistent -- a power cycle returns the board to flash.

It exists because nothing written from Linux changes what these boards boot:
ADI's own USB-drive update path was run end to end and the board still came up
on 5.10 after a power cycle. Until DFU or the u-boot console is available, this
is the only way to run 6.12 on the hardware.
