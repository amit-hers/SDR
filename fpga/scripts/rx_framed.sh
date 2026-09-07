#!/bin/sh
# Continuous framed reception at 434 MHz. Run after every PL reload.
FS=${1:-7680000}
. "$(dirname "$0")/iio_lookup.sh"
P=$IIO_PHY; R=$IIO_RX; D=0x43C00000
echo 0 > $R/buffer/enable 2>/dev/null
devmem 0x79020040 32 0x3            # ADC core out of reset: it gates all of l_clk
devmem 0x7C400080 32 0              # DMA IRQ unmask -- re-masked by every PL reload
echo "$FS" > $P/in_voltage_sampling_frequency
echo 4000000 > $P/in_voltage_rf_bandwidth
echo 434000000 > $P/out_altvoltage0_RX_LO_frequency
echo slow_attack > $P/in_voltage0_gain_control_mode
# MODEM CORES FIRST, ADC CHANNELS SECOND. The other order latches the sticky
# overflow flag on the first sample -- the adapter has a stream sink that is not
# yet draining and an ADC that cannot be told to wait -- and it never clears.
devmem $((D+0x10)) 32 1             # demod enable
devmem $((D+0x28)) 32 1             # diff_mode = 1, matching the transmitter
devmem $((D+0x00)) 32 0x81          # ap_start + auto_restart
# 0x51, not 0x71: dfmt_type must be 0 on ADC core 10.03 or the demodulator input
# rails at 12-bit full scale regardless of RF.
for ch in 0 1 2 3; do devmem $((0x79020400 + 64*ch)) 32 0x51; done
echo 1 > $R/scan_elements/in_voltage0_en 2>/dev/null
echo 1 > $R/scan_elements/in_voltage1_en 2>/dev/null
# At least PKT_BYTES (32768) worth, or the DMA transfer is truncated before the
# packetizer's TLAST arrives. Scan size is 4 bytes, so 16384 here is 65536 bytes
# -- one packet of headroom.
echo 16384 > $R/buffer/length
echo 1 > $R/buffer/enable 2>/dev/null
sleep 1
echo "# fs=$(cat $P/in_voltage_sampling_frequency) l_clk=$(devmem 0x79020054 32) rssi=$(cat $P/in_voltage0_rssi) gain=$(cat $P/in_voltage0_hardwaregain) diff=$(devmem $((D+0x28)) 32) ap=$(devmem $((D+0x00)) 32) up=$(cut -d. -f1 /proc/uptime)s"
