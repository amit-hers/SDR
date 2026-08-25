cd /home/amither/Documents/SDR
./build/tests/sdr-tests >/dev/null 2>&1; echo "tests exit=$?"
BURST=1 FQ=1 SDR_WALK_BIAS=0 BLAST_SZ=1000 BLAST_RATE=150 MOD=QPSK BW=2 \
  timeout 900 ./scripts/rf/datalink.sh 60 >/dev/null 2>&1
echo "=== run health (watch for capture drops from the heavy trace) ==="
grep -oE "rx_good=[0-9]+ rx_bad=[0-9]+ bursts=[0-9]+" /tmp/dB.log | tail -1
grep -oE "dropped=[0-9]+" /tmp/sdr_stats_B.json 2>/dev/null | tail -1
python3 -c "
import json;s=json.load(open('/tmp/sdr_stats_B.json'))
print('  dropped =',s.get('dropped'),' occ =',s.get('rx_occupancy_pct'),'%')"
grep -oE "[0-9.]+% of one core" /tmp/dB.log | tail -1
wc -l /tmp/fq_B.logs