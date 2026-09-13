#!/usr/bin/env bash
# Rebuild the ADI 5.10 kernel with CONFIG_TUN=y for the Pluto+.
#
# WHY: the Pluto+ 6.12.77 firmware never powers up the AD9363 transmit LO, so
# nothing radiates (see the memory note kernel-612-breaks-transmit). ADI's 5.10
# does transmit -- proven on hardware, +20.7 dB with crest 1.13 on a self-listen
# test. But 5.10 ships without CONFIG_TUN, which sdr_bridge needs. Enabling that
# one symbol is a far smaller change than moving the bridge to AF_PACKET or
# debugging the 6.12 regression, and it builds on a configuration already known
# to work end to end.
#
# The config is the BOARD'S OWN, captured from /proc/config.gz on a running
# unit, with exactly one symbol flipped. Verified: the only diff is
#   -# CONFIG_TUN is not set
#   +CONFIG_TUN=y
set -euo pipefail
COMMIT=9dfba10b795d0004ae90f2ab29dac0197c8a3b3e   # plutosdr-fw v0.35's linux submodule
SRC="${1:-$PWD/linux-adi}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# TOOLCHAIN -- both halves of this matter, and each was learned by breaking it.
#
# SOFT-FLOAT gnueabi, not gnueabihf. Ubuntu's gnueabihf gcc is hard-float and
# cannot accept the plain -march=armv7-a the kernel passes ("selected
# architecture lacks an FPU"), which surfaces as a wall of assembler errors --
# "selected processor does not support `isb' in ARM mode" -- that look like a
# kernel problem and are not.
#
# GCC 10, not 13. A GCC 13 build of this tree COMPILES CLEANLY AND DOES NOT
# BOOT: flashed to both radios, neither came back, neither even presented
# u-boot's DFU gadget. 5.10 predates GCC 13 and the original was built with
# GCC 8.2.0. GCC 10 is contemporaneous with 5.10 and produces a kernel within
# 1 KB of the original (4211816 B against 4212824 B), where the GCC 13 build
# was 23 KB smaller -- a useful smoke test before flashing anything.
#
# BUILD SIZE IS NOT PROOF OF BOOT. Verify on ONE radio and keep the other on a
# known-good image until the new one is confirmed; do not flash both.
GCCVER="${GCCVER:-10}"
export ARCH=arm CROSS_COMPILE=arm-linux-gnueabi-
if command -v "arm-linux-gnueabi-gcc-$GCCVER" >/dev/null; then
  TC=$(mktemp -d); ln -sf "/usr/bin/arm-linux-gnueabi-gcc-$GCCVER" "$TC/arm-linux-gnueabi-gcc"
  for t in ld as objcopy objdump ar nm strip ranlib; do
    ln -sf "/usr/bin/arm-linux-gnueabi-$t" "$TC/arm-linux-gnueabi-$t" 2>/dev/null || true
  done
  export PATH="$TC:$PATH"
else
  echo "need: sudo apt install gcc-$GCCVER-arm-linux-gnueabi flex bison bc libssl-dev libelf-dev" >&2
  echo "      (a GCC 13 build of this tree compiles but does NOT boot)" >&2
  exit 1
fi
echo "toolchain: $(arm-linux-gnueabi-gcc --version | head -1)"

if [[ ! -d "$SRC/.git" ]]; then
  mkdir -p "$SRC"; ( cd "$SRC" && git init -q && git remote add origin https://github.com/analogdevicesinc/linux.git )
fi
# Shallow fetch needs the FULL sha; an abbreviated one is rejected with
# "couldn't find remote ref".
( cd "$SRC" && git fetch --depth 1 origin "$COMMIT" && git checkout -q FETCH_HEAD )

cp "$HERE/pluto-5.10-tun.config" "$SRC/.config"
( cd "$SRC" && make olddefconfig >/dev/null )
# BOTH symbols matter and BOTH are absent from the stock ADI config:
#   CONFIG_TUN  -- sdr_bridge's tun backend
#   CONFIG_MACB -- the Cadence GEM driver for the Pluto+'s RJ45. The standard
#                  Pluto has no Ethernet port so ADI does not build it, which
#                  is why a stock 5.10 board has only lo and usb0. Without it
#                  there is no eth0 and the AF_PACKET backend has nothing to
#                  bind to, so switching backends does NOT avoid a kernel
#                  rebuild -- it only changes which symbol is needed.
for sym in CONFIG_TUN CONFIG_MACB; do
  grep -q "^$sym=y" "$SRC/.config" || { echo "ERROR: $sym did not survive olddefconfig" >&2; exit 1; }
done
( cd "$SRC" && make -j"$(nproc)" zImage )
SZ=$(stat -c%s "$SRC/arch/arm/boot/zImage")
echo "zImage $SZ B (original ADI GCC 8.2 build is 4212824 B; a wildly different size is a warning sign)"

# The FIT kernel entry takes the RAW zImage. uImage is the same image plus a
# 64-byte u-boot header; putting that in a FIT gives the kernel two headers and
# the entry point lands in the wrong place.
echo "built: $SRC/arch/arm/boot/zImage"
grep -q "Universal TUN" <(strings -a "$SRC/vmlinux") \
  && echo "verified: TUN driver present" \
  || { echo "ERROR: TUN driver absent from the built kernel" >&2; exit 1; }
