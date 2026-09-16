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
TOOLS=/root/sdr-tools
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
