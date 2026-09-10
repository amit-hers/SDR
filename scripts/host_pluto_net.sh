#!/usr/bin/env bash
# Give each attached Pluto a PERSISTENT, unambiguous host route. Run once, on
# the HOST. Replaces re-running pluto_routes.sh after every re-enumeration.
#
# THE PROBLEM, precisely: every Pluto runs its own DHCP server and hands out
# 192.168.2.10. NetworkManager DHCPs on both USB NICs and is given the SAME
# address on both, so the host holds 192.168.2.10 twice and the 192.168.2.0/24
# route is ambiguous -- whichever NIC was configured last wins and the other
# board silently goes unreachable. NM re-runs DHCP on every re-plug, reboot and
# lease renewal, so any static fix applied by hand is undone minutes later. That
# is why the boards appear to "flip-flop" and why a board can look like it
# dropped off the bus when it is merely unrouted.
#
# THE FIX: bind a static profile to each NIC's MAC, and give the host a **/32**
# address. A /32 creates no subnet route, so the ambiguous /24 never exists;
# reachability comes from an explicit per-board /32 route on the right device.
# Profiles are keyed to the MAC, so they survive re-enumeration and host
# reboots without anything being re-run.
#
# Undo with:  nmcli con delete pluto-unit-a pluto-unit-b
set -uo pipefail

# NIC MAC -> (host address, board address, profile name)
declare -A HOST=( [00:e0:22:3d:32:ae]=192.168.2.11 [00:e0:22:6d:c9:b7]=192.168.2.10 )
declare -A BOARD=([00:e0:22:3d:32:ae]=192.168.2.17 [00:e0:22:6d:c9:b7]=192.168.2.1  )
declare -A NAME=( [00:e0:22:3d:32:ae]=pluto-unit-a [00:e0:22:6d:c9:b7]=pluto-unit-b )

command -v nmcli >/dev/null || { echo "nmcli not found"; exit 1; }

for mac in "${!HOST[@]}"; do
    nic=""
    for n in /sys/class/net/*; do
        [[ -f "$n/address" ]] || continue
        [[ "$(cat "$n/address")" == "$mac" ]] && nic=$(basename "$n")
    done
    if [[ -z "$nic" ]]; then
        echo "  ${NAME[$mac]}: no NIC with MAC $mac attached; profile still (re)created"
    fi
    con="${NAME[$mac]}"
    sudo nmcli con delete "$con" >/dev/null 2>&1 || true
    # mac-based binding, not ifname: the interface name is derived from the MAC
    # here, but binding to the MAC is what makes this correct if that changes.
    sudo nmcli con add type ethernet con-name "$con" \
        802-3-ethernet.mac-address "$mac" \
        ipv4.method manual \
        ipv4.addresses "${HOST[$mac]}/32" \
        ipv4.routes "${BOARD[$mac]}/32" \
        ipv4.never-default yes \
        ipv6.method disabled \
        connection.autoconnect yes \
        connection.autoconnect-priority 10 >/dev/null || { echo "  FAILED: $con"; continue; }
    [[ -n "$nic" ]] && sudo nmcli con up "$con" >/dev/null 2>&1
    printf '  %-14s mac=%s host=%-13s board=%-13s nic=%s\n' \
           "$con" "$mac" "${HOST[$mac]}/32" "${BOARD[$mac]}/32" "${nic:-absent}"
done

# Reachability, checked HONESTLY. A bare ping is not evidence: with no /32
# route the packet leaves by the DEFAULT route and something else on the LAN
# answers, so a board that is not even plugged in reports "up". Observed
# exactly that here -- 192.168.2.1 answering at 3.6 ms over wifi while the
# board was off the USB bus entirely. Confirm the route resolves to the
# expected interface FIRST, then ping pinned to it.
echo "--- reachability ---"
for mac in "${!HOST[@]}"; do
    ip_b="${BOARD[$mac]}"
    nic=""
    for n in /sys/class/net/*; do
        [[ -f "$n/address" ]] || continue
        [[ "$(cat "$n/address")" == "$mac" ]] && nic=$(basename "$n")
    done
    printf '  %-14s ' "$ip_b"
    if [[ -z "$nic" ]]; then
        echo "NOT ATTACHED (no NIC with MAC $mac)"
        continue
    fi
    via=$(ip route get "$ip_b" 2>/dev/null | head -1 | grep -o 'dev [^ ]*' | awk '{print $2}')
    if [[ "$via" != "$nic" ]]; then
        echo "MISROUTED (goes via ${via:-none}, not $nic)"
        continue
    fi
    if ping -c1 -W3 -I "$nic" "$ip_b" >/dev/null 2>&1; then
        echo "up (via $nic)"
    else
        echo "down (attached on $nic, no reply)"
    fi
done
echo "--- routes ---"
ip route | grep -E "192\.168\.2\." | sed 's/^/  /'
