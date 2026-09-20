#!/bin/sh
# Does appliance_start.sh hand the bridge the packet size the BITSTREAM reports?
#
# The bridge reads PKT_BYTES-sized blocks and sizes its IIO buffer at
# PKT_BYTES/4 samples. Decoding across a packet boundary loses frames silently,
# and the bridge refuses to start on a mismatch -- so on an 8192-byte bitstream
# an appliance that never passes --pkt does not degrade, it does not come up at
# all. This exercises both directions: the value is passed when the bitstream
# publishes one, and startup is REFUSED when the published value is impossible.
#
# The script under test hardcodes /mnt/jffs2 and /sys, so it runs here as a
# path-rewritten copy. The rewrite is then REVERSED and compared byte-for-byte
# against the original: if the two ever diverge by anything but those paths the
# test aborts instead of quietly testing a stale copy.
set -u
SRC=$(dirname "$0")/../../release/appliance/appliance_start.sh
SCHEMA=$(dirname "$0")/../../release/appliance/config_schema.sh
[ -f "$SRC" ] || { echo "no $SRC"; exit 2; }

for d in /proc/[0-9]*; do
    case "$(readlink "$d/exe" 2>/dev/null)" in
        */sdr_bridge*) echo "PRECONDITION: a real sdr_bridge is running (pid ${d#/proc/});"
                       echo "  the script would exit early and every case would falsely pass."
                       exit 2 ;;
    esac
done

ROOT=$(mktemp -d); trap 'rm -rf "$ROOT"' EXIT
rewrite() {
    sed -e "s#/mnt/jffs2#$ROOT/jffs2#g" \
        -e "s#/sys/bus/iio/devices#$ROOT/sys/iio#g" \
        -e "s#/sys/class/net#$ROOT/sys/net#g" \
        -e "s#/tmp/appliance.log#$ROOT/appliance.log#g" "$1"
}
rewrite "$SRC" > "$ROOT/start.sh"
sed -e "s#$ROOT/jffs2#/mnt/jffs2#g" \
    -e "s#$ROOT/sys/iio#/sys/bus/iio/devices#g" \
    -e "s#$ROOT/sys/net#/sys/class/net#g" \
    -e "s#$ROOT/appliance.log#/tmp/appliance.log#g" "$ROOT/start.sh" > "$ROOT/back.sh"
if ! cmp -s "$ROOT/back.sh" "$SRC"; then
    echo "PRECONDITION: the path rewrite is not reversible; the copy under test"
    echo "  differs from $SRC by more than its paths. Refusing to report a result."
    exit 2
fi
grep -q 'RX_PKT_BYTES' "$ROOT/start.sh" || {
    echo "PRECONDITION: no RX_PKT_BYTES logic in $SRC -- nothing to test."; exit 2; }

mkdir -p "$ROOT/jffs2/tools" "$ROOT/sys/net/eth0"
for i in 0 1 2; do mkdir -p "$ROOT/sys/iio/iio:device$i"; done
echo ad9361-phy                > "$ROOT/sys/iio/iio:device0/name"
echo cf-ad9361-dds-core-lpc    > "$ROOT/sys/iio/iio:device1/name"
echo cf-ad9361-lpc             > "$ROOT/sys/iio/iio:device2/name"
echo 1 > "$ROOT/sys/net/eth0/carrier"
[ -f "$SCHEMA" ] && cp "$SCHEMA" "$ROOT/jffs2/config_schema.sh" && chmod +x "$ROOT/jffs2/config_schema.sh"
cat > "$ROOT/jffs2/bridge.conf" <<'CONF'
CONFIG_VERSION=1
MODE=raw-eth
IFACE=eth0
SAMPLE_RATE=15360000
FREQUENCY=434000000
RX_FREQUENCY=444000000
DIFF_MODE=1
NODE_ID=2
CONF
for t in tx_fabric.sh rx_framed.sh; do echo '#!/bin/sh' > "$ROOT/jffs2/tools/$t"; done
cat > "$ROOT/jffs2/sdr_bridge" <<'BR'
#!/bin/sh
printf '%s\n' "$*" > "$ARGS_FILE"
BR
chmod +x "$ROOT/jffs2/sdr_bridge"
# No appliance_supervise.sh on purpose: the script then execs the bridge
# directly, so the stub captures the real argv instead of the supervisor's.

mkdir -p "$ROOT/bin"
cat > "$ROOT/bin/devmem" <<'DM'
#!/bin/sh
a=$(printf '0x%08X' "$(( $1 ))")
case "$a" in
    0x43C50000) echo 0x5344524C ;;
    0x43C50008|0x43C5000C) echo 0x00000003 ;;
    0x43C50018) echo "$FPGA_PKT" ;;
    0x43C10010|0x43C00010) echo 0x00000001 ;;
    0x43C10020|0x43C00028) echo 0x00000001 ;;
    0x79020400|0x79020440) echo 0x00000051 ;;
    *) echo 0x00000000 ;;
esac
DM
chmod +x "$ROOT/bin/devmem"
PATH="$ROOT/bin:$PATH"; export PATH

pass=0; fail=0
run() {   # run <fpga_pkt_value>
    rm -f "$ROOT/args" "$ROOT/appliance.log"
    FPGA_PKT="$1" ARGS_FILE="$ROOT/args" sh "$ROOT/start.sh" >/dev/null 2>&1
    ARGS=$(cat "$ROOT/args" 2>/dev/null || true)
}
ck() { if [ "$2" = "$3" ]; then pass=$((pass+1)); echo "  ok   $1"
       else fail=$((fail+1)); echo "  FAIL $1: expected [$3] got [$2]"; fi; }

echo "positive: the bitstream publishes a size, the bridge is told it"
run 0x00002000; ck "8192 passed through"        "$ARGS" "--raw-eth eth0 --node-id 2 --pkt 8192"
run 0x00008000; ck "32768 passed through"       "$ARGS" "--raw-eth eth0 --node-id 2 --pkt 32768"
run 0x00000800; ck "2048 (minimum) passed"      "$ARGS" "--raw-eth eth0 --node-id 2 --pkt 2048"

echo "compatibility: an identity block predating the field must not regress"
run 0x00000000; ck "field absent -> no --pkt"   "$ARGS" "--raw-eth eth0 --node-id 2"

echo "negative: an impossible published size must REFUSE to forward"
run 0x00001800; ck "6144 not a power of two"    "${ARGS:-<not started>}" "<not started>"
run 0x00000400; ck "1024 below minimum"         "${ARGS:-<not started>}" "<not started>"
run 0x00020000; ck "131072 above maximum"       "${ARGS:-<not started>}" "<not started>"

echo "negative: the refusal must say why"
run 0x00001800
grep -q 'RX_PKT_BYTES.*not a power of two' "$ROOT/appliance.log" \
    && { pass=$((pass+1)); echo "  ok   reason logged"; } \
    || { fail=$((fail+1)); echo "  FAIL no reason in the log"; }

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
