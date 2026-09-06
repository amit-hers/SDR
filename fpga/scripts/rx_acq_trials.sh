#!/bin/sh
# Cold-start acquisition trials for the framed link. Run ON the Pluto.
#   usage: rx_acq_trials.sh [trials] [PKT_BYTES]
#
# Each trial soft-resets the demodulator so AGC, Costas and the timing NCO all
# converge from their power-on state, then captures the first DMA packet. The
# host tool reports how far into that packet the first frame decoded.
#
# Sequencing is the whole difficulty, and every step below is load-bearing:
#
#  * soft_reset is only seen by a RUNNING core. A stalled HLS core has stopped
#    reading AXI-Lite and the write is silently lost, so a drain must be active
#    while reset is asserted -- and lock_count is read back to PROVE it landed
#    rather than assuming.
#  * /dev/iio:device4 is SINGLE-OPEN, and kill does not take effect while the
#    drain sits inside a read waiting for a whole packet -- 30 ms at
#    PKT_BYTES 32768. Without waiting for the descriptor to close, the capture
#    collides with the dying drain and the reset appears to land on alternate
#    trials only. That looked like a hardware fault and was pure sequencing.
#  * Reset is released AFTER the drain is gone, so acquisition starts at a known
#    instant with the device free.
#
# If a run is interrupted, the drain outlives the ssh client that started it --
# see free_capture_dev.sh.
set -u
N=${1:-20}; PKT=${2:-32768}
D=0x43C00000
rm -f /tmp/q*.bin
ok=0; i=1
while [ $i -le $N ]; do
  cat /dev/iio:device4 > /dev/null & CP=$!
  sleep 1
  devmem $((D+0x20)) 32 1
  sleep 1
  L=$(devmem $((D+0x18)) 32)
  kill -9 $CP 2>/dev/null
  wait $CP 2>/dev/null
  sleep 1
  devmem $((D+0x20)) 32 0
  dd if=/dev/iio:device4 of=/tmp/q$i.bin bs=$PKT count=1 2>/dev/null
  if [ "$L" = "0x00000000" ]; then ok=$((ok+1)); else echo "# trial $i RESET NOT SEEN ($L)"; fi
  i=$((i+1))
done
echo "# $ok of $N trials had a confirmed cold start, up=$(cut -d. -f1 /proc/uptime)s"
