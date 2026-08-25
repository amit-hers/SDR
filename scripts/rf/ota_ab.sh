#!/bin/bash
# Over-the-air A/B: FixedTimingSync (default) vs liquid symsync_crcf.
#
# Everything except the timing implementation is held identical -- power,
# bandwidth, modulation, payload size, offered load -- and the variants
# alternate so slow drift in the RF environment hits both equally.
#
# Run AFTER swapping the coax for antennas. The coax setup used A_ATTEN=25
# through a 40 dB pad (RSSI ~ -58 dBm); over the air that pad is gone, so the
# transmit level must be re-tuned or the receiver will be starved.
#
#   ./ota_ab.sh tune      find a workable A_ATTEN over the air
#   ./ota_ab.sh ab [n]    run the matched A/B, n trials (default 2)
set -u
SP=/home/amither/Documents/SDR/scripts/rf
PW=proview
MODE="${1:-ab}"; TRIALS="${2:-2}"

row() {  # label  extra-env...
  local lbl="$1"; shift
  echo "$PW" | sudo -S rm -f /tmp/sdr_stats_A.json /tmp/sdr_stats_B.json 2>/dev/null
  local out
  out=$(env "$@" MOD=QPSK BW=2 BWF=1.35 BLAST_RATE=200 \
        timeout 260 "$SP/datalink.sh" 25 2>&1)
  local b a gp
  b=$(echo "$out" | grep 'node B:')
  a=$(echo "$out" | grep 'node A:' | grep -oE 'tx=[0-9]+')
  gp=$(echo "$out" | grep -oE '[0-9.]+ kbps' | head -1)
  local good bad bursts demod
  good=$(echo "$b" | grep -oE 'rx_good=[0-9]+' | cut -d= -f2)
  bad=$(echo  "$b" | grep -oE 'rx_bad=[0-9]+'  | cut -d= -f2)
  bursts=$(echo "$b" | grep -oE 'bursts=[0-9]+' | cut -d= -f2)
  demod=$(echo "$b" | grep -oE 'demod=[0-9]+'  | cut -d= -f2)
  local crc acq
  crc=$(python3 -c "g=${good:-0};b=${bad:-0};print(f'{100*g/(g+b):.1f}%' if g+b else 'n/a')")
  acq=$(python3 -c "d=${demod:-0};b=${bursts:-0};print(f'{100*d/b:.1f}%' if b else 'n/a')")
  printf "  %-22s %-9s good=%-6s bad=%-5s CRC=%-7s acq=%-7s %s\n" \
         "$lbl" "$a" "${good:-?}" "${bad:-?}" "$crc" "$acq" "$gp"
}

case "$MODE" in
  tune)
    echo "=== OTA transmit-level tune (antennas connected) ==="
    for at in 0 5 10 20; do row "A_ATTEN=$at" A_ATTEN=$at; done
    ;;
  ab)
    echo "=== OTA A/B: FixedTimingSync vs liquid (A_ATTEN=${A_ATTEN:-10}) ==="
    for t in $(seq "$TRIALS"); do
      row "t$t fixed"  A_ATTEN="${A_ATTEN:-10}" SDR_TSYNC=fixed
      row "t$t liquid" A_ATTEN="${A_ATTEN:-10}" SDR_TSYNC=liquid
    done
    ;;
esac
