#!/bin/sh
# Verify the running bitstream matches what this software expects. Run ON the
# Pluto, before any modem bring-up.
#   usage: fpga_abi_check.sh [expected_abi] [expected_regmap]
#   exit 0 = compatible, 1 = incompatible, 2 = no identity block at all
#
# WHY THIS REFUSES RATHER THAN WARNS. A mismatched bitstream does not fail
# loudly on its own: the register reads still succeed, they just mean something
# else. That has already happened here -- adding one HLS scalar shifted
# bpsk_mode from 0x18 to 0x24, and every script writing the old address carried
# on "working" while configuring a different register entirely. The resulting
# symptoms looked like RF problems and were chased as such. Refusing to start is
# cheaper than any amount of debugging downstream of a silent mismatch.
set -u
BASE=0x43C50000
WANT_ABI=${1:-3}
WANT_REGMAP=${2:-3}
MAGIC_EXPECT=0x5344524C

rd() { devmem $(printf '0x%X' $((BASE + $1))) 32 2>/dev/null; }

MAGIC=$(rd 0x00)
if [ -z "$MAGIC" ]; then
  echo "ERROR: no response at $BASE -- this bitstream has no identity block."
  echo "       Either it predates the version registers or the PL is not loaded."
  echo "Refusing to start modem."
  exit 2
fi
# devmem prints 0xXXXXXXXX; normalise for comparison.
norm() { printf '0x%08X' "$1" 2>/dev/null || echo "$1"; }
if [ "$(norm $MAGIC)" != "$(norm $MAGIC_EXPECT)" ]; then
  echo "ERROR: FPGA identity magic mismatch"
  echo "  read:     $(norm $MAGIC)"
  echo "  expected: $(norm $MAGIC_EXPECT)"
  echo "Refusing to start modem."
  exit 2
fi

VER=$(rd 0x04); ABI=$(rd 0x08); REGMAP=$(rd 0x0C); EPOCH=$(rd 0x10); SHA=$(rd 0x14)
ABI_D=$((ABI)); REGMAP_D=$((REGMAP))

if [ "$ABI_D" != "$WANT_ABI" ]; then
  echo "ERROR: FPGA/software incompatibility"
  echo "FPGA ABI: $ABI_D"
  echo "Software expects: $WANT_ABI"
  echo "Refusing to start modem."
  exit 1
fi
if [ "$REGMAP_D" != "$WANT_REGMAP" ]; then
  echo "ERROR: FPGA/software incompatibility"
  echo "Register map: $REGMAP_D"
  echo "Software expects: $WANT_REGMAP"
  echo "Refusing to start modem."
  exit 1
fi

# Decode 0x00MMmmpp for humans; the raw word stays available above.
MAJ=$(( (VER >> 16) & 0xFF )); MIN=$(( (VER >> 8) & 0xFF )); PAT=$(( VER & 0xFF ))
echo "FPGA v${MAJ}.${MIN}.${PAT}  ABI $ABI_D  regmap $REGMAP_D  build $(norm $EPOCH)  src $(norm $SHA)  OK"
exit 0
