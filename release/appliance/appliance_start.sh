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
# TX and RX frequencies are SEPARATE, and must be crossed between the two
# units: A transmits where B listens and vice versa.
#
# A single frequency does not work. The bridge transmits idle fill continuously
# to hold the demodulator's timing lock, so both radios key up permanently; on
# one channel each unit then jams its own receiver. Measured with one unit alone
# on the air: 207,991 false frames, 121,622 CRC failures, zero bytes delivered
# -- it was demodulating itself. With TX and RX 10 MHz apart and BOTH units
# transmitting, the same pair runs at 0.00% and 0.06% PER.
: "${FREQUENCY:=434000000}"          # this unit's TRANSMIT frequency
: "${RX_FREQUENCY:=${FREQUENCY}}"    # this unit's RECEIVE frequency
: "${DIFF_MODE:=1}"
# Each unit needs a DISTINCT node id. Frames carrying this unit's own id are
# discarded as self-reception, so two units left on the default exchange
# nothing while every counter looks healthy -- frames decode, bytes delivered
# stay at zero, and the discard is not reported.
: "${NODE_ID:=1}"

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

log "bring-up: fs=$SAMPLE_RATE tx_lo=$FREQUENCY rx_lo=$RX_FREQUENCY diff=$DIFF_MODE"
sh "$TOOLS/tx_fabric.sh" "$SAMPLE_RATE" "$FREQUENCY" "$DIFF_MODE" >>"$LOG" 2>&1
sh "$TOOLS/rx_framed.sh" "$SAMPLE_RATE" "$DIFF_MODE" "$RX_FREQUENCY" >>"$LOG" 2>&1

# Verify the fabric actually came up before handing over, so a failure is
# reported here rather than as unexplained bridge errors later.
MOD_EN=$(devmem 0x43C10010 32 2>/dev/null)
DEM_EN=$(devmem 0x43C00010 32 2>/dev/null)
if [ "$MOD_EN" != "0x00000001" ] || [ "$DEM_EN" != "0x00000001" ]; then
    log "modem did not enable (mod=$MOD_EN dem=$DEM_EN); refusing to start the bridge"
    exit 1
fi
if [ "$FREQUENCY" = "$RX_FREQUENCY" ]; then
    log "WARNING tx and rx are both $FREQUENCY -- this unit will jam its own"
    log "        receiver, because idle fill keeps the transmitter keyed. Set"
    log "        RX_FREQUENCY to the peer's FREQUENCY in bridge.conf."
fi
# ── Preflight: refuse to forward on a configuration that cannot work ────────
#
# Every check here is a fault that has actually occurred, and each one produced
# TOTAL SILENT FAILURE: frames decoded, counters healthy, nothing delivered.
# A bridge that forwards nothing is indistinguishable from a dead radio, so the
# safe state is not forwarding at all, loudly.
PF=0
pf_fail() { log "PREFLIGHT FAIL: $*"; PF=1; }

# 1. FPGA identity. A userspace/bitstream mismatch changes register meanings;
#    the register map has shifted between builds before.
FPGA_MAGIC=$(devmem 0x43C50000 32 2>/dev/null)
FPGA_ABI=$(devmem 0x43C50008 32 2>/dev/null)
FPGA_MAP=$(devmem 0x43C5000C 32 2>/dev/null)
[ "$FPGA_MAGIC" = "0x5344524C" ] || pf_fail "FPGA magic $FPGA_MAGIC, expected 0x5344524C"
[ "$FPGA_ABI" = "0x00000003" ]   || pf_fail "FPGA ABI $FPGA_ABI, expected 0x00000003"
[ "$FPGA_MAP" = "0x00000003" ]   || pf_fail "register map $FPGA_MAP, expected 0x00000003"

# 2. TX and RX must differ. Equal means the unit jams its own receiver, because
#    idle fill keeps the transmitter keyed: measured 207,991 false frames and
#    zero bytes delivered.
[ "$FREQUENCY" != "$RX_FREQUENCY" ] || pf_fail "tx and rx both $FREQUENCY; this unit will jam itself"

# 3. diff_mode must match between this unit's modulator and demodulator.
MOD_DIFF=$(devmem 0x43C10020 32 2>/dev/null)
DEM_DIFF=$(devmem 0x43C00028 32 2>/dev/null)
[ "$MOD_DIFF" = "$DEM_DIFF" ] || pf_fail "diff_mode mod=$MOD_DIFF dem=$DEM_DIFF; a mismatch decodes nothing"

# 4. ADC data format. 0x71 rails the demodulator input at full scale whatever
#    the RF does; the correct value on core 10.03 is 0x51.
for _ch in 0 1; do
    _f=$(devmem $((0x79020400 + 64*_ch)) 32 2>/dev/null)
    [ "$_f" = "0x00000051" ] || pf_fail "ADC chan$_ch format $_f, expected 0x00000051"
done

# 5. Node id. Two units sharing one discard every frame the other sends, as
#    self-reception, silently. This cannot be checked against the peer from
#    here, but an unset id is the case that actually happened.
case "$NODE_ID" in
    ''|*[!0-9]*) pf_fail "NODE_ID '$NODE_ID' is not a number" ;;
    *) [ "$NODE_ID" -gt 0 ] 2>/dev/null || pf_fail "NODE_ID must be > 0" ;;
esac

# 6. The data interface must exist and be up before it is put in promiscuous
#    mode and bridged.
if [ -d "/sys/class/net/$IFACE" ]; then
    [ "$(cat /sys/class/net/$IFACE/carrier 2>/dev/null)" = "1" ] \
        || log "PREFLIGHT WARN: $IFACE has no carrier; forwarding will start but carry nothing"
else
    pf_fail "interface $IFACE does not exist"
fi

if [ "$PF" -ne 0 ]; then
    log "REFUSING TO FORWARD. The modem is configured but the bridge will not"
    log "start, because a fault above makes silent total loss the likely result."
    log "Networking is left in a safe non-forwarding state."
    exit 1
fi
log "preflight OK (fpga=$FPGA_MAGIC abi=$FPGA_ABI map=$FPGA_MAP tx=$FREQUENCY rx=$RX_FREQUENCY node=$NODE_ID diff=$MOD_DIFF)"

log "modem enabled (mod=$MOD_EN dem=$DEM_EN)"

case "$MODE" in
  raw-eth) set -- --raw-eth "$IFACE" --node-id "$NODE_ID" ;;
  tun)     set -- --local "${LOCAL_IP:?}" --peer "${PEER_IP:?}" --iface "${TUN_IFACE:-sdr0}" ;;
  *)       log "unknown MODE '$MODE'"; exit 1 ;;
esac
[ -n "${STATS_S:-}" ] && set -- "$@" --stats "$STATS_S"

log "starting bridge: $*"
# Hand over to the supervisor rather than exec'ing the bridge directly, so a
# crash or a hang is recovered instead of ending the appliance silently.
SUP=/mnt/jffs2/appliance_supervise.sh
if [ -x "$SUP" ]; then
    BRIDGE_ARGS="$*" LOG="$LOG" exec "$SUP"
fi
log "no $SUP; running the bridge unsupervised"
exec "$BRIDGE" "$@" >>"$LOG" 2>&1
