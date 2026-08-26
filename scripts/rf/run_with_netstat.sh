#!/bin/bash
# Runs datalink.sh while sampling the radios' NIC counters, so network
# utilisation is measured rather than inferred from sample rates.
#
#   run_with_netstat.sh <outprefix> <secs>     (datalink env passed through)
set -u
NIC=${NIC:-enp3s0}
OUT="${1:?usage: run_with_netstat.sh <outprefix> <secs>}"
SECS="${2:-60}"
r0=$(cat /sys/class/net/$NIC/statistics/rx_bytes)
t0=$(cat /sys/class/net/$NIC/statistics/tx_bytes)
s0=$(date +%s.%N)
/home/amither/Documents/SDR/scripts/rf/datalink.sh "$SECS" >/dev/null 2>&1
s1=$(date +%s.%N)
r1=$(cat /sys/class/net/$NIC/statistics/rx_bytes)
t1=$(cat /sys/class/net/$NIC/statistics/tx_bytes)
LINK=$(cat /sys/class/net/$NIC/speed 2>/dev/null || echo 1000)
python3 - "$r0" "$r1" "$t0" "$t1" "$s0" "$s1" "$LINK" "$OUT" <<'PY'
import sys
r0,r1,t0,t1,s0,s1,link,out = sys.argv[1:9]
dt=float(s1)-float(s0)
rx=(int(r1)-int(r0))*8/dt/1e6
tx=(int(t1)-int(t0))*8/dt/1e6
link=float(link)
with open(out+"_net.txt","w") as f:
    f.write(f"nic_rx_mbps {rx:.1f}\nnic_tx_mbps {tx:.1f}\n"
            f"nic_total_mbps {rx+tx:.1f}\nlink_mbps {link:.0f}\n"
            f"utilisation_pct {100*(rx+tx)/link:.1f}\nwall_s {dt:.1f}\n")
print(f"  NIC {out}: host<-radios {rx:.0f} Mbps, host->radios {tx:.0f} Mbps, "
      f"total {rx+tx:.0f} of {link:.0f} Mbps = {100*(rx+tx)/link:.1f}% utilisation")
PY
for f in burst_A burst_B; do cp /tmp/$f.log "${OUT}_${f#burst_}.log" 2>/dev/null; done
cp /tmp/dA.log "${OUT}_dA.log" 2>/dev/null; cp /tmp/dB.log "${OUT}_dB.log" 2>/dev/null
