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
#  * /dev/iio:device4 is SINGLE-OPEN, so the drain must be gone before the
#    capture or the two collide and the reset appears to land on alternate
#    trials only -- which reads as an intermittent hardware fault.
#  * THE DRAIN IS BOUNDED, NOT KILLED, and reset is RELEASED BEFORE waiting on
#    it. Two ways this hangs otherwise, both met in practice: a drain blocked
#    inside the driver's read does not die on SIGKILL, so `kill` then `wait`
#    hung this script for twenty minutes on trial 1; and the reset path emits no
#    bytes at all, so a drain still waiting for its count blocks forever while
#    reset is asserted. The count must also outlast the sleep -- at 64 packets
#    that is ~1.9 s against 1 s -- or the drain has already ended when reset is
#    written, the core is stalled, and the write is silently dropped.
#
# RESOLUTION LIMIT -- READ THIS BEFORE TRUSTING THE NUMBERS. What is measured is
# how far into the captured packet the first frame decodes, so the packet is the
# instrument and PKT_BYTES is its granularity: 0.95 ms at 1024, 7.6 ms at 8192,
# 30 ms at 32768. Cold-start acquisition is about 0.5-2 ms, so at 32768 the core
# has long since locked before the capture even begins and every trial reads
# zero. Run this against a SMALL-PKT_BYTES bitstream. The loops do not depend on
# PKT_BYTES -- it sets the DMA transfer size and nothing else -- so a figure
# measured at 1024 or 8192 describes the demodulator at any of them.
#
# If a run is interrupted anyway, the drain outlives the ssh client that started
# it -- see free_capture_dev.sh.
set -u
N=${1:-20}; PKT=${2:-32768}
D=0x43C00000
rm -f /tmp/q*.bin
ok=0; i=1
while [ $i -le $N ]; do
  dd if=/dev/iio:device4 of=/dev/null bs=$PKT count=64 2>/dev/null & CP=$!
  sleep 1
  devmem $((D+0x20)) 32 1          # seen, because the drain above is still running
  L=$(devmem $((D+0x18)) 32)   # reset branch writes lock_count=0, so this proves it landed
  devmem $((D+0x20)) 32 0          # release FIRST: under reset the core emits
  wait $CP                         # nothing, so the drain would never finish
  dd if=/dev/iio:device4 of=/tmp/q$i.bin bs=$PKT count=1 2>/dev/null
  if [ "$L" = "0x00000000" ]; then ok=$((ok+1)); else echo "# trial $i RESET NOT SEEN ($L)"; fi
  i=$((i+1))
done
echo "# $ok of $N trials had a confirmed cold start, up=$(cut -d. -f1 /proc/uptime)s"
