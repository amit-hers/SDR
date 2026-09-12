#!/usr/bin/env bash
# Two-radio RF acceptance: frame a known stream on one radio, receive it on the
# other, and report against the acceptance criteria. Run on the HOST.
#
#   scripts/rf_acceptance.sh <tx_ip> <rx_ip> [sample_rate] [--reboot]
#   e.g. scripts/rf_acceptance.sh 192.168.2.1 192.168.2.17 3840000 --reboot
#
# ACCEPTANCE: anchored packets > 0, decoded frames > 0, CRC failures 0,
# payload mismatches 0. Anything less is reported as FAIL with the numbers.
#
# Everything this does was learned the hard way and is load-bearing:
#
#  * Transfers go through iio_writedev / iio_readdev. A raw write()/read() on
#    /dev/iio:deviceN never programs the DMA on the 6.12.77 kernel -- it blocks
#    forever while every register reads healthy.
#  * The buffer must NOT be pre-enabled; libiio allocates it and otherwise
#    fails with "Unable to allocate buffer: Device or resource busy".
#  * The DMA source select is written AFTER the writer opens the buffer,
#    because opening it re-points the DAC channel at the internal DDS.
#  * kill -9 on a libiio helper leaves the buffer allocated and every later
#    open returns EBUSY until reboot -- hence --reboot for a clean run.
#  * TX2 is tied to zero in the bitstream and has no cable; keep it attenuated
#    so it is not radiating an unmodulated carrier into our own passband.
set -uo pipefail
TXIP="${1:?usage: rf_acceptance.sh <tx_ip> <rx_ip> [rate] [--reboot]}"
RXIP="${2:?usage: rf_acceptance.sh <tx_ip> <rx_ip> [rate] [--reboot]}"
FS="${3:-3840000}"
REBOOT=0; for a in "$@"; do [[ "$a" == "--reboot" ]] && REBOOT=1; done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PW="${PLUTO_PW:-analog}"
SSHO=(-o PubkeyAuthentication=no -o PreferredAuthentications=password
      -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=15)
sshd() { sshpass -p "$PW" ssh "${SSHO[@]}" "root@$1" "${@:2}"; }
push() { sshpass -p "$PW" ssh "${SSHO[@]}" "root@$1" "cat > $3" < "$2"; }

W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
FLT="$ROOT/build/framed_link_test"
[[ -x "$FLT" ]] || FLT="$W/framed_link_test"
if [[ ! -x "$FLT" ]]; then
  g++ -O2 -std=c++17 -I "$ROOT/include" -o "$W/framed_link_test" \
      "$ROOT/fpga/tools/framed_link_test.cpp" "$ROOT/build/src/core/libsdr_core.a" \
      -lliquid -lcrypto || { echo "FAIL: could not build framed_link_test"; exit 1; }
  FLT="$W/framed_link_test"
fi

echo "== generating reference frames =="
"$FLT" gen 1500 "$W/tx.iq" "$W/tx.bytes" >/dev/null || { echo "FAIL: gen"; exit 1; }
REFLEN=$(stat -c%s "$W/tx.bytes")
echo "   $REFLEN B of framed reference"

wait_up() {
  local ip=$1 n=0
  until sshd "$ip" true >/dev/null 2>&1; do
    sudo -n bash "$ROOT/scripts/host_pluto_net.sh" >/dev/null 2>&1 || true
    sleep 5; n=$((n+1)); (( n > 40 )) && { echo "FAIL: $ip did not come back"; exit 1; }
  done
}
if (( REBOOT )); then
  echo "== rebooting both radios for a clean buffer state =="
  for ip in "$TXIP" "$RXIP"; do
    printf '#!/bin/sh\nsync\nnohup sh -c "sleep 1; reboot -f" >/dev/null 2>&1 &\n' > "$W/rb.sh"
    push "$ip" "$W/rb.sh" /tmp/rb.sh 2>/dev/null
    sshd "$ip" 'sh /tmp/rb.sh' >/dev/null 2>&1 || true
  done
  sleep 15
  for ip in "$TXIP" "$RXIP"; do wait_up "$ip"; echo "   $ip back"; done
fi

# ── transmitter ───────────────────────────────────────────────────────────
cat > "$W/tx.sh" <<'TXS'
#!/bin/sh
D=/root/sdr-tools
. $D/iio_lookup.sh
P=$IIO_PHY; DB=0x79024000
sh $D/tx_fabric.sh "$1" 434000000 1 >/dev/null 2>&1
echo 1 > $IIO_TX/scan_elements/out_voltage0_en 2>/dev/null
echo 1 > $IIO_TX/scan_elements/out_voltage1_en 2>/dev/null
echo 0 > $IIO_TX/scan_elements/out_voltage2_en 2>/dev/null
echo 0 > $IIO_TX/scan_elements/out_voltage3_en 2>/dev/null
echo 0   > $P/out_voltage0_hardwaregain 2>/dev/null
echo -30 > $P/out_voltage1_hardwaregain 2>/dev/null
setsid sh -c "iio_writedev -b 32768 -c cf-ad9361-dds-core-lpc voltage0 voltage1 < /tmp/tx.bytes > /dev/null 2>/tmp/tx.err" < /dev/null &
sleep 4
for ch in 0 1; do devmem $((DB+0x418+64*ch)) 32 2; devmem $((DB+0x400+64*ch)) 32 0x00100001; done
sleep 1
echo "writers=$(ps w | grep -c '[i]io_writedev') dmac=$(devmem 0x7C420400 32) err=$(head -1 /tmp/tx.err 2>/dev/null)"
TXS

# ── receiver ──────────────────────────────────────────────────────────────
cat > "$W/rx.sh" <<'RXS'
#!/bin/sh
D=/root/sdr-tools
. $D/iio_lookup.sh
P=$IIO_PHY
sh $D/rx_framed.sh "$1" >/dev/null 2>&1
echo slow_attack > $P/in_voltage0_gain_control_mode 2>/dev/null
sleep 2
timeout 120 iio_readdev -b 8192 -s "$2" cf-ad9361-lpc voltage0 voltage1 > /tmp/cap.bin 2>/tmp/rx.err
echo "captured=$(wc -c < /tmp/cap.bin 2>/dev/null || echo 0) gain=$(cat $P/in_voltage0_hardwaregain) rssi=$(cat $P/in_voltage0_rssi) err=$(head -1 /tmp/rx.err 2>/dev/null)"
RXS

push "$TXIP" "$W/tx.bytes" /tmp/tx.bytes
push "$TXIP" "$W/tx.sh"    /tmp/tx.sh
push "$RXIP" "$W/rx.sh"    /tmp/rx.sh

echo "== $TXIP transmitting at $FS =="
sshd "$TXIP" "sh /tmp/tx.sh $FS" 2>/dev/null | sed 's/^/   /'
sleep 3
echo "== $RXIP receiving =="
sshd "$RXIP" "sh /tmp/rx.sh $FS 262144" 2>/dev/null | sed 's/^/   /'
sshd "$RXIP" 'cat /tmp/cap.bin' > "$W/cap.bin" 2>/dev/null
echo "   pulled $(stat -c%s "$W/cap.bin") B"

# Leave the transmitter quiet rather than radiating after the run.
sshd "$TXIP" 'D=/root/sdr-tools; . $D/iio_lookup.sh; echo -40 > $IIO_PHY/out_voltage0_hardwaregain' >/dev/null 2>&1 || true

echo
echo "== ACCEPTANCE =="
OUT=$("$FLT" per "$W/cap.bin" "$W/tx.bytes" "$REFLEN" 32768 2>&1)
echo "$OUT" | sed 's/^/   /'
ANCH=$(echo "$OUT" | grep -oE 'anchored [0-9]+' | awk '{print $2}'); ANCH=${ANCH:-0}
DEC=$(echo  "$OUT" | grep -oE 'decoded [0-9]+' | tail -1 | awk '{print $2}'); DEC=${DEC:-0}
CRC=$(echo  "$OUT" | grep -oE 'CRC failures [0-9]+' | awk '{print $3}'); CRC=${CRC:-1}
MIS=$(echo  "$OUT" | grep -oE 'payload mismatches [0-9]+' | awk '{print $3}'); MIS=${MIS:-1}
echo
printf '   anchored=%s decoded=%s crc_failures=%s payload_mismatches=%s\n' "$ANCH" "$DEC" "$CRC" "$MIS"
if (( ANCH > 0 && DEC > 0 && CRC == 0 && MIS == 0 )); then
  echo "   RESULT: PASS"; exit 0
fi
echo "   RESULT: FAIL"
(( ANCH == 0 )) && echo "   nothing anchored -- no frames arrived; check the RF level first"
exit 1
