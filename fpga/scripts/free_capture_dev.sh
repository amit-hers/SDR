#!/bin/sh
# Release $IIO_RX_DEV after an aborted capture. Run ON the Pluto.
#
# The device is SINGLE-OPEN. A drain left behind by an interrupted run holds it,
# and every later capture then returns 0 bytes with EBUSY -- which reads like a
# dead receive path and sent one debugging session chasing the radio.
#
# Three things make this harder than `pkill`, and all three were met in practice:
#
#  * Killing the local ssh client does NOT stop what it launched on the board.
#    An interrupted acquisition loop keeps running under init and respawns the
#    drain as fast as it is killed, so the SCRIPT must die before its children.
#  * A drain blocked inside the driver's read does not die on SIGKILL at all.
#    At PKT_BYTES 32768 a read waits ~30 ms for a whole packet, and the process
#    sits in the kernel where the signal cannot reach it. Dropping
#    buffer/enable is what actually returns it.
#  * Leaving the core undrained stalls it: a stalled HLS core stops reading
#    AXI-Lite, so soft_reset is silently ignored afterwards. If reads still do
#    not come back, reload the PL and re-run rx_framed.sh -- that is the
#    reliable recovery, not more signals.
set -u
. "$(dirname "$0")/iio_lookup.sh"
R=$IIO_RX

for p in $(ps | grep '[a]cqframes.sh' | awk '{print $1}'); do kill -9 $p 2>/dev/null; done
for p in $(ps | grep '[r]xframes.sh'  | awk '{print $1}'); do kill -9 $p 2>/dev/null; done
sleep 1
for p in $(ps | grep '[c]at /dev/iio'   | awk '{print $1}'); do kill -9 $p 2>/dev/null; done
for p in $(ps | grep '[d]d if=/dev/iio' | awk '{print $1}'); do kill -9 $p 2>/dev/null; done
for p in $(ps | grep '[t]x_feed.sh'     | awk '{print $1}'); do kill -9 $p 2>/dev/null; done
# A feed is `cat <file> > <chardev>`, so it does not match '/dev/iio' in ps.
for p in $(ps | grep '[c]at /tmp/'      | awk '{print $1}'); do kill -9 $p 2>/dev/null; done
sleep 1

# Dropping buffer/enable is what actually returns a process blocked inside the
# driver; SIGKILL cannot reach it there. Do it for BOTH directions.
echo 0 > "$R/buffer/enable" 2>/dev/null
echo 0 > "$IIO_TX/buffer/enable" 2>/dev/null
sleep 2

echo "readers left: $(ps | grep -c '[c]at /dev/iio')"
echo "feeders left: $(ps | grep -c '[c]at /tmp/')"
echo "rx buffer/enable: $(cat "$R/buffer/enable" 2>/dev/null)"
echo "tx buffer/enable: $(cat "$IIO_TX/buffer/enable" 2>/dev/null)"
echo "NOTE: re-run rx_framed.sh / tx_fabric.sh afterwards to re-enable the"
echo "      buffers. If reads still return 0, reload the PL -- the modem core is"
echo "      stalled and cannot see a soft reset."
