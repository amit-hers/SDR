#!/bin/sh
# Board-side runbook for the DATA_CLK capture. Usage: capture_pn.sh [bist_mode]
#   bist_mode: 0=off (ADC noise, control), 1=INJ_TX, 2=INJ_RX (default)
#
# Every step is VERIFIED rather than assumed -- the whole reason this design
# carries SPI is that a debugfs write reports success even when the SPI pins are
# not connected, which would silently yield a noise capture that no decode can
# ever solve.
#   ctrl axi_gpio : 0x41200000 ch1 (out, bit0=arm), 0x41200008 ch2 (in, bit0=done)
#   capture BRAM  : 0x40000000, 1024 x 32-bit
MODE=${1:-2}
D=/sys/kernel/debug/iio/iio:device0
P=/sys/bus/iio/devices/iio:device0

rdreg() { echo "$1" > $D/direct_reg_access 2>/dev/null; cat $D/direct_reg_access 2>/dev/null; }

echo "== 1. SPI alive? (reg 0x37 = product ID; 0x0 means the SPI pins are dead)"
ID=$(rdreg 0x37)
echo "   reg 0x37 = $ID"
case "$ID" in 0x0|"") echo "   FAIL: SPI not reachable, aborting"; exit 1;; esac

echo "== 2. force a known radio state"
echo fdd > $P/ensm_mode 2>/dev/null
echo "   ensm_mode = $(cat $P/ensm_mode 2>&1)"

echo "== 3. enable BIST mode $MODE and verify it reached the chip"
echo "$MODE" > $D/bist_prbs 2>/dev/null
# REG_BIST_CONFIG 0x3F4: BIST_ENABLE=bit0, BIST_CTRL_POINT=bits[3:2]
#   INJ_TX -> 0x01, INJ_RX -> 0x09, disabled -> 0x00
echo "   bist_prbs   = $(cat $D/bist_prbs 2>&1)"
echo "   reg 0x3F4   = $(rdreg 0x3f4)   (expect 0x9 for mode 2, 0x1 for mode 1)"

echo "== 4. arm the DATA_CLK capture"
devmem 0x41200000 32 0            # disarm -> clears done, re-arms the one-shot
devmem 0x41200000 32 1            # arm
i=0
while [ $i -lt 50 ]; do
  DN=$(devmem 0x41200008 32)
  [ $((DN & 1)) -eq 1 ] && break
  i=$((i+1))
done
echo "   done flag = $DN after $i polls"
case "$((DN & 1))" in 0) echo "   FAIL: capture never completed -> no DATA_CLK on N20"; exit 1;; esac

echo "== 5. BRAM dump (1024 words)"
echo "-----BEGIN CAPTURE-----"
dd if=/dev/mem bs=4096 count=1 skip=262144 2>/dev/null | od -An -tx4 -v
echo "-----END CAPTURE-----"
devmem 0x41200000 32 0
