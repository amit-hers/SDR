#!/bin/sh
# Bring the radio up as an appliance: modem first, then the bridge.
#
# ORDER IS NOT OPTIONAL. sdr_bridge does NOT configure the fabric modem or the
# AD936x -- it only opens the IIO devices and frames what arrives. Started
# against an unconfigured datapath it spawns iio_writedev/iio_readdev, both go
# defunct immediately, and every subsequent write increments the tx error
# counter while the packet counter stays frozen. Measured: err climbing past
# 2300 with tx stuck at 9 packets, and no message saying why. So configure the
# modem and the AD936x BEFORE starting the bridge.
set -u
CONF=/mnt/jffs2/bridge.conf
TOOLS=/mnt/jffs2/tools
BRIDGE=/mnt/jffs2/sdr_bridge
LOG=/tmp/appliance.log

log() { printf '%s %s\n' "$(cut -d. -f1 /proc/uptime)s" "$*" >> "$LOG"; }

[ -f "$CONF" ] || { log "no $CONF; appliance mode not configured, doing nothing"; exit 0; }
. "$CONF"

: "${MODE:=raw-eth}"
: "${IFACE:=eth0}"
: "${SAMPLE_RATE:=3840000}"
: "${FREQUENCY:=434000000}"
: "${DIFF_MODE:=1}"

[ -x "$BRIDGE" ] || { log "no $BRIDGE"; exit 1; }
[ -d "$TOOLS" ] || { log "no $TOOLS (modem bring-up scripts missing)"; exit 1; }

# Wait for the IIO devices before touching anything.
#
# At boot this runs from autorun.sh the moment the bitstream is loaded, which
# is earlier than the AD9361 and the fabric DMA cores finish probing. Running
# the bring-up against devices that are not there yet fails in the silent way
# described above -- the modem is left unconfigured and the bridge spins on a
# dead datapath -- and by hand the race never shows, because by then probing
# finished minutes ago.
#
# Wait by NAME. The index is not stable: the stock rootfs has been seen to
# renumber these, and a script that hardcodes an index eventually addresses the
# wrong core.
wait_iio() {
    _want=$1; _tries=0
    while [ "$_tries" -lt 60 ]; do
        for _d in /sys/bus/iio/devices/iio:device*; do
            [ -r "$_d/name" ] || continue
            [ "$(cat "$_d/name" 2>/dev/null)" = "$_want" ] && return 0
        done
        _tries=$((_tries+1)); sleep 1
    done
    return 1
}
for _dev in ad9361-phy cf-ad9361-dds-core-lpc cf-ad9361-lpc; do
    if ! wait_iio "$_dev"; then
        log "timed out after 60s waiting for IIO device '$_dev'; not starting"
        exit 1
    fi
done
log "IIO devices present"

# Refuse to start a second instance.
#
# Two bridges on one interface both capture promiscuously and both transmit,
# so every frame crosses the radio twice and the receiver sees duplicates of
# traffic that was never duplicated. Identify them by executable rather than by
# a pattern match on the command line: a pattern searched for is also present
# in the searcher's own argv, so pkill/grep on the name kills the shell doing
# the killing.
for d in /proc/[0-9]*; do
    [ "${d#/proc/}" = "$$" ] && continue
    exe=$(readlink "$d/exe" 2>/dev/null) || continue
    case "$exe" in
        */sdr_bridge*)
            log "bridge already running (pid ${d#/proc/}); not starting another"
            exit 0 ;;
    esac
done

log "bring-up: fs=$SAMPLE_RATE lo=$FREQUENCY diff=$DIFF_MODE"
sh "$TOOLS/tx_fabric.sh" "$SAMPLE_RATE" "$FREQUENCY" "$DIFF_MODE" >>"$LOG" 2>&1
sh "$TOOLS/rx_framed.sh" "$SAMPLE_RATE"                            >>"$LOG" 2>&1

# Verify the fabric actually came up before handing over, so a failure is
# reported here rather than as unexplained bridge errors later.
MOD_EN=$(devmem 0x43C10010 32 2>/dev/null)
DEM_EN=$(devmem 0x43C00010 32 2>/dev/null)
if [ "$MOD_EN" != "0x00000001" ] || [ "$DEM_EN" != "0x00000001" ]; then
    log "modem did not enable (mod=$MOD_EN dem=$DEM_EN); refusing to start the bridge"
    exit 1
fi
log "modem enabled (mod=$MOD_EN dem=$DEM_EN)"

case "$MODE" in
  raw-eth) set -- --raw-eth "$IFACE" ;;
  tun)     set -- --local "${LOCAL_IP:?}" --peer "${PEER_IP:?}" --iface "${TUN_IFACE:-sdr0}" ;;
  *)       log "unknown MODE '$MODE'"; exit 1 ;;
esac
[ -n "${STATS_S:-}" ] && set -- "$@" --stats "$STATS_S"

log "starting bridge: $*"
exec "$BRIDGE" "$@" >>"$LOG" 2>&1
