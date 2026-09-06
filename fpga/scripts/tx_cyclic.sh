#!/bin/sh
# Cyclic transmission of $1 (IQ file) with buffer $2 samples.
# CYCLIC, not a re-run loop: iio_writedev -c hands ONE buffer to the DMA and the
# hardware repeats it seamlessly. Re-running the writer instead leaves a gap and
# a fresh differential-phase reference at every restart, and the driver
# re-selects the internal DDS each time the buffer is re-opened.
F=${1:-/tmp/tx3.iq}; NB=${2:-811776}; FS=${3:-7680000}
DB=0x79024000; P=/sys/bus/iio/devices/iio:device0; D=/sys/bus/iio/devices/iio:device3
for p in $(ps | grep '[i]io_writedev' | awk '{print $1}'); do kill -9 $p 2>/dev/null; done
sleep 2
echo "$FS" > $P/out_voltage_sampling_frequency
echo "$FS" > $P/in_voltage_sampling_frequency
echo 4000000 > $P/out_voltage_rf_bandwidth
echo 434000000 > $P/out_altvoltage1_TX_LO_frequency
echo 0 > $P/out_voltage0_hardwaregain
echo fdd > $P/ensm_mode
for a in 0 1 2 3; do echo 0 > $D/out_altvoltage${a}_*_raw 2>/dev/null; done
setsid sh -c "iio_writedev -b $NB -c cf-ad9361-dds-core-lpc voltage0 voltage1 < $F > /dev/null 2>/tmp/tx.err" < /dev/null &
sleep 5
# Source select AFTER the buffer is open: opening it re-points the channel at
# the internal DDS, so selecting DMA any earlier is silently undone. Everything
# else reads healthy while this is wrong -- buffer enabled, DMA cyclic, writer
# consuming the file at exactly the right rate -- and the DAC emits nothing.
for ch in 0 1; do devmem $((DB+0x418+64*ch)) 32 2; done
sleep 1
echo "# fs=$(cat $P/out_voltage_sampling_frequency) writers=$(ps | grep -c '[i]io_writedev') sel0=$(devmem $((DB+0x418)) 32) status=$(devmem $((DB+0x5C)) 32) up=$(cut -d. -f1 /proc/uptime)s"
