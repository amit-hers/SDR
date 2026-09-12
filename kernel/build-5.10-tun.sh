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

# Toolchain: the SOFT-FLOAT gnueabi one. Ubuntu's gnueabihf gcc is hard-float
# and cannot accept the plain -march=armv7-a the kernel passes ("selected
# architecture lacks an FPU"), which surfaces as a wall of assembler errors --
# "selected processor does not support `isb' in ARM mode" -- that look like a
# kernel problem and are not.
export ARCH=arm CROSS_COMPILE=arm-linux-gnueabi-
command -v arm-linux-gnueabi-gcc >/dev/null || {
  echo "need: sudo apt install gcc-arm-linux-gnueabi flex bison bc libssl-dev libelf-dev" >&2; exit 1; }

if [[ ! -d "$SRC/.git" ]]; then
  mkdir -p "$SRC"; ( cd "$SRC" && git init -q && git remote add origin https://github.com/analogdevicesinc/linux.git )
fi
# Shallow fetch needs the FULL sha; an abbreviated one is rejected with
# "couldn't find remote ref".
( cd "$SRC" && git fetch --depth 1 origin "$COMMIT" && git checkout -q FETCH_HEAD )

cp "$HERE/pluto-5.10-tun.config" "$SRC/.config"
( cd "$SRC" && make olddefconfig >/dev/null && grep -q '^CONFIG_TUN=y' .config )
( cd "$SRC" && make -j"$(nproc)" zImage )

# The FIT kernel entry takes the RAW zImage. uImage is the same image plus a
# 64-byte u-boot header; putting that in a FIT gives the kernel two headers and
# the entry point lands in the wrong place.
echo "built: $SRC/arch/arm/boot/zImage"
grep -q "Universal TUN" <(strings -a "$SRC/vmlinux") \
  && echo "verified: TUN driver present" \
  || { echo "ERROR: TUN driver absent from the built kernel" >&2; exit 1; }
