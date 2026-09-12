#!/usr/bin/env bash
# Put each Pluto's USB link in its own network namespace. Run on the HOST.
#
#   scripts/pluto_netns.sh setup     create the namespaces and address them
#   scripts/pluto_netns.sh status    show what is reachable
#   scripts/pluto_netns.sh teardown  return the NICs to the root namespace
#   scripts/pluto_netns.sh exec A|B <cmd...>   run a command against one board
#
# WHY NAMESPACES AND NOT ADDRESSES: on the ADI 5.10 rootfs both boards present
# the SAME host-side MAC and the SAME IP. S23udc derives the gadget MAC from
#
#     serial=`dmesg | grep SPI-NOR-UniqueID`      # empty on this kernel
#     host_addr = 00:E0:22 + sha1(serial)[0:6]
#
# and with the serial empty both boards hash the same string, so both links
# carry 00:E0:22:AD:C8:3B and both boards answer on 192.168.2.1. Duplicate MACs
# in one L2 domain produce arbitrary and very misleading behaviour -- the ARP
# cache, the bridge table and the route all disagree about which link a frame
# belongs to, and a test can appear to talk to one board while reaching the
# other. Fixing it properly needs a per-board serial, which lives in a u-boot
# environment this rootfs CANNOT read (every fw_env.config offset tried returns
# "Bad CRC, using default environment"), so writing one risks an unbootable
# board.
#
# A namespace per link sidesteps all of it: the two addresses are never in the
# same L2 domain, so they cannot be confused, and nothing on the board changes.
set -uo pipefail
OUI="00:e0:22"
declare -A PORT=( [A]=1-8 [B]=1-6 )      # A = UNIT-A, B = UNIT-B, by USB port
HOSTIP=192.168.2.10
BOARDIP=192.168.2.1

nic_for_port() {
    local port=$1 n
    for n in /sys/class/net/*; do
        [[ -f "$n/address" ]] || continue
        [[ "$(cat "$n/address")" == "$OUI:"* ]] || continue
        [[ "$(readlink -f "$n")" == *"/$port/"* ]] && { basename "$n"; return 0; }
    done
    return 1
}

setup() {
    for k in A B; do
        local ns="pluto$k" nic
        nic=$(nic_for_port "${PORT[$k]}") || { echo "  $ns: no NIC on USB port ${PORT[$k]}"; continue; }
        sudo ip netns del "$ns" 2>/dev/null || true
        sudo ip netns add "$ns"
        sudo ip link set "$nic" netns "$ns"
        sudo ip netns exec "$ns" ip addr add "$HOSTIP/24" dev "$nic"
        sudo ip netns exec "$ns" ip link set "$nic" up
        sudo ip netns exec "$ns" ip link set lo up
        printf '  %-8s nic=%-18s host=%s board=%s\n' "$ns" "$nic" "$HOSTIP" "$BOARDIP"
    done
}

status() {
    for k in A B; do
        local ns="pluto$k"
        printf '  %-8s ' "$ns"
        if ! sudo ip netns list 2>/dev/null | grep -q "^$ns"; then echo "not created"; continue; fi
        if sudo ip netns exec "$ns" ping -c1 -W2 "$BOARDIP" >/dev/null 2>&1; then
            echo "up   ($(sudo ip netns exec "$ns" ip -br link show | awk '/00:e0:22/{print $1}'))"
        else
            echo "down ($(sudo ip netns exec "$ns" ip -br link show | awk '/00:e0:22/{print $1}' || echo 'no nic'))"
        fi
    done
}

teardown() {
    for k in A B; do
        local ns="pluto$k" nic
        sudo ip netns list 2>/dev/null | grep -q "^$ns" || continue
        nic=$(sudo ip netns exec "$ns" ip -br link show 2>/dev/null | awk '/00:e0:22/{print $1}')
        [[ -n "$nic" ]] && sudo ip netns exec "$ns" ip link set "$nic" netns 1 2>/dev/null || true
        sudo ip netns del "$ns" 2>/dev/null || true
        echo "  $ns removed${nic:+ (returned $nic)}"
    done
}

case "${1:-status}" in
  setup)    setup; echo "--- reachability ---"; status ;;
  status)   status ;;
  teardown) teardown ;;
  exec)     k=${2:?A or B}; shift 2; exec sudo ip netns exec "pluto$k" "$@" ;;
  *) echo "usage: $0 {setup|status|teardown|exec A|B <cmd...>}" >&2; exit 2 ;;
esac
