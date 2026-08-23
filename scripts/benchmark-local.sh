#!/usr/bin/env bash
# Reproducible one-way A -> RF -> B throughput test for two radios on one host.
# Run as root so the daemons can create TAPs and this script can use netns.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="$ROOT_DIR/build/src/daemon/sdr-datalink"
CONFIG_A="$ROOT_DIR/configs/throughput_a.json"
CONFIG_B="$ROOT_DIR/configs/throughput_b.json"
DURATION=20
NETNS="sdr-benchmark-b"
PID_A=""
PID_B=""

usage() {
    echo "Usage: sudo $0 [--duration SECONDS] [--config-a PATH] [--config-b PATH]"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --duration) DURATION="$2"; shift 2 ;;
        --config-a) CONFIG_A="$(realpath "$2")"; shift 2 ;;
        --config-b) CONFIG_B="$(realpath "$2")"; shift 2 ;;
        --help) usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ $EUID -ne 0 ]]; then
    echo "This benchmark must run as root: sudo $0" >&2
    exit 1
fi
for path in "$BINARY" "$CONFIG_A" "$CONFIG_B"; do
    [[ -e "$path" ]] || { echo "Missing: $path" >&2; exit 1; }
done
command -v iperf3 >/dev/null || { echo "iperf3 is required" >&2; exit 1; }

LIVE_DAEMONS=""
for pid in $(pgrep -x sdr-datalink 2>/dev/null || true); do
    state="$(ps -o stat= -p "$pid" 2>/dev/null || true)"
    [[ -z "$state" || "$state" == Z* ]] && continue
    LIVE_DAEMONS="${LIVE_DAEMONS:+$LIVE_DAEMONS,}$pid"
done
if [[ -n "$LIVE_DAEMONS" ]]; then
    echo "Refusing to start: live sdr-datalink PID(s): $LIVE_DAEMONS" >&2
    ps -o pid,user,stat,args -p "$LIVE_DAEMONS" >&2 || true
    exit 1
fi
if ip netns list | awk '{print $1}' | grep -Fxq "$NETNS"; then
    echo "Refusing to reuse existing network namespace: $NETNS" >&2
    exit 1
fi

cleanup() {
    set +e
    [[ -n "$PID_A" ]] && kill -TERM "$PID_A" 2>/dev/null
    [[ -n "$PID_B" ]] && kill -TERM "$PID_B" 2>/dev/null
    [[ -n "$PID_A" ]] && wait "$PID_A" 2>/dev/null
    [[ -n "$PID_B" ]] && wait "$PID_B" 2>/dev/null
    ip netns del "$NETNS" 2>/dev/null
}
trap cleanup EXIT INT TERM

ip netns add "$NETNS"
"$BINARY" --config "$CONFIG_B" >/tmp/sdr-benchmark-b.log 2>&1 &
PID_B=$!
"$BINARY" --config "$CONFIG_A" >/tmp/sdr-benchmark-a.log 2>&1 &
PID_A=$!

for _ in $(seq 1 100); do
    ip link show sdr0 >/dev/null 2>&1 && ip link show sdr1 >/dev/null 2>&1 && break
    kill -0 "$PID_A" 2>/dev/null || { cat /tmp/sdr-benchmark-a.log; exit 1; }
    kill -0 "$PID_B" 2>/dev/null || { cat /tmp/sdr-benchmark-b.log; exit 1; }
    sleep 0.1
done
ip link show sdr0 >/dev/null 2>&1 || { echo "sdr0 was not created" >&2; exit 1; }
ip link show sdr1 >/dev/null 2>&1 || { echo "sdr1 was not created" >&2; exit 1; }

ip link set sdr1 netns "$NETNS"

# The RF bridge transports raw Ethernet bytes, not the virtio/TAP metadata
# Linux uses for deferred checksums and segmentation.  Disable those offloads
# so TCP/UDP packets have complete checksums before the daemon reads them.
ethtool -K sdr0 tx off tso off gso off gro off
ip netns exec "$NETNS" ethtool -K sdr1 tx off tso off gso off gro off

ip address replace 10.99.0.1/24 dev sdr0
ip link set sdr0 up
ip netns exec "$NETNS" ip address replace 10.99.0.2/24 dev sdr1
ip netns exec "$NETNS" ip link set lo up
ip netns exec "$NETNS" ip link set sdr1 up

MAC_B="$(ip netns exec "$NETNS" cat /sys/class/net/sdr1/address)"
MAC_A="$(cat /sys/class/net/sdr0/address)"
ip neigh replace 10.99.0.2 lladdr "$MAC_B" dev sdr0 nud permanent
ip netns exec "$NETNS" ip neigh replace 10.99.0.1 lladdr "$MAC_A" dev sdr1 nud permanent

ip netns exec "$NETNS" iperf3 -s -1 >/tmp/sdr-benchmark-iperf-server.log 2>&1 &
SERVER_PID=$!
sleep 1

echo "Running A -> B RF throughput test for $DURATION seconds..."
iperf3 -c 10.99.0.2 -t "$DURATION" -i 1 --get-server-output
wait "$SERVER_PID"

echo
echo "Node A stats: /tmp/sdr_benchmark_A.json"
echo "Node B stats: /tmp/sdr_benchmark_B.json"
echo "Daemon logs: /tmp/sdr-benchmark-a.log, /tmp/sdr-benchmark-b.log"
