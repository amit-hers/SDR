#!/usr/bin/env bash
# Give each attached Pluto a reachable, unambiguous route. Run on the HOST.
#
# Both boards present a USB-ethernet gadget on 192.168.2.0/24, and the host ends
# up with the SAME address (192.168.2.10) on both NICs. Routing is then
# ambiguous: whichever /32 was added last works and the other board goes
# unreachable, so the two appear to flip-flop. Giving each NIC a distinct source
# address removes the ambiguity, and only then do the /32 routes hold.
#
# Re-run after ANY re-enumeration -- a reboot, a re-plug, or a board dropping
# off the bus flushes both the address and the route.
set -uo pipefail
declare -A WANT=( [PLUTOPLUS-UNIT-B]=192.168.2.1 )   # serial prefix -> board IP
SRC_BASE=10                                           # host gets .10, .11, ...

i=0
found=0
for d in /sys/bus/usb/devices/*/; do
    [[ -f "$d/idVendor" ]] || continue
    [[ "$(cat "$d/idVendor")" == "0456" ]] || continue
    serial=$(cat "$d/serial" 2>/dev/null)
    port=$(basename "$d")
    nic=""
    for n in /sys/class/net/*; do
        [[ "$(readlink -f "$n")" == *"/$port/"* ]] && nic=$(basename "$n")
    done
    [[ -n "$nic" ]] || { echo "  $port ($serial): no NIC yet"; continue; }

    # The board's own address comes from its config, so read it rather than
    # guess: the labels on these units do not match their addresses.
    board=""
    for cand in 192.168.2.1 192.168.2.17; do
        ping -c1 -W1 -I "$nic" "$cand" >/dev/null 2>&1 && { board="$cand"; break; }
    done
    [[ -n "$board" ]] || { echo "  $nic ($serial): no board answered on this link"; continue; }

    src="192.168.2.$((SRC_BASE + i))"
    sudo ip addr flush dev "$nic" 2>/dev/null
    sudo ip addr add "$src/24" dev "$nic" 2>/dev/null
    sudo ip link set "$nic" up
    sudo ip route replace "$board/32" dev "$nic" src "$src" metric 50
    printf '  %-18s %-28s board=%-14s host=%s\n' "$nic" "$serial" "$board" "$src"
    i=$((i+1)); found=1
done
[[ $found -eq 1 ]] || { echo "  no Pluto found"; exit 1; }
echo "--- reachability ---"
for ip in 192.168.2.1 192.168.2.17; do
    printf '  %-14s ' "$ip"
    ping -c1 -W2 "$ip" >/dev/null 2>&1 && echo up || echo down
done
