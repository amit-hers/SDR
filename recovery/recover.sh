#!/bin/bash
set -e
cd /home/parallels/Documents/SDR/recovery

BOOT_BIN="../tezuka-plutoplus-v0.3.5-7cf6171/sdimg/BOOT.bin"
PLUTO_DFU="../tezuka-plutoplus-v0.3.5-7cf6171/pluto.dfu"

echo "=== HamGeek Pluto+ JTAG Recovery ==="
echo "Using: $BOOT_BIN ($(stat -c%s $BOOT_BIN) bytes)"

# Test JTAG connection first
openocd -f zynq7020_ft4232.cfg \
  -c "init; jtag arp_init; scan_chain; shutdown" 2>&1

echo "JTAG scan OK — proceeding with flash..."
