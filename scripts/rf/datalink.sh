#!/bin/bash
# Two-node RF link test over the air or over coax.
#
# Node A: TAP sdr0, 10.99.0.1, root namespace,  config.json
# Node B: TAP sdr1, 10.99.0.2, netns "rxns",    config_node2.json
#
# sdr1 lives in its own namespace deliberately. With both TAPs in the root
# namespace on one subnet the kernel routes between them directly and NOTHING
# crosses the air -- every "success" would be local loopback.
#
# env: MOD BW BWF BLAST_SZ BLAST_RATE B_ATTEN SDR_TSYNC TCP=1 SECS=<n>
set -u
SECS="${1:-30}"
REPO=/home/amither/Documents/SDR
SP=$REPO/scripts/rf
PW=proview
BIN=$REPO/build/src/daemon/sdr-datalink
s_() { echo "$PW" | sudo -S "$@" 2>/dev/null; }

cleanup() {
  pkill -f "$SP/blast.py" >/dev/null 2>&1
  echo "$PW" | sudo -S pkill -x sdr-datalink >/dev/null 2>&1
  s_ pkill -f udp_sink.py >/dev/null 2>&1
  s_ pkill -f "iperf3" >/dev/null 2>&1
  sleep 2
  s_ ip netns del rxns >/dev/null 2>&1
  s_ ip link del sdr0  >/dev/null 2>&1
  s_ ip link del sdr1  >/dev/null 2>&1
}
trap cleanup EXIT
cleanup

A=$("$SP/plutoip.sh" A) || exit 1
B=$("$SP/plutoip.sh" B) || exit 1
echo "=== A=$A  B=$B  mod=${MOD:-BPSK} bw=${BW:-1}MHz tsync=${SDR_TSYNC:-liquid} ==="

for pair in "config.json $A 0x00000001 434 439 sdr0 A" \
            "config_node2.json $B 0x00000002 439 434 sdr1 B"; do
  set -- $pair
  python3 - "$1" "$2" "$3" "$4" "$5" "$6" "$7" <<'PY'
import json,os,sys
cfg,ip,nid,ftx,frx,tap,tag = sys.argv[1:8]
c=json.load(open(f"/home/amither/Documents/SDR/{cfg}"))
c.update(pluto_ip=ip, node_id=nid, freq_tx_mhz=float(ftx), freq_rx_mhz=float(frx),
         tap_iface=tap, mode="bridge", stats_path=f"/tmp/sdr_stats_{tag}.json",
         modulation=os.environ.get("MOD","BPSK"), bw_mhz=int(os.environ.get("BW","1")),
         rx_bw_factor=float(os.environ.get("BWF","1.35")), spectrum_interval_ms=0)
if tag=="B" and os.environ.get("B_ATTEN"): c["tx_atten_db"]=float(os.environ["B_ATTEN"])
if tag=="A" and os.environ.get("A_ATTEN"): c["tx_atten_db"]=float(os.environ["A_ATTEN"])
json.dump(c,open(f"/home/amither/Documents/SDR/{cfg}","w"),indent=2)
PY
done

echo "$PW" | sudo -S bash -c \
  "cd $REPO && SDR_PROFILE=1 nohup $BIN --config config.json > /tmp/dA.log 2>&1 &" 
sleep 3
echo "$PW" | sudo -S bash -c \
  "cd $REPO && ${SDR_TSYNC:+SDR_TSYNC=$SDR_TSYNC} SDR_PROFILE=1 SDR_FRAME_LOG=/tmp/frames_B.txt nohup $BIN --config config_node2.json > /tmp/dB.log 2>&1 &"
sleep 8

s_ ip addr add 10.99.0.1/24 dev sdr0
s_ ip link set sdr0 up
s_ ip netns add rxns
s_ ip link set sdr1 netns rxns
s_ ip netns exec rxns ip addr add 10.99.0.2/24 dev sdr1
s_ ip netns exec rxns ip link set sdr1 up
s_ ip netns exec rxns ip link set lo up
MA=$(cat /sys/class/net/sdr0/address 2>/dev/null)
MB=$(s_ ip netns exec rxns cat /sys/class/net/sdr1/address 2>/dev/null)
s_ ip neigh replace 10.99.0.2 lladdr "$MB" dev sdr0 nud permanent
s_ ip netns exec rxns ip neigh replace 10.99.0.1 lladdr "$MA" dev sdr1 nud permanent
sleep 2

if [ "${TCP:-0}" = "1" ]; then
  echo "=== TCP test (${SECS}s) ==="
  s_ ip netns exec rxns iperf3 -s -1 -D --logfile /tmp/iperf_s.txt >/dev/null 2>&1
  sleep 2
  s_ timeout $((SECS+20)) iperf3 -c 10.99.0.2 -t "$SECS" -i 5 2>&1 | tail -12
else
  echo "=== UDP throughput, ${SECS}s ==="
  s_ ip netns exec rxns python3 "$SP/udp_sink.py" > /tmp/sink.txt 2>&1 &
  sleep 1
  python3 "$SP/blast.py" "${BLAST_SZ:-1000}" "${BLAST_RATE:-20}" "$SECS" >/dev/null 2>&1
  sleep 4
  s_ pkill -f udp_sink.py >/dev/null 2>&1
  sleep 2
  cat /tmp/sink.txt
fi

sleep 2
echo "=== node stats ==="
for t in A B; do
  python3 - "$t" <<'PY'
import json,sys
t=sys.argv[1]
try: s=json.load(open(f"/tmp/sdr_stats_{t}.json"))
except Exception: print(f"  node {t}: no stats"); raise SystemExit
print(f"  node {t}: tx={s.get('frames_tx')} rx_good={s.get('frames_rx_good')} "
      f"rx_bad={s.get('frames_rx_bad')} bursts={s.get('bursts_detected')} "
      f"demod={s.get('bursts_demodulated')} dropped={s.get('dropped')} "
      f"snr={s.get('snr_db')} occ={s.get('rx_occupancy_pct')}% "
      f"txduty_now={s.get('tx_duty_now_pct')}% txduty_peak={s.get('tx_duty_peak_pct')}%")
PY
done
