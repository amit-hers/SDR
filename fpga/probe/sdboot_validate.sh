#!/bin/sh
# Post-SD-boot validation. Run ON the Pluto after booting with the SD card.
#
# Purpose: the AD9363 pin map is proven, but ad9361_dig_tune runs at DRIVER
# PROBE, which happens at boot against whatever PL the FSBL loaded. Booting
# from SD (with uEnv.txt's uenvcmd reconfiguring the PL from system_top.bin)
# is what finally lets the driver tune against the corrected map, which brings
# up cf_axi_adc and the DMA -- the prerequisite for any end-to-end frame test.
#
# Expected BEFORE this work:  "ad9361_dig_tune_delay: Tuning RX FAILED!"
#                             "cf_axi_adc: probe ... failed with error -5"
# Expected AFTER:             neither line; a cf-ad9361-lpc iio device present.

say() { echo; echo "== $* =="; }

say "1. how did we boot?"
BM=$(devmem 0xF800025C 32); V=$((BM & 0xF))
printf "   BOOT_MODE reg=%s  mode[3:0]=%d  -> " "$BM" "$V"
case $V in
  0) echo "JTAG" ;; 1) echo "Quad-SPI (NOT SD -- uEnv.txt on the card was never read)" ;;
  2) echo "NOR" ;; 4) echo "NAND" ;; 5) echo "SD CARD" ;; *) echo "unknown" ;;
esac
tr -d '\0' < /proc/device-tree/chosen/bootargs 2>/dev/null \
  | tr ' ' '\n' | grep -i MODEBOOT | sed 's/^/   /'
ls /dev/mmcblk* >/dev/null 2>&1 && echo "   SD visible to Linux: yes" \
                                || echo "   SD visible to Linux: no"

say "2. did dig_tune pass and cf_axi_adc probe?"
if dmesg | grep -q "Tuning RX FAILED"; then
  echo "   FAIL: dig_tune still failing -- the PL in use is NOT the corrected one."
  dmesg | grep -iE "dig_tune|cf_axi_adc" | tail -5 | sed 's/^/   /'
else
  echo "   dig_tune: no failure reported"
  dmesg | grep -iE "dig_tune|cf_axi_adc|ad9361_probe" | tail -5 | sed 's/^/   /'
fi

say "3. IIO stack"
for d in /sys/bus/iio/devices/iio:device*; do
  [ -e "$d/name" ] && printf "   %s = %s\n" "$(basename $d)" "$(cat $d/name)"
done
ls /sys/bus/iio/devices/ | grep -q . && \
  { ls /sys/bus/iio/devices/*/buffer >/dev/null 2>&1 \
      && echo "   buffer interface: PRESENT (DMA usable)" \
      || echo "   buffer interface: absent"; }

say "4. modem cores over AXI-Lite"
echo "   axi_ad9361 version 0x79020000 = $(devmem 0x79020000 32)"
echo "   demod AP_CTRL      0x43C00000 = $(devmem 0x43C00000 32)  (0x4 = idle)"
echo "   mod   AP_CTRL      0x43C10000 = $(devmem 0x43C10000 32)"

say "5. RX bring-up IN THE REQUIRED ORDER"
# adi_iq_to_axis latches overflow permanently if a sample arrives before the
# sink is ready, so the core MUST be running before ADC channels are enabled.
devmem 0x79020040 32 0x3
devmem 0x43C00010 32 0            # drain mode: consumes everything, emits nothing
devmem 0x43C00000 32 0x81         # ap_start + auto_restart, BEFORE channels
for ch in 0 1 2 3; do devmem $((0x79020400 + 64*ch)) 32 1; done
sleep 3
echo "   adc_dovf = $(devmem 0x79020088 32)   (0x0 = clean; 0x4 = overflow)"
echo "   dac_dunf = $(devmem 0x79024088 32)   (0x0 = clean; 0x1 = underflow)"

say "6. TX readiness (dac_dunf is the criterion; TX pin map is still INFERRED)"
echo "   dac rstn 0x79024040 = $(devmem 0x79024040 32)"
for ch in 0 1; do
  echo "   dac ch$ch cntrl = $(devmem $((0x79024400 + 64*ch)) 32)"
done
echo "   NOTE: keep any DMA transfer length an exact multiple of PKT_BYTES=1024."
