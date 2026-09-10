#!/usr/bin/env bash
# Give each attached Pluto a stable, unambiguous host route. Run on the HOST,
# and re-run after a re-plug or a --persist flash.
#
# THE PROBLEM, precisely: every Pluto runs its own DHCP server and hands out
# 192.168.2.10. NetworkManager DHCPs on both USB NICs and is given the SAME
# address on both, so the host holds 192.168.2.10 twice and the 192.168.2.0/24
# route is ambiguous -- whichever NIC was configured last wins and the other
# board goes silently unreachable. NM re-runs DHCP on every re-plug and lease
# renewal, so a static address applied by hand is undone minutes later. This is
# why boards appear to "flip-flop" and why one can look like it dropped off the
# bus while it is enumerated and merely unrouted.
#
# WHY THIS PROBES RATHER THAN MATCHING AN IDENTIFIER: a --persist flash
# regenerates the USB gadget's MAC *and* its serial. UNIT-B went from
# 00:e0:22:6d:c9:b7 / PLUTOPLUS-UNIT-B-1786295009 to 00:e0:22:3e:18:23 /
# 3SXRLJMXS7EL5IBJ across one flash, which silently broke a MAC-bound profile.
# The only durable facts are the Analog Devices OUI (00:e0:22) and the address
# the board answers on, so this finds NICs by OUI and asks each link who is
# there.
#
# THE FIX: a /32 host address per NIC creates no subnet route, so the ambiguous
# /24 never exists; reachability comes from an explicit per-board /32 route on
# the correct device.
set -uo pipefail

OUI="00:e0:22"                       # Analog Devices; survives firmware changes
CANDIDATES=(192.168.2.1 192.168.2.17)
SRC_BASE=10                          # host takes .10, .11, ...

# Stop NetworkManager from DHCPing these links out from under us. Without this
# the addresses set below are flushed at the next lease renewal.
NMCONF=/etc/NetworkManager/conf.d/90-pluto-unmanaged.conf
if [[ "${1:-}" == "--install" ]]; then
    printf '[keyfile]\nunmanaged-devices=mac:%s*\n' "$OUI" | sudo tee "$NMCONF" >/dev/null
    sudo systemctl reload NetworkManager 2>/dev/null || sudo systemctl restart NetworkManager
    echo "  installed $NMCONF (NetworkManager will not manage $OUI:* links)"
    sleep 3
fi

declare -A CLAIMED=()
i=0; found=0
for n in /sys/class/net/*; do
    [[ -f "$n/address" ]] || continue
    mac=$(cat "$n/address")
    [[ "$mac" == "$OUI:"* ]] || continue
    nic=$(basename "$n")

    src="192.168.2.$((SRC_BASE + i))"
    sudo ip addr flush dev "$nic" 2>/dev/null
    sudo ip addr add "$src/32" dev "$nic" 2>/dev/null
    sudo ip link set "$nic" up

    # Ask the link who is there. The labels on these units do not match their
    # addresses, and a flash can reset one to the stock address, so identity
    # must be discovered rather than assumed.
    board=""
    for cand in "${CANDIDATES[@]}"; do
        # Skip an address another link already claimed. Without this the second
        # NIC "discovers" a board that is really answering through the FIRST --
        # both links then hold a route to the same address, the kernel picks
        # one, and the other radio is reported present while being unreachable.
        [[ -n "${CLAIMED[$cand]:-}" ]] && continue
        sudo ip route replace "$cand/32" dev "$nic" src "$src" 2>/dev/null
        if ping -c1 -W1 -I "$nic" "$cand" >/dev/null 2>&1; then
            board="$cand"; CLAIMED[$cand]="$nic"; break
        fi
        sudo ip route del "$cand/32" dev "$nic" 2>/dev/null
    done
    if [[ -z "$board" ]]; then
        printf '  %-18s mac=%s  no board answered on this link\n' "$nic" "$mac"
        continue
    fi
    printf '  %-18s mac=%s  host=%-13s board=%s\n' "$nic" "$mac" "$src/32" "$board"
    i=$((i+1)); found=1
done
[[ $found -ge 1 ]] || { echo "  no Pluto NIC found (looked for OUI $OUI)"; exit 1; }

# Reachability, checked HONESTLY. A bare ping is not evidence: with no /32 route
# the packet leaves by the DEFAULT route and something else on the LAN answers,
# so a board that is not plugged in reports "up". Observed exactly that --
# 192.168.2.1 answering at 3.6 ms over wifi while the board was off the USB bus.
echo "--- reachability ---"
for ip_b in "${CANDIDATES[@]}"; do
    printf '  %-14s ' "$ip_b"
    via=$(ip route get "$ip_b" 2>/dev/null | head -1 | grep -o 'dev [^ ]*' | awk '{print $2}')
    if [[ -z "$via" ]] || [[ "$(cat /sys/class/net/$via/address 2>/dev/null)" != "$OUI:"* ]]; then
        echo "NOT ATTACHED (no Pluto link routes to it; would exit via ${via:-none})"
        continue
    fi
    ping -c1 -W3 -I "$via" "$ip_b" >/dev/null 2>&1 && echo "up (via $via)" || echo "down (on $via, no reply)"
done
