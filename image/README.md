# Building a 6.12 image that fits

The stock tezuka 6.12 image is 29.6 MB and cannot be written to these boards:
mtd3 starts at 2 MB and the w25q256 aliases past 16 MB, so the write window is
14 MB (see the `flash-16mb-boundary` note). u-boot imposes a second, LOWER
ceiling -- it reads only `fit_size` bytes -- so the practical limit is
**12,395,781 B**, the size of an image known to boot on this hardware
(`uboot-fit-size`).

    stock          ours
    kernel  6.83   4.25 MB   trim_kernel_config.py, two passes
    ramdisk 20.24  4.64 MB   prune_rootfs.py
    fpga     2.44  2.44 MB
    dtb      0.02  0.02 MB
    total   29.6  11.35 MB

## prune_rootfs.py

Prunes tezuka's 6.12 userspace by **actual ELF dependency closure**, not by
guesswork, and refuses to pack a broken rootfs.

* Uses `readelf -d` for DT_NEEDED. A hand-rolled ELF parser got the program
  header field order wrong (read `p_paddr` as `p_filesz`) and cheerfully
  deleted `libc.so.6` while reporting that the image fit. Do not hand-roll it.
* **Validation gate:** after pruning, every surviving binary must resolve every
  NEEDED library, or the build aborts. This caught `/root/websrv` needing
  libcivetweb after civetweb was dropped.
* `FORCE` keeps the runtime our own tools need. They live in jffs2, not in this
  tree, so the closure cannot see them.
* Drops the DATV scripts and `S95bgcript`. `watchdatveasy.sh` writes
  `TX_LO_powerdown=1` on every `ensm_mode=rx` -- that, not the kernel, is what
  silenced the transmitter (`kernel-612-breaks-transmit`).
* Repoints root's shell from `/bin/bash` (which it deletes, saving ~1 MB) to
  busybox `sh`. Without this dropbear authenticates, finds no shell to exec,
  and every ssh session closes with **no output and no error**.
* `/opt/vfat.img` is 30 MB raw but 41 KB compressed -- deleting it saves
  essentially nothing. Measure before trusting a candidate.
* `libiio` links `libxml2`, so libxml2 stays.

## mk-boot-dtb.sh

Builds the DTB for a JTAG-hosted boot. `linux,initrd-end` is computed from the
ramdisk file every time: a stale value truncates the initramfs and the kernel
cannot mount root, silently, because there is no console. The watchdog node is
disabled here too -- note it carries `status = "okay"` LATER in the block, and
the last value wins, so the existing property must be rewritten rather than a
new one prepended.

See `fpga/jtag/README.md` for the boot procedure itself.
