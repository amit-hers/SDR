#!/bin/sh
set -eu
API=${1:?sdr-agent binary}
W=$(mktemp -d)
PID=""
cleanup() { [ -n "$PID" ] && kill "$PID" 2>/dev/null || true; rm -rf "$W"; }
trap cleanup EXIT INT TERM

TOKEN=0123456789abcdef0123456789abcdef01234567
printf '%s\n' "$TOKEN" > "$W/token"
chmod 600 "$W/token"
cat > "$W/status" <<'EOF'
#!/bin/sh
printf '%s\n' '{"unit":{"serial":"TEST"},"fpga":{"magic":"0x5344524C"},"peer":{"compatibility":"COMPATIBLE"}}'
EOF
chmod 700 "$W/status"
# "queues" is deliberately ABSENT here (unlike production): the missing-
# section test further down relies on it being the one section this fixture
# does not have. The queue-drop event test adds it later, live.
printf '%s\n' '{"tx":{"packets":7},"peer":{"compatibility":"COMPATIBLE"}}' > "$W/metrics"
cat > "$W/log" <<'EOF'
1s ordinary line
2s bridge: PEER_COMPATIBLE (node 2)
3s bridge: RECOVERY drained demod soft_reset
4s ordinary line
5s SUPERVISOR starting bridge (attempt 1 of 5 in this window)
6s bridge: PREFLIGHT FAIL: NODE_ID is not a number
7s SUPERVISOR starting bridge (attempt 3 of 5 in this window)
EOF

# A bridge.conf and a fake IIO tree, so /api/v1/radio can be checked for the
# requested-vs-actual comparison without hardware. The LO reads back 2 Hz off
# (the PLL quantises), which must still count as a match.
printf 'FREQUENCY=444000000\nRX_FREQUENCY=434000000\nSAMPLE_RATE=15360000\n' > "$W/bridge.conf"
mkdir -p "$W/iio/iio:device0" "$W/iio/iio:device1"
echo xadc > "$W/iio/iio:device0/name"
echo ad9361-phy > "$W/iio/iio:device1/name"
echo 443999998 > "$W/iio/iio:device1/out_altvoltage1_TX_LO_frequency"
echo 433999998 > "$W/iio/iio:device1/out_altvoltage0_RX_LO_frequency"
echo 15360000  > "$W/iio/iio:device1/in_voltage_sampling_frequency"
echo fdd       > "$W/iio/iio:device1/ensm_mode"
echo '47.000000 dB' > "$W/iio/iio:device1/in_voltage0_hardwaregain"
echo slow_attack > "$W/iio/iio:device1/in_voltage0_gain_control_mode"

PORT=$((42000 + ($$ % 10000)))
"$API" --bind 127.0.0.1 --port "$PORT" --token-file "$W/token" \
  --status-program "$W/status" --metrics-file "$W/metrics" --log-file "$W/log" \
  --conf "$W/bridge.conf" --iio-dir "$W/iio" --fault-file "$W/faults.jsonl" \
  >"$W/api.out" 2>"$W/api.err" &
PID=$!
i=0
while [ "$i" -lt 30 ]; do
  code=$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/api/v1/health" 2>/dev/null || true)
  [ "$code" = 401 ] && break
  i=$((i+1)); sleep 0.1
done
if [ "${code:-}" != 401 ]; then
  if grep -q 'Operation not permitted' "$W/api.err"; then
    echo "diagnostics API socket denied by test sandbox: SKIP"
    exit 77
  fi
  echo "API did not start"; cat "$W/api.err"; exit 1
fi

req() { curl -sS -H "Authorization: Bearer $TOKEN" "$@"; }
code=$(curl -sS -o /dev/null -w '%{http_code}' -H 'Authorization: Bearer wrong-token-value-that-is-long' "http://127.0.0.1:$PORT/api/v1/health")
[ "$code" = 401 ] || { echo "wrong token accepted"; exit 1; }
req "http://127.0.0.1:$PORT/api/v1/health" | grep -q '"status":"ok"'
req "http://127.0.0.1:$PORT/api/v1/status" | grep -q '"serial":"TEST"'
req "http://127.0.0.1:$PORT/api/v1/metrics" | grep -q '"packets":7'
req "http://127.0.0.1:$PORT/metrics" | grep -q '"packets":7'
req "http://127.0.0.1:$PORT/api/v1/bridge" | grep -q '"packets":7'
# Section views: a depth-1 member of the status or metrics document, nothing
# else. "peer" exists in both documents; /peer must come from the metrics.
req "http://127.0.0.1:$PORT/api/v1/peer" | grep -q '^{"compatibility":"COMPATIBLE"}$'
req "http://127.0.0.1:$PORT/api/v1/fpga" | grep -q '^{"magic":"0x5344524C"}$'
code=$(req -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/api/v1/queues")
[ "$code" = 503 ] || { echo "missing section did not return 503"; exit 1; }
radio=$(req "http://127.0.0.1:$PORT/api/v1/radio")
echo "$radio" | grep -q '"requested":{[^}]*"tx_lo_hz":444000000'
echo "$radio" | grep -q '"actual":{[^}]*"tx_lo_hz":443999998'
echo "$radio" | grep -q '"rx_gain_db":47.000000'
echo "$radio" | grep -q '"rx_gain_mode":"slow_attack"'
echo "$radio" | grep -q '"ensm_mode":"fdd"'
echo "$radio" | grep -q '"tx_rf_bandwidth_hz":null'
echo "$radio" | grep -q '"match":{"tolerance_hz":1000,"tx_lo":true,"rx_lo":true,"sample_rate":true}'
req "http://127.0.0.1:$PORT/api/v1/logs" | grep -q 'ordinary line'
events=$(req "http://127.0.0.1:$PORT/api/v1/events")
echo "$events" | grep -q PEER_COMPATIBLE
echo "$events" | grep -q '"code":"DEMOD_RECOVERY"'
echo "$events" | grep -q '"severity":"warning"'
echo "$events" | grep -q '"node_id":null'
echo "$events" | grep -qv 'ordinary line'
# The first bridge start in a window is informational; the supervisor
# bringing a bridge back up after it died or hung is not the same event and
# must not be reported as though it were.
echo "$events" | grep -q '"code":"BRIDGE_START"'
echo "$events" | grep -q '"code":"BRIDGE_RESTART"'
python3 -c "
import json, sys
ev = json.loads('''$events''')['events']
start   = next(e for e in ev if e['code'] == 'BRIDGE_START')
restart = next(e for e in ev if e['code'] == 'BRIDGE_RESTART')
assert start['severity'] == 'info', start
assert restart['severity'] == 'warning', restart
"
echo "$events" | grep -q '"code":"PREFLIGHT_FAIL"'
echo "$events" | grep -qv 'PREFLIGHT_FAILED'   # the roadmap's exact spelling, not the old one
# No counter-derived event yet: rx.frames and both queue-drop counters are
# unchanged from where the fixture set them, so this is only a baseline poll.
echo "$events" | grep -qv 'RF_LOSS'
echo "$events" | grep -qv 'QUEUE_DROP'

# QUEUE_DROP: a drop counter that increases between two polls of the SAME
# metrics file becomes an event, without any change to the bridge or the
# supervisor -- the agent only has to notice the counter move. Two steps:
# "queues" was ABSENT until now, and its first appearance only seeds a
# baseline (a counter a freshly (re)started agent has never seen before must
# not be reported as though it just jumped from zero) -- the actual rise from
# 0 has to happen on a SECOND poll to be a real edge.
printf '%s\n' '{"tx":{"packets":7},"rx":{"frames":100},"queues":{"control_drops":0,"bulk_drops":0},"peer":{"compatibility":"COMPATIBLE"}}' > "$W/metrics"
sleep 1.2
echo "$(req "http://127.0.0.1:$PORT/api/v1/events")" | grep -qv 'QUEUE_DROP' \
  || { echo "QUEUE_DROP fired on the counter's first appearance, not a real rise"; exit 1; }
printf '%s\n' '{"tx":{"packets":7},"rx":{"frames":100},"queues":{"control_drops":3,"bulk_drops":1},"peer":{"compatibility":"COMPATIBLE"}}' > "$W/metrics"
sleep 1.2
events2=$(req "http://127.0.0.1:$PORT/api/v1/events")
echo "$events2" | grep -q 'control queue dropped 3'
echo "$events2" | grep -q 'bulk queue dropped 1'
python3 -c "
import json
ev = [e for e in json.loads('''$events2''')['events'] if e['code'] == 'QUEUE_DROP']
assert len(ev) == 2, ev
assert all(e['severity'] == 'warning' and e['subsystem'] == 'bridge' for e in ev), ev
"

# Persistent fault history: /tmp/appliance.log is on the ramdisk and gone at
# the next boot; this is the ONE thing on jffs2 that is not. Every event this
# process has classified or synthesised so far -- log-derived AND
# counter-derived alike -- must already be sitting in the on-disk journal,
# not just in the process's own RAM state.
[ -s "$W/faults.jsonl" ] || { echo "fault journal file was never written"; exit 1; }
python3 -c "
import json
with open('$W/faults.jsonl') as f:
    lines = [json.loads(l) for l in f if l.strip()]
codes = [e['code'] for e in lines]
for want in ('BRIDGE_START', 'PREFLIGHT_FAIL', 'BRIDGE_RESTART', 'PEER_COMPATIBLE', 'DEMOD_RECOVERY', 'QUEUE_DROP'):
    assert want in codes, f'{want} missing from on-disk journal: {codes}'
assert codes.count('QUEUE_DROP') == 2, codes
"
fh=$(req "http://127.0.0.1:$PORT/api/v1/fault-history")
echo "$fh" | grep -q '"code":"BRIDGE_START"'
echo "$fh" | grep -q '"code":"QUEUE_DROP"'

# The whole point: it must survive the RAMDISK LOG being wiped, which is what
# an actual reboot does to /tmp/appliance.log. Kill this instance, wipe the
# log (nothing else about a reboot matters to this check), start a BRAND NEW
# process against the SAME journal file, and confirm the history is still
# there -- proving it lives in the file, not in anything this process
# remembered in memory.
kill "$PID"; wait "$PID" || true
before_count=$(python3 -c "import json; print(sum(1 for l in open('$W/faults.jsonl') if l.strip()))")
: > "$W/log"
PORT2=$((42000 + (($$ + 1) % 10000)))
"$API" --bind 127.0.0.1 --port "$PORT2" --token-file "$W/token" \
  --status-program "$W/status" --metrics-file "$W/metrics" --log-file "$W/log" \
  --conf "$W/bridge.conf" --iio-dir "$W/iio" --fault-file "$W/faults.jsonl" \
  >"$W/api3.out" 2>"$W/api3.err" &
PID=$!
i=0
while [ "$i" -lt 30 ]; do
  code=$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT2/api/v1/health" 2>/dev/null || true)
  [ "$code" = 401 ] && break
  i=$((i+1)); sleep 0.1
done
after=$(curl -sS -H "Authorization: Bearer $TOKEN" "http://127.0.0.1:$PORT2/api/v1/fault-history")
python3 -c "
import json
n = len(json.loads('''$after''')['events'])
assert n == $before_count, f'expected {$before_count} surviving events, got {n}'
"
echo "sdr-agent persistent fault history: PASS"
kill "$PID"; wait "$PID" || true

# Back to the original instance/port for the rest of this script, which
# addresses everything via $PORT.
"$API" --bind 127.0.0.1 --port "$PORT" --token-file "$W/token" \
  --status-program "$W/status" --metrics-file "$W/metrics" --log-file "$W/log" \
  --conf "$W/bridge.conf" --iio-dir "$W/iio" --fault-file "$W/faults.jsonl" \
  >"$W/api.out" 2>"$W/api.err" &
PID=$!
i=0
while [ "$i" -lt 30 ]; do
  code=$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/api/v1/health" 2>/dev/null || true)
  [ "$code" = 401 ] && break
  i=$((i+1)); sleep 0.1
done
[ "${code:-}" = 401 ] || { echo "could not restart the shared instance after the reboot-survival check"; exit 1; }

bundle=$(req "http://127.0.0.1:$PORT/api/v1/diagnostic-bundle")
echo "$bundle" | grep -q '"bundle_version":2'
echo "$bundle" | grep -q '"radio":{"requested"'
echo "$bundle" | grep -q '"structured_events"'
echo "$bundle" | grep -q '"recent_log"'
code=$(curl -sS -o /dev/null -w '%{http_code}' -X POST -H "Authorization: Bearer $TOKEN" "http://127.0.0.1:$PORT/api/v1/status")
[ "$code" = 405 ] || { echo "write method was not rejected"; exit 1; }
code=$(req -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/api/v1/no-such-endpoint")
[ "$code" = 404 ] || { echo "unknown route was not rejected"; exit 1; }

# The API owns a 64-KiB RAM ring and bounds the producer's tmpfs log at
# 256 KiB. This is the negative path: an accidentally noisy producer must not
# be able to grow /tmp until the appliance is out of memory.
dd if=/dev/zero of="$W/log" bs=1024 count=300 oflag=append conv=notrunc 2>/dev/null
sleep 1
[ "$(wc -c < "$W/log")" -le 262144 ] || { echo "source log was not bounded"; exit 1; }
ring_bytes=$(req "http://127.0.0.1:$PORT/api/v1/logs" | wc -c)
[ "$ring_bytes" -le 65536 ] || { echo "RAM log ring exceeded 64 KiB"; exit 1; }

# Live telemetry (SSE). A streaming connection must not block the
# single-threaded accept loop from serving anyone else -- it forks -- and
# must stay within its concurrent-subscriber cap.
code=$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/api/v1/stream")
[ "$code" = 401 ] || { echo "unauthenticated stream request was not rejected"; exit 1; }

timeout 3.5 curl -sS -N -H "Authorization: Bearer $TOKEN" \
  "http://127.0.0.1:$PORT/api/v1/stream" > "$W/stream.out" 2>/dev/null || true
events=$(grep -c '^data: ' "$W/stream.out")
[ "$events" -ge 2 ] || { echo "stream produced fewer than 2 events in 3.5s: got $events"; exit 1; }
first=$(grep '^data: ' "$W/stream.out" | head -1 | sed 's/^data: //')
echo "$first" | grep -q '"rssi_db"'   || { echo "stream event missing rssi_db"; exit 1; }
echo "$first" | grep -q '"bridge":{'  || { echo "stream event missing bridge"; exit 1; }
echo "$first" | grep -q '"packets":7' || { echo "stream event's bridge section did not embed the metrics file"; exit 1; }
echo "$first" | grep -q '"supervisor":' || { echo "first stream tick is missing supervisor state"; exit 1; }
second=$(grep '^data: ' "$W/stream.out" | sed -n '2p' | sed 's/^data: //')
echo "$second" | grep -qv '"supervisor":' \
  && echo "$second" | grep -q '"rssi_db"' \
  || { echo "second tick unexpectedly carried supervisor state (or lost rssi_db)"; exit 1; }
# Poll for that connection's slot to clear rather than guess a fixed delay --
# see the identical wait further below for why a fixed sleep was flaky here.
w=0
while [ "$w" -lt 40 ]; do
  [ -z "$(ps --ppid "$PID" -o pid= 2>/dev/null)" ] && break
  w=$((w+1)); sleep 0.1
done

# While a stream is open, an ordinary request must still be served promptly --
# this is the whole reason streaming forks instead of blocking the accept loop.
timeout 3 curl -sS -N -H "Authorization: Bearer $TOKEN" \
    "http://127.0.0.1:$PORT/api/v1/stream" > /dev/null 2>&1 &
open_pid=$!
sleep 0.3
code=$(timeout 2 curl -sS -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $TOKEN" \
  "http://127.0.0.1:$PORT/api/v1/health")
[ "$code" = 200 ] || { echo "a normal request was not served while a stream was open"; exit 1; }
kill "$open_pid" 2>/dev/null; wait "$open_pid" 2>/dev/null || true
# A subscriber slot frees only once the daemon's forked child notices the
# closed socket -- up to a full ~1 Hz tick after the client is gone -- and the
# parent reaps it on its next ~250 ms poll. A fixed sleep here was flaky (the
# worst case is close to the full budget); poll for the slot to actually
# clear instead, bounded so a real leak still fails the test rather than
# hanging it.
w=0
while [ "$w" -lt 40 ]; do
  [ -z "$(ps --ppid "$PID" -o pid= 2>/dev/null)" ] && break
  w=$((w+1)); sleep 0.1
done
[ -z "$(ps --ppid "$PID" -o pid= 2>/dev/null)" ] || { echo "a stream child did not exit after its client disconnected"; exit 1; }

# Concurrent-subscriber cap (4): a 5th stream must be refused, not queued.
# Waited on by explicit PID, not a bare `wait` -- this script has already
# backgrounded and reaped jobs above, and a plain `wait` here proved to hang
# rather than return once those five clients (which do all exit on their own
# via `timeout 4`) were done.
rm -f "$W"/cap*.out
cap_pids=""
for i in 1 2 3 4 5; do
  timeout 4 curl -sS -N -H "Authorization: Bearer $TOKEN" \
    "http://127.0.0.1:$PORT/api/v1/stream" > "$W/cap$i.out" 2>/dev/null &
  cap_pids="$cap_pids $!"
done
for p in $cap_pids; do wait "$p" 2>/dev/null || true; done
refused=0
for i in 1 2 3 4 5; do
  head -c 1 "$W/cap$i.out" 2>/dev/null | grep -q '{' && refused=$((refused+1))
done
[ "$refused" -eq 1 ] || { echo "expected exactly 1 of 5 concurrent streams refused (503), got $refused"; exit 1; }
sleep 1.5

echo "sdr-agent live telemetry stream: PASS"

# RF_LOSS's own instance, separate from the one above: it needs a threshold
# short enough to actually observe firing and clearing within a test run, and
# the shared instance above must keep the real 120s default so every "no
# event yet" check earlier in this script stays true regardless of how long
# this file takes to run up to that point. --rf-loss-threshold-s exists
# expressly for this; production has no reason to ever pass it.
kill "$PID"; wait "$PID" || true; PID=""
printf '%s\n' '{"tx":{"packets":1},"rx":{"frames":100},"peer":{"compatibility":"COMPATIBLE"}}' > "$W/metrics"
"$API" --bind 127.0.0.1 --port "$PORT" --token-file "$W/token" \
  --status-program "$W/status" --metrics-file "$W/metrics" --log-file "$W/log" \
  --conf "$W/bridge.conf" --iio-dir "$W/iio" --rf-loss-threshold-s 2 \
  >"$W/rfloss.out" 2>"$W/rfloss.err" &
PID=$!
i=0
while [ "$i" -lt 30 ]; do
  code=$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/api/v1/health" 2>/dev/null || true)
  [ "$code" = 401 ] && break
  i=$((i+1)); sleep 0.1
done
sleep 0.3   # let the very first poll establish the frames=100 baseline
rfl=$(req "http://127.0.0.1:$PORT/api/v1/events")
echo "$rfl" | grep -qv 'RF_LOSS' || { echo "RF_LOSS fired before the threshold elapsed"; exit 1; }
# check_counters() only re-evaluates once per real second (by design -- this
# state machine needs nothing faster), so detection lags the configured
# threshold by up to that same ~1s depending on how the two happen to align.
# Poll for it rather than guess a fixed wait -- see the identical reasoning
# for the stream concurrent-client cap, above.
w=0; found=0
while [ "$w" -lt 80 ]; do
  rfl=$(req "http://127.0.0.1:$PORT/api/v1/events")
  echo "$rfl" | grep -q '"code":"RF_LOSS"' && { found=1; break; }
  w=$((w+1)); sleep 0.1
done
[ "$found" -eq 1 ] || { echo "RF_LOSS did not fire after a real stall"; exit 1; }
echo "$rfl" | grep -q '"severity":"error"'
echo "$rfl" | grep -qv 'RF_LOSS_CLEARED'
printf '%s\n' '{"tx":{"packets":1},"rx":{"frames":250},"peer":{"compatibility":"COMPATIBLE"}}' > "$W/metrics"
w=0; found=0
while [ "$w" -lt 40 ]; do
  rfl=$(req "http://127.0.0.1:$PORT/api/v1/events")
  echo "$rfl" | grep -q '"code":"RF_LOSS_CLEARED"' && { found=1; break; }
  w=$((w+1)); sleep 0.1
done
[ "$found" -eq 1 ] || { echo "RF_LOSS did not clear once frames resumed"; exit 1; }
echo "sdr-agent RF_LOSS fire/clear: PASS"
kill "$PID"; wait "$PID" || true

# Fault journal bound: a --fault-max-bytes small enough to reach in a test
# run, rather than writing 32 KiB of real entries to observe the same thing.
# Every surviving line must still be whole, valid JSON -- a truncation that
# split an entry mid-object would be worse than not bounding it at all.
rm -f "$W/faults2.jsonl"
printf '%s\n' '{"tx":{"packets":1},"rx":{"frames":100},"queues":{"control_drops":0,"bulk_drops":0},"peer":{"compatibility":"COMPATIBLE"}}' > "$W/metrics"
"$API" --bind 127.0.0.1 --port "$PORT" --token-file "$W/token" \
  --status-program "$W/status" --metrics-file "$W/metrics" --log-file "$W/log" \
  --conf "$W/bridge.conf" --iio-dir "$W/iio" --fault-file "$W/faults2.jsonl" --fault-max-bytes 1024 \
  >"$W/api4.out" 2>"$W/api4.err" &
PID=$!
i=0
while [ "$i" -lt 30 ]; do
  code=$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/api/v1/health" 2>/dev/null || true)
  [ "$code" = 401 ] && break
  i=$((i+1)); sleep 0.1
done
[ "${code:-}" = 401 ] || { echo "fault-journal-bound instance did not start"; exit 1; }
for n in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
  printf '{"tx":{"packets":1},"rx":{"frames":100},"queues":{"control_drops":%s,"bulk_drops":0},"peer":{"compatibility":"COMPATIBLE"}}\n' "$n" > "$W/metrics"
  sleep 1.05
done
size=$(wc -c < "$W/faults2.jsonl")
[ "$size" -le 1024 ] || { echo "fault journal exceeded its 1024 B bound: $size B"; exit 1; }
python3 -c "
with open('$W/faults2.jsonl') as f:
    lines = [l for l in f if l.strip()]
import json
for l in lines: json.loads(l)   # every surviving line must be whole, valid JSON
assert len(lines) < 15, f'expected truncation to have dropped some of 15 events, kept all {len(lines)}'
print(f'  {len(lines)} of 15 events kept within the {1024} B bound, all valid JSON')
"
echo "sdr-agent fault journal bound: PASS"
kill "$PID"; wait "$PID" || true; PID=""

# Automatic diagnostic bundle: capture on FAULT/repeated recovery, not only
# when someone happens to ask for /api/v1/diagnostic-bundle at the right
# moment. Its own dedicated instance -- short cooldown and recovery window,
# same reasoning as RF_LOSS's own instance above.
rm -rf "$W/bundles"; : > "$W/log"
printf '%s\n' '{"tx":{"packets":1},"rx":{"frames":100},"peer":{"compatibility":"COMPATIBLE"},"node_id":99}' > "$W/metrics"
"$API" --bind 127.0.0.1 --port "$PORT" --token-file "$W/token" \
  --status-program "$W/status" --metrics-file "$W/metrics" --log-file "$W/log" \
  --conf "$W/bridge.conf" --iio-dir "$W/iio" --fault-file "$W/faults3.jsonl" \
  --bundle-dir "$W/bundles" --bundle-cooldown-s 4 --recovery-window-s 8 \
  >"$W/api5.out" 2>"$W/api5.err" &
PID=$!
i=0
while [ "$i" -lt 30 ]; do
  code=$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/api/v1/health" 2>/dev/null || true)
  [ "$code" = 401 ] && break
  i=$((i+1)); sleep 0.1
done
[ "${code:-}" = 401 ] || { echo "bundle-capture instance did not start"; exit 1; }

# SUPERVISOR_FAULT captures a bundle with the fields the roadmap named.
printf '10s SUPERVISOR FAULT: too many restarts\n' >> "$W/log"
w=0; got=""
while [ "$w" -lt 30 ]; do
  got=$(curl -sS -H "Authorization: Bearer $TOKEN" "http://127.0.0.1:$PORT/api/v1/bundles")
  echo "$got" | grep -q '"trigger":"SUPERVISOR_FAULT"' && break
  w=$((w+1)); sleep 0.1
done
echo "$got" | grep -q '"trigger":"SUPERVISOR_FAULT"' || { echo "SUPERVISOR_FAULT did not capture a bundle"; exit 1; }
one=$(curl -sS -H "Authorization: Bearer $TOKEN" "http://127.0.0.1:$PORT/api/v1/bundles/latest")
for want in '"bundle_version":1' '"trigger":"SUPERVISOR_FAULT"' '"config":' '"config_fnv1a":' \
            '"fpga":{"magic":"0x5344524C"' '"radio":{"requested"' '"bridge":{' '"node_id":99' '"recent_events":{'; do
  echo "$one" | grep -q "$want" || { echo "captured bundle missing: $want"; exit 1; }
done

# Cooldown: a second fault right away must NOT capture a second bundle.
printf '11s SUPERVISOR FAULT: again immediately\n' >> "$W/log"
sleep 1.2
n=$(curl -sS -H "Authorization: Bearer $TOKEN" "http://127.0.0.1:$PORT/api/v1/bundles" | python3 -c "import json,sys; print(len(json.load(sys.stdin)['bundles']))")
[ "$n" -eq 1 ] || { echo "expected the cooldown to suppress a second capture, got $n bundles"; exit 1; }

# Past the cooldown, PREFLIGHT_FAIL captures its own bundle. board_uptime_s()
# truncates to whole seconds, so a 4s configured cooldown's real effective
# range is roughly 3-5s (same reasoning as RF_LOSS's own test above) -- wait
# comfortably past the worst case before expecting it to have cleared.
sleep 4.8
printf '20s bridge: PREFLIGHT FAIL: NODE_ID missing\n' >> "$W/log"
w=0
while [ "$w" -lt 30 ]; do
  n=$(curl -sS -H "Authorization: Bearer $TOKEN" "http://127.0.0.1:$PORT/api/v1/bundles" | python3 -c "import json,sys; print(len(json.load(sys.stdin)['bundles']))")
  [ "$n" -eq 2 ] && break
  w=$((w+1)); sleep 0.1
done
[ "$n" -eq 2 ] || { echo "PREFLIGHT_FAIL did not capture its own bundle"; exit 1; }

# REPEATED_RECOVERY: three DEMOD_RECOVERY events inside the (shortened) window.
sleep 1.5
for i in 1 2 3; do printf '%ds bridge: RECOVERY drained demod soft_reset\n' "$((30+i))" >> "$W/log"; sleep 1.2; done
w=0
while [ "$w" -lt 30 ]; do
  got=$(curl -sS -H "Authorization: Bearer $TOKEN" "http://127.0.0.1:$PORT/api/v1/bundles")
  echo "$got" | grep -q '"trigger":"REPEATED_RECOVERY"' && break
  w=$((w+1)); sleep 0.1
done
echo "$got" | grep -q '"trigger":"REPEATED_RECOVERY"' || { echo "REPEATED_RECOVERY did not capture a bundle"; exit 1; }

# Bounded storage: a run of faults spaced past the cooldown must not
# accumulate bundles without limit -- oldest are pruned, ids stay monotonic.
sleep 1.5
for i in 1 2 3 4; do
  printf '%ds SUPERVISOR FAULT: filler %d\n' "$((40+i))" "$i" >> "$W/log"
  sleep 1.3
done
final=$(curl -sS -H "Authorization: Bearer $TOKEN" "http://127.0.0.1:$PORT/api/v1/bundles")
python3 -c "
import json
bs = json.loads('''$final''')['bundles']
assert len(bs) <= 5, f'expected at most 5 bundles kept, got {len(bs)}'
ids = [b['id'] for b in bs]
assert ids == sorted(ids), f'ids not monotonic: {ids}'
"
echo "sdr-agent automatic diagnostic bundle: PASS"
kill "$PID"; wait "$PID" || true; PID=""

chmod 644 "$W/token"
if "$API" --bind 127.0.0.1 --port "$PORT" --token-file "$W/token" \
     --status-program "$W/status" --metrics-file "$W/metrics" --log-file "$W/log" \
     >"$W/insecure.out" 2>"$W/insecure.err"; then
  echo "insecure token permissions were accepted"; exit 1
fi
grep -q 'mode 0600' "$W/insecure.err"

echo "sdr-agent authentication/read-only/section/radio/negative paths: PASS"
