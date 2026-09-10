#!/bin/bash
# End-to-end loopback: two sdr_bridge instances, each with a real TUN in its own
# network namespace, the transmit FIFO of one wired to the receive FIFO of the
# other. Proves the bridge actually carries IP through the framing path, which
# no unit test can: those exercise the codec, not the process.
#
# NEEDS ROOT -- TUNSETIFF requires CAP_NET_ADMIN, and the interfaces live in
# network namespaces so both ends can hold a /30 without colliding. Skips
# cleanly when run unprivileged, so it is safe in an ordinary ctest run.
#
# WHAT THIS DOES NOT COVER: a FIFO delivers the exact byte grid that was
# written, so every frame decodes at offset 0 and the four-phase search is
# never exercised. On the air the demodulator picks a phase arbitrarily. That
# property is covered by tests/test_offsets.cpp instead.
set -u
SRC_DIR="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
BR="$SRC_DIR/build/sdr_bridge.host"

if [ "$(id -u)" != "0" ]; then
    echo "  SKIP: needs root (TUNSETIFF requires CAP_NET_ADMIN)"
    echo "PASS"; exit 0
fi
if [ ! -x "$BR" ]; then
    echo "  SKIP: $BR not built -- run fpga/tools/build_bridge.sh"
    echo "PASS"; exit 0
fi
# Root is necessary but not sufficient: a container without /dev/net/tun, or a
# kernel without the driver, cannot run this. Skip rather than fail, so the test
# can be a CI gate without turning every restricted environment into a red
# build -- a gate that fails for reasons unrelated to the code gets disabled.
if [ ! -c /dev/net/tun ]; then
    echo "  SKIP: no /dev/net/tun on this host"
    echo "PASS"; exit 0
fi
if ! ip netns add sdr_probe_$$ 2>/dev/null; then
    echo "  SKIP: cannot create network namespaces here"
    echo "PASS"; exit 0
fi
ip netns del sdr_probe_$$ 2>/dev/null
W=/tmp/sdr_e2e; rm -rf $W; mkdir -p $W
AB=$W/f_ab; BA=$W/f_ba
mkfifo $AB $BA

cleanup() {
  kill $PA $PB 2>/dev/null
  sleep 1
  kill -9 $PA $PB 2>/dev/null
  ip netns del nsA 2>/dev/null
  ip netns del nsB 2>/dev/null
}
trap cleanup EXIT

ip netns del nsA 2>/dev/null; ip netns del nsB 2>/dev/null
ip netns add nsA || exit 1
ip netns add nsB || exit 1

# Hold both FIFOs open read-write so neither bridge blocks in open(). Both open
# their transmit device before their receive device, so without this each waits
# for the other's reader and nothing starts. The holder never reads, so it does
# not compete for data.
exec 3<>$AB
exec 4<>$BA

echo "=== starting bridge A (node 1) ==="
ip netns exec nsA $BR --iface sdrA --local 172.30.99.1 --peer 172.30.99.2 \
    --tx $AB --rx $BA --node-id 1 --pkt 65536 --stats 3 > $W/a.log 2>&1 &
PA=$!
echo "=== starting bridge B (node 2) ==="
ip netns exec nsB $BR --iface sdrB --local 172.30.99.2 --peer 172.30.99.1 \
    --tx $BA --rx $AB --node-id 2 --pkt 65536 --stats 3 > $W/b.log 2>&1 &
PB=$!

sleep 4
echo "=== bridge A startup ==="; head -6 $W/a.log
echo "=== bridge B startup ==="; head -6 $W/b.log
echo "=== interfaces ==="
ip netns exec nsA ip -4 addr show sdrA 2>&1 | grep inet
ip netns exec nsB ip -4 addr show sdrB 2>&1 | grep inet

echo
echo "=== PING 172.30.99.2 FROM NAMESPACE A (real IP over the framing path) ==="
ip netns exec nsA ping -c 5 -W 3 -i 0.5 172.30.99.2 2>&1 | tail -8

echo
echo "=== larger payload: 1200-byte ICMP ==="
ip netns exec nsA ping -c 3 -W 3 -s 1200 172.30.99.2 2>&1 | tail -5

echo
echo "=== final statistics ==="
echo "--- A ---"; tail -2 $W/a.log
echo "--- B ---"; tail -2 $W/b.log

# Verdict. Both sizes must cross with no loss, and nothing may have failed CRC:
# a bridge that delivers most packets is not a bridge, it is a fault report.
small=$(ip netns exec nsA ping -c 5 -W 3 -i 0.3 172.30.99.2 2>&1 | grep -o "[0-9]*% packet loss" | tr -d "% packetlos")
large=$(ip netns exec nsA ping -c 3 -W 3 -s 1200 172.30.99.2 2>&1 | grep -o "[0-9]*% packet loss" | tr -d "% packetlos")
crc=$(grep -o "crcerr [0-9]*" $W/a.log $W/b.log | awk "{s+=\$2} END {print s+0}")
echo
echo "loss small=${small:-100}%  loss large=${large:-100}%  crc errors=${crc}"
if [ "${small:-100}" != "0" ] || [ "${large:-100}" != "0" ]; then
    echo "FAIL: packets were lost across the framing path"; exit 1
fi
if [ "${crc:-1}" != "0" ]; then
    echo "FAIL: $crc CRC failures on a lossless transport"; exit 1
fi
echo "PASS"
