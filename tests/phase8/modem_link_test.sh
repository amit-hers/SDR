#!/usr/bin/env bash
# Minimal two-unit MODEM test: one board's modulator -> the other's demodulator.
# No Ethernet, no bridge, no RJ45. Proves sync and byte recovery, or does not.
#
# Runs BEFORE any Phase 8 Ethernet work, because a transparent-Ethernet result
# means nothing if the modem underneath it has not been shown to carry bytes.
#
# Every modem setting is WRITTEN and then READ BACK from both boards and
# compared. A silent mismatch is the failure mode this exists to catch: with
# mod diff_mode=0 against dem diff_mode=1 the signal level looks perfect, every
# register reads healthy, symbol statistics look balanced, and not one frame
# decodes. That combination cost a full investigation.
#
#   sudo -E tests/phase8/modem_link_test.sh [--reverse]
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

A_IP=${A_IP:-192.168.2.17}
B_IP=${B_IP:-192.168.2.1}
FS=${FS:-7680000}
LO=${LO:-434000000}
DIFF=${DIFF:-1}
TXATT=${TXATT:--20}
REF=${REF:-/tmp/tx.bytes}          # on the BOARD
REF_HOST=${REF_HOST:-/tmp/tx.bytes}  # the same file on this host, for analysis          # 50800 B: 40 numbered frames from framed_link_test gen

SSH=(sshpass -p analog ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10)
on() { local h=$1; shift; timeout 180 "${SSH[@]}" "root@$h" "$*" </dev/null 2>/dev/null; }

# ---- positively identify both boards -------------------------------------
identify() {
    local h=$1 name=$2
    local ser kern fpga abi rmv
    # The serial must come from the HOST's USB enumeration. Asking the board
    # reads its own (empty) USB device tree -- it is a gadget, not a host -- so
    # that path silently identifies every board as "".
    local dev port
    dev=$(ip route get "$h" 2>/dev/null | sed -n 's/.* dev \([^ ]*\).*/\1/p' | head -1)
    port=$(readlink -f "/sys/class/net/$dev/device" 2>/dev/null | grep -oE '[0-9]+-[0-9]+' | head -1)
    ser=$(cat "/sys/bus/usb/devices/$port/serial" 2>/dev/null)
    kern=$(on "$h" 'uname -r')
    fpga=$(on "$h" 'devmem 0x43C50000 32')
    abi=$(on "$h"  'devmem 0x43C50008 32')
    rmv=$(on "$h"  'devmem 0x43C5000C 32')
    printf '  %-7s ip=%-14s iface=%s port=%s serial=%s\n' "$name" "$h" "${dev:-?}" "${port:-?}" "${ser:-EMPTY}"
    printf '          kernel=%s\n' "${kern:-UNREACHABLE}"
    printf '          fpga_magic=%s abi=%s reg_map=%s\n' "$fpga" "$abi" "$rmv"
    [[ -n "$kern" ]] || { echo "  $name is not reachable"; return 1; }
    [[ "$fpga" == "0x5344524C" ]] || { echo "  $name has the wrong bitstream ($fpga)"; return 1; }
    return 0
}

# ---- configure, then read back and compare -------------------------------
settings_of() {
    local h=$1
    on "$h" '
      P=/sys/bus/iio/devices/iio:device0
      echo "fs_rx=$(cat $P/in_voltage_sampling_frequency)"
      echo "fs_tx=$(cat $P/out_voltage_sampling_frequency)"
      echo "lo_tx=$(cat $P/out_altvoltage1_TX_LO_frequency)"
      echo "lo_rx=$(cat $P/out_altvoltage0_RX_LO_frequency)"
      echo "bw_rx=$(cat $P/in_voltage_rf_bandwidth)"
      echo "mod_en=$(devmem 0x43C10010 32)"
      echo "mod_bpsk=$(devmem 0x43C10018 32)"
      echo "mod_diff=$(devmem 0x43C10020 32)"
      echo "dem_en=$(devmem 0x43C00010 32)"
      echo "dem_diff=$(devmem 0x43C00028 32)"
      echo "adc_fmt0=$(devmem 0x79020400 32)"
      echo "adc_fmt1=$(devmem 0x79020440 32)"'
}

configure() {
    local h=$1
    on "$h" "
      me=\$\$
      for d in /proc/[0-9]*; do p=\${d#/proc/}; [ \"\$p\" = \"\$me\" ] && continue
        e=\$(readlink \"\$d/exe\" 2>/dev/null)||continue
        case \"\$e\" in */sdr_bridge*|*/iio_readdev*|*/iio_writedev*|*/dd) kill -9 \"\$p\" 2>/dev/null;; esac
      done
      sleep 2
      sh /mnt/jffs2/tools/watchdog_relax.sh >/dev/null 2>&1
      sh /mnt/jffs2/tools/tx_fabric.sh $FS $LO $DIFF >/dev/null 2>&1
      sh /mnt/jffs2/tools/rx_framed.sh $FS $DIFF     >/dev/null 2>&1
      devmem 0x43C10020 32 $DIFF
      devmem 0x43C00028 32 $DIFF
      echo $TXATT > /sys/bus/iio/devices/iio:device0/out_voltage0_hardwaregain"
}

run_direction() {
    local tx=$1 rx=$2 txname=$3 rxname=$4
    echo
    echo "=== $txname TX  ->  $rxname RX ==="
    configure "$tx"; configure "$rx"

    # Read back BOTH boards and compare the settings that must agree.
    local -A ta ra
    while IFS='=' read -r k v; do [[ -n $k ]] && ta[$k]=$v; done < <(settings_of "$tx")
    while IFS='=' read -r k v; do [[ -n $k ]] && ra[$k]=$v; done < <(settings_of "$rx")
    local bad=0
    echo "  setting          $txname(tx)          $rxname(rx)"
    for k in fs_rx fs_tx lo_tx lo_rx bw_rx mod_diff dem_diff adc_fmt0 adc_fmt1 mod_bpsk; do
        local mark=" "
        [[ "${ta[$k]:-?}" != "${ra[$k]:-?}" ]] && { mark="<-- MISMATCH"; bad=1; }
        printf '  %-16s %-20s %-20s %s\n' "$k" "${ta[$k]:-?}" "${ra[$k]:-?}" "$mark"
    done
    # The pairing that actually carries the link: transmitter's mod_diff must
    # equal the receiver's dem_diff.
    if [[ "${ta[mod_diff]:-x}" != "${ra[dem_diff]:-y}" ]]; then
        echo "  CRITICAL: tx mod_diff=${ta[mod_diff]:-?} != rx dem_diff=${ra[dem_diff]:-?}"
        bad=1
    fi
    (( bad )) && { echo "  ABORT: settings do not match; fix before interpreting any result"; return 1; }
    echo "  settings verified identical on both ends"

    # Feed on TX, capture on RX. The feed must outlast the capture, or the
    # capture reads silence and the statistics lie.
    on "$tx" "rm -f /tmp/big.bytes; i=0; while [ \$i -lt 400 ]; do cat $REF >> /tmp/big.bytes; i=\$((i+1)); done"
    on "$tx" "setsid sh /mnt/jffs2/tools/tx_feed.sh /tmp/big.bytes 32768 >/dev/null 2>&1 </dev/null & sleep 1; echo started" >/dev/null
    sleep 5
    local rssi feed
    rssi=$(on "$rx" 'cat /sys/bus/iio/devices/iio:device0/in_voltage0_rssi')
    feed=$(on "$tx" "ps | grep -c '[i]io_writedev'")
    echo "  during: rx rssi=$rssi  tx feed alive=$feed"
    on "$rx" 'rm -f /tmp/lnk.bin; timeout 10 iio_readdev -b 8192 cf-ad9361-lpc voltage0 voltage1 > /tmp/lnk.bin 2>/dev/null; echo done' >/dev/null
    local still
    still=$(on "$tx" "ps | grep -c '[i]io_writedev'")
    on "$tx" "for p in \$(ps | grep '[i]io_writedev' | awk '{print \$1}'); do kill -9 \$p 2>/dev/null; done"
    echo "  after : tx feed alive=$still (must be 1, or the capture outran the feed)"

    on "$rx" 'cat /tmp/lnk.bin' > "/tmp/link_${txname}_to_${rxname}.bin" 2>/dev/null
    python3 - "/tmp/link_${txname}_to_${rxname}.bin" <<'PY'
import sys
SYNC=bytes([0xC0,0xFF,0xEE,0x77]); ISY=bytes(x^0xFF for x in SYNC)
def bs(b,n): return b if n==0 else bytes(((b[i]<<n)|(b[i+1]>>(8-n)))&0xFF for i in range(len(b)-1))
d=open(sys.argv[1],'rb').read()
pp=[bs(d,n).count(SYNC)+bs(d,n).count(ISY) for n in (0,2,4,6)]
print(f"  captured {len(d)} B; sync per phase 0/2/4/6 = {pp}  TOTAL {sum(pp)}")
print("  SYNC: " + ("FOUND" if sum(pp) else "NONE -- no frame recovery"))
PY
    local flt="$ROOT/build/framed_link_test"
    [[ -x /tmp/flt ]] && flt=/tmp/flt
    if [[ -x "$flt" ]]; then
        echo "  --- frame recovery ---"
        "$flt" per "/tmp/link_${txname}_to_${rxname}.bin" "$REF_HOST" 50800 32768 2>/dev/null \
          | tail -6 | sed 's/^/  /' || true
    fi
}

echo "=== positive identification ==="
identify "$A_IP" "UNIT-A" || exit 1
identify "$B_IP" "UNIT-B" || { echo; echo "UNIT-B not usable. Check it is plugged in and that"; echo "$B_IP routes over its own USB NIC, not via the internet."; exit 1; }

if [[ "${1:-}" == "--reverse" ]]; then
    run_direction "$B_IP" "$A_IP" "UNIT-B" "UNIT-A"
else
    run_direction "$A_IP" "$B_IP" "UNIT-A" "UNIT-B" && {
        echo; echo "A->B done; repeating in reverse as required"
        run_direction "$B_IP" "$A_IP" "UNIT-B" "UNIT-A"
    }
fi
