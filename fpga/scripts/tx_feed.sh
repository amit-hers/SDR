#!/bin/sh
# Stream a byte file through the fabric modulator. Run ON the Pluto, after
# tx_fabric.sh.  usage: tx_feed.sh <file> [buffer_samples]
#
# The write to the TX char device blocks until the DAC consumes, so the file is
# paced by the link itself -- no rate limiting is needed or wanted here.
#
# Channel source is selected AFTER the buffer is enabled: opening the buffer
# re-points the DAC at the internal DDS, so an earlier write is silently undone
# and the DAC emits nothing while every register still reads healthy.
set -u
F=${1:-/tmp/vid.bytes}; NB=${2:-32768}
. "$(dirname "$0")/iio_lookup.sh"
DB=0x79024000; D=$IIO_TX
echo 0 > $D/buffer/enable 2>/dev/null || true
echo 1 > $D/scan_elements/out_voltage0_en 2>/dev/null || true
echo 1 > $D/scan_elements/out_voltage1_en 2>/dev/null || true
echo "$NB" > $D/buffer/length
echo 1 > $D/buffer/enable
for ch in 0 1; do
  devmem $((DB+0x418+64*ch)) 32 2
done
echo "# feeding $(wc -c < "$F") B, src=$(devmem $((DB+0x418)) 32), start=$(cut -d. -f1 /proc/uptime)s"
cat "$F" > "$IIO_TX_DEV"
echo "# done, end=$(cut -d. -f1 /proc/uptime)s"
