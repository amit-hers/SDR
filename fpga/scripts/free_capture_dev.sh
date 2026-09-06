#!/bin/sh
# Release /dev/iio:device4 after an aborted capture. Run ON the Pluto.
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
R=/sys/bus/iio/devices/iio:device4

for p in $(ps | grep '[a]cqframes.sh' | awk '{print $1}'); do kill -9 $p 2>/dev/null; done
for p in $(ps | grep '[r]xframes.sh'  | awk '{print $1}'); do kill -9 $p 2>/dev/null; done
sleep 1
for p in $(ps | grep '[c]at /dev/iio'   | awk '{print $1}'); do kill -9 $p 2>/dev/null; done
for p in $(ps | grep '[d]d if=/dev/iio' | awk '{print $1}'); do kill -9 $p 2>/dev/null; done
sleep 1

# Unblock anything still inside the driver.
if [ "$(ps | grep -c '[c]at /dev/iio')" != "0" ]; then
  echo 0 > $R/buffer/enable 2>/dev/null
  sleep 2
fi

echo "readers left: $(ps | grep -c '[c]at /dev/iio')"
echo "buffer/enable: $(cat $R/buffer/enable 2>/dev/null)"
echo "NOTE: if buffer/enable is 0, re-run rx_framed.sh; if reads still return 0,"
echo "      reload the PL -- the modem core is stalled and cannot see a reset."
