#!/bin/bash
# boards.sh -- enumerate the attached Pluto+ units and report what each one is,
# how to reach it, and WHICH BITSTREAM it is running.
#
# Two facts cost hours each and are why this exists.
#
# THE NAMES ON THE BOARDS DO NOT MATCH THE ADDRESSES. The unit whose USB serial
# says UNIT-B answers on 192.168.2.1; UNIT-A answers on 192.168.2.17. The USB
# serial is authoritative -- read it, do not infer the unit from its IP or from
# which NIC it appeared on.
#
# BOTH HOST NICs SIT IN 192.168.2.0/24 WITH THE SAME SOURCE ADDRESS. The kernel
# therefore sends every 192.168.2.x packet out whichever connected route has the
# lower metric, so exactly one board is reachable and the other looks dead. It
# is not dead: `ping -I <iface>` reaches it fine. The fix is a /32 route per
# board, which needs root; this script prints the command rather than running it.
#
# A board that is up but running the STOCK bitstream bus-errors on every
# 0x43Cxxxxx access. That reads like a dead peripheral and is nothing of the
# kind, so the PL is identified explicitly below before any register work.
set -u
SSHO="-o PubkeyAuthentication=no -o PreferredAuthentications=password
      -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5"
PW="${PLUTO_PW:-root}"

# The modem design's AXI-Lite windows. Stock has none of them.
WINDOWS="0x43C00000 0x43C10000 0x43C20000 0x43C30000 0x43C40000"

printf '%-26s %-5s %-17s %-9s %-9s %-14s %s\n' \
       SERIAL PORT NIC TTY VOLUME IP STATUS
need_route=()

for dev in /sys/bus/usb/devices/*; do
  [ -f "$dev/idVendor" ] || continue
  [ "$(cat "$dev/idVendor")" = "0456" ] || continue
  [ "$(cat "$dev/idProduct" 2>/dev/null)" = "b673" ] || continue
  port=$(basename "$dev")
  serial=$(cat "$dev/serial" 2>/dev/null)

  nic=$(for n in /sys/class/net/*; do
          case "$(readlink -f "$n")" in *"/$port/"*) basename "$n"; break;; esac
        done)
  tty=$(for t in /sys/class/tty/ttyACM*; do
          [ -e "$t" ] || continue
          case "$(readlink -f "$t")" in *"/$port/"*) basename "$t"; break;; esac
        done)
  vol=$(for m in /media/*/PlutoSDR*; do
          [ -f "$m/config.txt" ] || continue
          src=$(findmnt -no SOURCE "$m" 2>/dev/null); blk=$(basename "${src%%[0-9]}")
          case "$(readlink -f "/sys/block/$blk" 2>/dev/null)" in
            *"/$port/"*) echo "$m"; break;; esac
        done)
  # config.txt lives on a FAT volume and has CRLF line endings. Without the
  # tr the address carries a trailing CR, every ping fails, and both boards
  # report unreachable while answering perfectly from the shell.
  ip=$([ -n "$vol" ] && sed -n 's/^ *ipaddr *= *//p' "$vol/config.txt" | head -1 | tr -d '\r')
  ip=${ip:-unknown}

  # Reachable by the normal routing table?
  status="unreachable"
  if [ "$ip" != unknown ]; then
    if timeout 4 ping -c1 -W2 "$ip" >/dev/null 2>&1; then
      status="up"
    elif [ -n "$nic" ] && timeout 4 ping -c1 -W2 -I "$nic" "$ip" >/dev/null 2>&1; then
      status="NEEDS-ROUTE"      # alive, but the route sends us out the wrong NIC
      need_route+=("$ip $nic")
    fi
  fi

  # Which bitstream? Only ask a board we can actually log into.
  if [ "$status" = up ]; then
    hits=$(timeout 15 sshpass -p "$PW" ssh $SSHO "root@$ip" \
             "n=0; for a in $WINDOWS; do devmem \$a 32 >/dev/null 2>&1 && n=\$((n+1)); done; echo \$n" \
             2>/dev/null | tr -dc '0-9')
    case "${hits:-}" in
      ""|0) status="up/STOCK-PL" ;;
      *)    status="up/modem-PL(${hits}/5)" ;;
    esac
  fi

  printf '%-26s %-5s %-17s %-9s %-9s %-14s %s\n' \
         "${serial:-?}" "$port" "${nic:-–}" "${tty:-–}" \
         "$([ -n "$vol" ] && basename "$vol" || echo –)" "$ip" "$status"
done

if [ ${#need_route[@]} -gt 0 ]; then
  echo
  echo "Some boards are ALIVE but not routable. Run (needs root, survives until reboot):"
  for e in "${need_route[@]}"; do
    set -- $e
    echo "    sudo ip route replace $1/32 dev $2 src 192.168.2.10 metric 50"
  done
  echo "To make it persistent instead:"
  for e in "${need_route[@]}"; do
    set -- $e
    echo "    nmcli connection modify \"\$(nmcli -g GENERAL.CONNECTION device show $2 | head -1)\" +ipv4.routes \"$1/32\""
  done
fi
