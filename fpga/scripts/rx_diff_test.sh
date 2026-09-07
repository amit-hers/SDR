#!/bin/sh
# Differential decode on hardware. $1 = bitstream, $2 = diff_mode (0|1)
# Run ON the receiving board with the modem PL.
BIT=$1; DIFF=${2:-1}
P=$IIO_PHY; R=$IIO_RX; D=0x43C00000
echo 0 > /sys/class/fpga_manager/fpga0/flags
echo "$BIT" > /sys/class/fpga_manager/fpga0/firmware; sleep 3
devmem 0x79020040 32 0x3; devmem 0x7C400080 32 0
echo 7680000 > $P/in_voltage_sampling_frequency
echo 4000000 > $P/in_voltage_rf_bandwidth
echo 434000000 > $P/out_altvoltage0_RX_LO_frequency
echo slow_attack > $P/in_voltage0_gain_control_mode
devmem $((D+0x10)) 32 1                  # demod enable
devmem $((D+0x28)) 32 $DIFF              # diff_mode (0x28 per the generated _hw.h)
devmem $((D+0x00)) 32 0x81
echo 1 > $R/scan_elements/in_voltage0_en 2>/dev/null
echo 1 > $R/scan_elements/in_voltage1_en 2>/dev/null
echo 512 > $R/buffer/length; echo 1 > $R/buffer/enable 2>/dev/null
for ch in 0 1 2 3; do devmem $((0x79020400 + 64*ch)) 32 0x51; done
sleep 2
echo "# diff_mode readback = $(devmem $((D+0x28)) 32)  rssi=$(cat $P/in_voltage0_rssi) gain=$(cat $P/in_voltage0_hardwaregain)"
i=1
while [ $i -le 6 ]; do
  cat $IIO_RX_DEV > /dev/null & CP=$!
  sleep 1
  devmem $((D+0x20)) 32 1
  L=$(devmem $((D+0x18)) 32)
  devmem $((D+0x20)) 32 0
  kill $CP 2>/dev/null
  dd if=$IIO_RX_DEV of=/tmp/df$i.bin bs=1024 count=8 2>/dev/null
  echo "# trial $i reset_ok=$L bytes=$(wc -c < /tmp/df$i.bin)"
  i=$((i+1))
done
echo "# up=$(cut -d. -f1 /proc/uptime)s rssi_end=$(cat $P/in_voltage0_rssi)"
