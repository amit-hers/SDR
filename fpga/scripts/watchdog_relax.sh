#!/bin/sh
# Widen the watchdog timeout for long-running instrumentation. Run ON the Pluto.
#
# Stock is `watchdog -t 5 -T 10 /dev/watchdog`: a TEN SECOND hardware timeout.
# Any test that hammers the CPU for longer than that resets the board -- and the
# probe readbacks do exactly that, because reading N samples costs 2N `devmem`
# forks (2048 samples = 4096 processes, about 19 s). The board then reboots,
# and because the rootfs is a RAMDISK the bitstream, payloads and scripts in
# /lib/firmware and /tmp all vanish. The symptom is bus errors on 0x43Cxxxxx
# and a test that silently measures nothing.
#
# Verified: with -T 120 a 29 s fork burst survives; under -T 10 it would not.
#
# The ramdisk means this does NOT persist -- re-run after every boot. Check with
#   ps | grep "[w]atchdog -t"
# and confirm REBOOT_STATUS at 0xF8000258 after any unexplained restart.
T=${1:-120}
OLD=$(pidof watchdog)
[ -n "$OLD" ] && kill $OLD 2>/dev/null
sleep 1
watchdog -t 5 -T "$T" /dev/watchdog
sleep 1
echo "watchdog: $(ps | grep '[w]atchdog -t' | sed 's/^ *//')"
