#!/bin/bash
# Resolve a Pluto's gigabit (eth0) address by serial.
#
# The radios use avahi link-local autoip, so their addresses change on reboot
# and cannot be hard-coded. Reaching them also needs a route to 169.254.0.0/16
# on the interface their RJ45 ports are plugged into; NetworkManager flushes a
# manually-added route, so the durable fix is an address in the NM profile:
#   nmcli con mod <profile> +ipv4.addresses 169.254.1.1/16
set -u
WANT="${1:?usage: plutoip.sh A|B}"
ip route | grep -q '169\.254\.0\.0/16' || {
  echo "plutoip: no route to 169.254.0.0/16 -- radios unreachable" >&2; exit 1; }
for attempt in 1 2 3 4 5; do
  line=$(timeout 45 iio_info -s 2>/dev/null | grep "PLUTOPLUS-UNIT-$WANT" | grep -oE '169\.254\.[0-9]+\.[0-9]+' | head -1)
  [ -n "$line" ] && { echo "$line"; exit 0; }
  sleep 5
done
echo "plutoip: unit $WANT not found on the network" >&2
exit 1
