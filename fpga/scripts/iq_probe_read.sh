#!/bin/sh
# Read an iq_probe / dac_pin_probe capture. Run ON the Pluto.
#   iq_probe_read.sh <gpio_base_hex> [nsamples]
#     0x43C20000  TX IQ probe      (before axis_to_adi_iq)
#     0x43C30000  RX IQ probe      (the demodulator's input)
#     0x43C40000  DAC pin probe    (after axis_to_adi_iq)
#
# Emits one 0x%08x word per line: {Q[15:0], I[15:0]}.
#
# THE YIELD MATTERS. Each sample costs two `devmem` forks, so 2048 samples is
# ~4096 processes and about 19 s of solid forking. That starves the watchdog
# daemon, and stock is a TEN SECOND hardware timeout -- the board resets
# mid-capture, and since the rootfs is a ramdisk it comes back with
# /lib/firmware and /tmp empty, so the bitstream is gone and every subsequent
# 0x43Cxxxxx access bus-errors. A brief sleep every 256 samples lets the daemon
# be scheduled and costs almost nothing. Run watchdog_relax.sh as well for
# margin; do not rely on either alone.
G=${1:?usage: iq_probe_read.sh <gpio_base_hex> [nsamples]}
N=${2:-2048}
S=$((G+8))
devmem $G 32 0                     # disarm: clears done, re-arms the one-shot
devmem $G 32 0x80000000            # arm
sleep 1
devmem $G 32 0xC0000000            # sel=1: status
echo "# status=$(devmem $S 32)  (bit12=done bit11=running bits[10:0]=waddr)"
i=0
while [ $i -lt $N ]; do
  devmem $G 32 $((0x80000000 + i))
  devmem $S 32
  i=$((i+1))
  [ $((i % 256)) -eq 0 ] && sleep 1   # let the watchdog daemon run
done
devmem $G 32 0
