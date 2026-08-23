#!/bin/bash
# Alternating A/B of the timing-recovery implementations.
#
# Alternates variants so slow drift in the RF environment hits both equally,
# and refuses any run whose stats files are stale or whose offered load
# (A_tx) differs materially between variants -- a transmitter that sent less
# traffic invalidates every RX metric downstream of it.
#
#   ./ab.sh [trials] [secs]     env: MOD BW BWF BLAST_RATE A_ATTEN VARIANTS
set -u
TRIALS="${1:-2}"; SECS="${2:-30}"
REPO=/home/amither/Documents/SDR; SP=$REPO/scripts/rf; PW=proview
cd "$REPO"
for trial in $(seq "$TRIALS"); do
for v in ${VARIANTS:-liquid fixed}; do
  if [ "$v" = liquid ]; then unset SDR_TSYNC; else export SDR_TSYNC=$v; fi
  echo "$PW" | sudo -S rm -f /tmp/sdr_stats_A.json /tmp/sdr_stats_B.json 2>/dev/null
  rm -f /tmp/cpu.txt
  ip route | grep -q '169\.254\.0\.0/16' || \
    echo "$PW" | sudo -S ip route add 169.254.0.0/16 dev enp3s0 metric 90 2>/dev/null
  START=$(date +%s)
  ( sleep 14; for i in $(seq 8); do
      echo "$PW" | sudo -S ps -o %cpu= -C sdr-datalink 2>/dev/null | awk '{s+=$1} END{if(s>0)print s}'
      sleep 2
    done > /tmp/cpu.txt ) &
  out=$(timeout $((SECS*8+120)) "$SP/datalink.sh" "$SECS" 2>&1)
  wait
  gp=$(echo "$out" | grep -oE 'goodput +: .*' | head -1 | sed 's/goodput *: //')
  cpu=$(awk '{s+=$1;n++} END{if(n)printf "%.0f%%",s/n; else printf "n/a"}' /tmp/cpu.txt)
  python3 - "$v" "$trial" "$gp" "$cpu" "$START" <<'PY'
import json,os,sys
v,tr,gp,cpu,start=sys.argv[1],sys.argv[2],sys.argv[3],sys.argv[4],int(sys.argv[5])
def rd(p):
    if not os.path.exists(p): return None,"MISSING"
    if os.path.getmtime(p) < start: return None,"STALE"
    return json.load(open(p)),"ok"
A,sa=rd('/tmp/sdr_stats_A.json'); B,sb=rd('/tmp/sdr_stats_B.json')
if sa!="ok" or sb!="ok":
    print(f"t{tr} {v:<8} INVALID (A={sa} B={sb})"); raise SystemExit
d=B.get('bursts_detected') or 0; m=B.get('bursts_demodulated') or 0
g=B.get('frames_rx_good') or 0; bad=B.get('frames_rx_bad') or 0
print(f"t{tr} {v:<8} A_tx={A.get('frames_tx'):<6} good={g:<6} bad={bad:<5} "
      f"CRC={100.0*g/(g+bad) if g+bad else 0:5.1f}% bursts={d:<5} "
      f"acq={100.0*m/d if d else 0:5.1f}% drop={B.get('dropped'):<3} cpu={cpu:<5} {gp}")
PY
done
done
