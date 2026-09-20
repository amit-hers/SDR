#!/bin/sh
# One view of what this appliance is actually doing, as JSON.
#
# WHY THIS EXISTS. Every expensive failure here was diagnosable from counters
# that existed but were not shown together. rx_self was counted and never
# printed, so two units discarding each other's frames looked like a dead
# radio. A stalled demodulator advanced rx_dma while frames stayed at zero, and
# nothing reported the pair. A bridge forwarding nothing looked identical to a
# link with no traffic.
#
# THE RULE HERE: report what is measured, and say UNKNOWN for what is not.
# A status view that guesses is worse than one with gaps, because it is trusted.
#
#   usage: appliance_status.sh [--watch]
set -u
LOG=/tmp/appliance.log
CONF=/mnt/jffs2/bridge.conf
IFACE=$(grep -s '^IFACE=' "$CONF" | cut -d= -f2); IFACE=${IFACE:-eth0}

j()  { printf '"%s":%s' "$1" "$2"; }
js() { printf '"%s":"%s"' "$1" "$2"; }
reg() { devmem "$1" 32 2>/dev/null || echo '"UNKNOWN"'; }

# The last statistics line the bridge wrote. Absent means the bridge has not
# reported yet -- which is itself the answer to "is it running".
last=$(grep -E '^bridge: tx ' "$LOG" 2>/dev/null | tail -1)
lastcpu=$(grep -E '^bridge: cpu ' "$LOG" 2>/dev/null | tail -1)
# Explicit patterns, one per field. A generic "name <number>" matcher picks the
# wrong token whenever a name is a substring of another -- "rx" also appears in
# "rx gap", "err" inside "crcerr" -- and silently returns a plausible number
# from the wrong place, which is the failure mode this whole view exists to
# prevent.
# '#' as the delimiter, not '/': two queue patterns legitimately contain a
# slash ("qdrop 3/7"), which silently broke the substitution and reported null
# where a number existed -- the same shape of parsing fault this view exists to
# catch.
g() { echo "$last$lastcpu" | sed -n "s#.*$1.*#\1#p" | head -1; }
num() { v=$(g "$1"); [ -n "$v" ] && echo "$v" || echo null; }

nbridge=0
for d in /proc/[0-9]*; do e=$(readlink "$d/exe" 2>/dev/null) || continue
  case "$e" in */sdr_bridge*) nbridge=$((nbridge+1));; esac; done
nsup=0
for d in /proc/[0-9]*; do c=$(tr '\0' ' ' < "$d/cmdline" 2>/dev/null)
  case "$c" in *appliance_supervise*) nsup=$((nsup+1));; esac; done

# LAST PROGRESS. The distinction between running and working. A bridge whose
# frame count stopped advancing is not carrying traffic however healthy every
# other field looks; this is the field that says so.
now=$(cut -d. -f1 /proc/uptime)
STAMP=/tmp/status.progress
prev_frames=$(cut -d' ' -f1 "$STAMP" 2>/dev/null)
prev_time=$(cut -d' ' -f2 "$STAMP" 2>/dev/null)
prev_bytes=$(cut -d' ' -f3 "$STAMP" 2>/dev/null)
cur_frames=$(num 'dma, \([0-9][0-9]*\) frames')
cur_bytes=$(echo "$last" | sed -n 's/.*frames, \([0-9][0-9]*\) B.*/\1/p')
cur_frames=${cur_frames:-null}; cur_bytes=${cur_bytes:-0}
if [ "$cur_frames" != "null" ] && [ -n "${prev_frames:-}" ] && [ "$cur_frames" = "$prev_frames" ]; then
    frames_idle=$(( now - ${prev_time:-$now} ))
else
    frames_idle=0; printf '%s %s %s\n' "$cur_frames" "$now" "$cur_bytes" > "$STAMP"
fi
if [ "$cur_bytes" = "${prev_bytes:-}" ]; then bytes_idle=$(( now - ${prev_time:-$now} )); else bytes_idle=0; fi

cfg_ok=UNKNOWN
[ -x /mnt/jffs2/config_schema.sh ] && {
    /mnt/jffs2/config_schema.sh validate "$CONF" >/dev/null 2>&1 && cfg_ok=valid || cfg_ok=INVALID; }
prov=UNKNOWN
[ -x /mnt/jffs2/provision.sh ] && {
    /mnt/jffs2/provision.sh verify "$CONF" >/dev/null 2>&1 && prov=matches_hardware || prov=MISMATCH; }

printf '{\n'
printf '  "unit": {'; js serial "$(cat /mnt/jffs2/serial.txt 2>/dev/null | head -1)"; printf ','
  js mac "$(cat /sys/class/net/$IFACE/address 2>/dev/null)"; printf ','
  j uptime_s "$now"; printf '},\n'
printf '  "fpga": {'; js magic "$(reg 0x43C50000)"; printf ','; js version "$(reg 0x43C50004)"; printf ','
  js abi "$(reg 0x43C50008)"; printf ','; js regmap "$(reg 0x43C5000C)"; printf '},\n'
printf '  "config": {'; js status "$cfg_ok"; printf ','; js provisioning "$prov"; printf ','
  js node_id "$(grep -s '^NODE_ID=' $CONF | cut -d= -f2)"; printf ','
  js tx_hz "$(grep -s '^FREQUENCY=' $CONF | cut -d= -f2)"; printf ','
  js rx_hz "$(grep -s '^RX_FREQUENCY=' $CONF | cut -d= -f2)"; printf ','
  js sample_rate "$(grep -s '^SAMPLE_RATE=' $CONF | cut -d= -f2)"; printf '},\n'
# Peer state needs the handshake wired into the bridge; it is not, and saying
# so is the honest answer rather than inferring compatibility from traffic.
printf '  "peer": {'; js compatibility UNKNOWN; printf ','
  js note "handshake not yet integrated into the bridge"; printf '},\n'
printf '  "modem": {'; js mod_en "$(reg 0x43C10010)"; printf ','; js dem_en "$(reg 0x43C00010)"; printf ','
  js lock_count "$(reg 0x43C00018)"; printf ','; js mu_clamped "$(reg 0x43C00030)"; printf ','
  js rssi_db "$(cat /sys/bus/iio/devices/iio:device0/in_voltage0_rssi 2>/dev/null | cut -d' ' -f1)"; printf '},\n'
printf '  "rx": {'; j dma "$(num '| rx \([0-9][0-9]*\) dma')"; printf ','; j frames "$cur_frames"; printf ','
  j delivered_bytes "${cur_bytes:-null}"; printf ','; j crc_errors "$(num '(crcerr \([0-9][0-9]*\)')"; printf ','
  j duplicates "$(num 'dup \([0-9][0-9]*\)')"; printf ','; j control "$(num 'ctrl \([0-9][0-9]*\)')"; printf ','
  j self_discarded "$(num 'self \([0-9][0-9]*\)')"; printf '},\n'
printf '  "tx": {'; j packets "$(num 'bridge: tx \([0-9][0-9]*\) pkts')"; printf ','; j idle_fill "$(num '(idle \([0-9][0-9]*\)')"; printf ','
  j errors "$(num 'err \([0-9][0-9]*\))')"; printf ','; j oversize "$(num 'oversize \([0-9][0-9]*\)')"; printf '},\n'
printf '  "ethernet": {'; j rx_packets "$(cat /sys/class/net/$IFACE/statistics/rx_packets 2>/dev/null || echo null)"; printf ','
  j tx_packets "$(cat /sys/class/net/$IFACE/statistics/tx_packets 2>/dev/null || echo null)"; printf ','
  j mtu "$(cat /sys/class/net/$IFACE/mtu 2>/dev/null || echo null)"; printf ','
  js flags "$(cat /sys/class/net/$IFACE/flags 2>/dev/null)"; printf '},\n'
printf '  "loop_guard": {'; j suppressed "$(num 'loopsup \([0-9][0-9]*\)')"; printf '},\n'
printf '  "queues": {'; j control_depth "$(num 'qctrl \([0-9][0-9]*\)')"; printf ','
  j bulk_depth "$(num 'qbulk \([0-9][0-9]*\)')"; printf ','
  j control_drops "$(num 'qdrop \([0-9][0-9]*\)/')"; printf ','
  j bulk_drops "$(num 'qdrop [0-9][0-9]*/\([0-9][0-9]*\)')"; printf ','
  j drain_ms "$(num 'qdrain \([0-9][0-9]*\)ms')"; printf '},\n'
printf '  "supervisor": {'; j bridges_running "$nbridge"; printf ','; j supervisors "$nsup"; printf ','
  j recoveries "$(num 'recoveries \([0-9][0-9]*\)')"; printf ','
  j restarts "$(grep -c 'SUPERVISOR starting bridge' $LOG 2>/dev/null | head -1 || echo 0)"; printf ','
  j faulted "$(grep -c 'SUPERVISOR FAULT' $LOG 2>/dev/null | head -1 || echo 0)"; printf '},\n'
# The two fields that separate "running" from "working".
printf '  "progress": {'; j frames_idle_s "$frames_idle"; printf ','; j delivered_idle_s "$bytes_idle"; printf ','
  js verdict "$( [ "$nbridge" -eq 0 ] && echo not_forwarding \
                 || { [ "$frames_idle" -gt 120 ] && echo NO_RX_PROGRESS || echo forwarding; } )"; printf '}\n'
printf '}\n'
