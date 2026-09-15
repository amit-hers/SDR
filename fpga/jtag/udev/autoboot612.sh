#!/usr/bin/env bash
# Triggered by udev when a Pluto enumerates: boot 6.12 into it over JTAG.
#
# A replug is exactly the state boot612.sh needs -- a freshly power-cycled board
# running its flash kernel -- so hotplug is the right moment to do this.
#
# udev kills anything still running when the event handler returns, so the rule
# detaches us; this script must not be run from udev directly.
set -uo pipefail
CONF=/etc/pluto-autoboot612.conf
LOG=/var/log/pluto-autoboot612.log
LOCK=/run/pluto-autoboot612.lock

log() { printf '%s %s\n' "$(date -Is)" "$*" >> "$LOG" 2>/dev/null; }

[[ -f "$CONF" ]] || { log "no $CONF; nothing to do"; exit 0; }
# shellcheck source=/dev/null
. "$CONF"
: "${SDR_ROOT:?}" "${SDR_SCRATCH:?}"

# ENABLED is a separate file, not a variable, so arming and disarming is one
# command and survives reinstalls. Absent means do nothing: a udev rule that
# reprograms a radio the moment it is plugged in should never be on by default.
[[ -f /etc/pluto-autoboot612.enabled ]] || { log "disarmed (no /etc/pluto-autoboot612.enabled)"; exit 0; }

# One at a time. Two boards, or udev firing per-interface, would otherwise run
# concurrent openocd sessions against the same cable.
exec 9>"$LOCK"
flock -n 9 || { log "another run in progress; skipping"; exit 0; }

# Let the gadget settle, then re-check that this board still needs booting: a
# board already running 6.12 reports a NON-EMPTY USB serial (it derives one from
# the SPI-NOR UniqueID, which 5.10 never exposes). Without this the successful
# boot re-enumerates, retriggers the rule and loops forever.
sleep 6
for d in /sys/bus/usb/devices/1-*; do
  [[ -f "$d/idVendor" ]] || continue
  [[ "$(cat "$d/idVendor" 2>/dev/null)" == "0456" ]] || continue
  s=$(cat "$d/serial" 2>/dev/null || true)
  if [[ -n "$s" ]]; then
    log "board at $(basename "$d") already reports serial '$s' -- looks like 6.12, not reboot(ing)"
    exit 0
  fi
done

log "booting 6.12 over JTAG"
SDR_SCRATCH="$SDR_SCRATCH" "$SDR_ROOT/fpga/jtag/boot612.sh" 1 >> "$LOG" 2>&1
rc=$?
log "boot612.sh exit=$rc"
exit $rc
