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
printf '%s\n' '{"tx":{"packets":7},"peer":{"compatibility":"COMPATIBLE"}}' > "$W/metrics"
cat > "$W/log" <<'EOF'
1s ordinary line
2s bridge: PEER_COMPATIBLE (node 2)
3s bridge: RECOVERY drained demod soft_reset
4s ordinary line
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
  --conf "$W/bridge.conf" --iio-dir "$W/iio" \
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

kill "$PID"; wait "$PID" || true; PID=""
chmod 644 "$W/token"
if "$API" --bind 127.0.0.1 --port "$PORT" --token-file "$W/token" \
     --status-program "$W/status" --metrics-file "$W/metrics" --log-file "$W/log" \
     >"$W/insecure.out" 2>"$W/insecure.err"; then
  echo "insecure token permissions were accepted"; exit 1
fi
grep -q 'mode 0600' "$W/insecure.err"

echo "sdr-agent authentication/read-only/section/radio/negative paths: PASS"
