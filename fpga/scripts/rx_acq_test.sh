#!/bin/sh
# Statistical acquisition test for the fabric demodulator. Run ON the receiving
# Pluto. Usage: rx_acq_test.sh [trials]
#
# THE ONE THING THAT MAKES THIS VALID: the demodulator must be RUNNING when
# soft_reset is asserted. A stalled HLS core -- and it stalls the moment its
# output backs up -- stops re-reading its AXI-Lite registers, so the reset is
# silently ignored and the bytes you then read are stale buffer contents. That
# reads as "the demodulator outputs random noise" (entropy 7.9 bits/byte,
# autocorrelation at the payload period 24.7% = chance) and is nothing of the
# kind. Start a drain, reset while it runs, release, then read.
#
# `reset_ok` must print 0x00000000. If it does not, the reset did not happen and
# the trial is meaningless -- do not interpret the result.
#
# Expects: modem PL loaded, ADC core out of reset, ADC channel format 0x51,
# a transmitter on the air, and RSSI well below 100 dB. Gate on the RSSI line:
# at 434 MHz a good link reads 64-65 dB with the AGC regulating around 43 dB.
N=${1:-8}
D=0x43C00000
R=/sys/bus/iio/devices/iio:device4
P=/sys/bus/iio/devices/iio:device0
pkill -f "cat /dev/iio:device4" 2>/dev/null
echo 1 > $R/buffer/enable 2>/dev/null
echo "# GATE rssi=$(cat $P/in_voltage0_rssi) gain=$(cat $P/in_voltage0_hardwaregain) up=$(cut -d. -f1 /proc/uptime)s"
i=1
while [ $i -le $N ]; do
  cat /dev/iio:device4 > /dev/null &      # drain, so the core is RUNNING
  CP=$!
  sleep 1
  devmem $((D+0x20)) 32 1                 # reset is seen only because it runs
  L=$(devmem $((D+0x18)) 32)              # must read 0x00000000
  devmem $((D+0x20)) 32 0
  kill $CP 2>/dev/null
  dd if=/dev/iio:device4 of=/tmp/a$i.bin bs=1024 count=8 2>/dev/null
  echo "# trial $i reset_ok=$L bytes=$(wc -c < /tmp/a$i.bin) locks=$(devmem $((D+0x18)) 32)"
  i=$((i+1))
done
# Uptime at both ends: these boards spontaneously reset (REBOOT_STATUS SRST_B)
# and the rootfs is a ramdisk, so a mid-test reset wipes state and silently
# invalidates everything above.
echo "# up_end=$(cut -d. -f1 /proc/uptime)s rssi_end=$(cat $P/in_voltage0_rssi)"
