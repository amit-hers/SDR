#!/bin/sh
# Supervise the bridge: restart it when it dies OR hangs, with a reason, and
# stop rather than loop for ever.
#
# WHY A SUPERVISOR AND NOT A RESPAWN LOOP:
#
#  * A HUNG bridge is not a dead one. The process stays alive holding the
#    single-open IIO character device, so nothing else can take the hardware and
#    no exit is ever observed. Liveness therefore has to be progress, not
#    existence -- the same lesson as the demodulator recovery, where "the
#    process is running" and "the link is working" turned out to be unrelated.
#
#  * Restarting for ever is its own outage. If a fault is not transient, an
#    endless restart loop hides it and keeps the radio keyed. After a budget of
#    attempts this enters a FAULT state: forwarding stopped, reason recorded,
#    and it stays there for a human.
#
#  * Duplicate instances are worse than none. Two bridges on one interface both
#    capture promiscuously and both transmit, so every frame crosses the radio
#    twice. Ownership is enforced before each start.
set -u
LOG=${LOG:-/tmp/appliance.log}
BRIDGE=/mnt/jffs2/sdr_bridge
STATE=/tmp/supervisor.state
MAX_RESTARTS=${MAX_RESTARTS:-5}      # within the window, then FAULT
WINDOW=${WINDOW:-600}                # seconds
HANG_CHECKS=${HANG_CHECKS:-3}        # consecutive no-progress polls = hung
POLL=${POLL:-20}

log() { printf '%s SUPERVISOR %s\n' "$(cut -d. -f1 /proc/uptime)s" "$*" >> "$LOG"; }

# Kill by executable identity. A command-line pattern is also present in the
# argv of whatever searches for it, which has killed the searching shell here
# more than once.
kill_bridges() {
    _me=$$
    for _d in /proc/[0-9]*; do
        _p=${_d#/proc/}; [ "$_p" = "$_me" ] && continue
        _e=$(readlink "$_d/exe" 2>/dev/null) || continue
        case "$_e" in */sdr_bridge*|*/iio_readdev*|*/iio_writedev*) kill -9 "$_p" 2>/dev/null;; esac
    done
}
count_bridges() {
    _n=0
    for _d in /proc/[0-9]*; do
        _e=$(readlink "$_d/exe" 2>/dev/null) || continue
        case "$_e" in */sdr_bridge*) _n=$((_n+1));; esac
    done
    echo "$_n"
}

restarts=0; window_start=$(cut -d. -f1 /proc/uptime)
while :; do
    # ── ownership: exactly one bridge owns the hardware path ────────────────
    n=$(count_bridges)
    if [ "$n" -gt 1 ]; then
        log "found $n bridges; killing all before starting one (duplicates double every frame)"
        kill_bridges; sleep 2; n=0
    fi

    if [ "$n" -eq 0 ]; then
        now=$(cut -d. -f1 /proc/uptime)
        [ $((now - window_start)) -gt "$WINDOW" ] && { restarts=0; window_start=$now; }
        if [ "$restarts" -ge "$MAX_RESTARTS" ]; then
            log "FAULT: $restarts restarts within ${WINDOW}s. Forwarding is STOPPED."
            log "FAULT: the fault is not transient; restarting again would hide it"
            log "FAULT: and keep the transmitter keyed. Intervention required."
            kill_bridges
            exit 2
        fi
        restarts=$((restarts+1))
        log "starting bridge (attempt $restarts of $MAX_RESTARTS in this window)"
        setsid "$BRIDGE" $BRIDGE_ARGS >> "$LOG" 2>&1 &
        sleep 8
        [ "$(count_bridges)" -eq 0 ] && log "bridge exited immediately after start"
    fi

    # ── liveness: progress, not existence ───────────────────────────────────
    # The bridge appends a statistics line every STATS_S. A live one grows the
    # log; a hung one does not, while still holding the IIO device.
    before=$(wc -c < "$LOG" 2>/dev/null || echo 0)
    stall=0
    while [ "$stall" -lt "$HANG_CHECKS" ]; do
        sleep "$POLL"
        _n=$(count_bridges)
        [ "$_n" -eq 0 ] && { log "bridge died; will restart"; break; }
        # Check for duplicates HERE too, not only at the top of the outer loop.
        # That loop can sit in this poll for HANG_CHECKS*POLL seconds, and a
        # second bridge doubles every frame onto the radio for the whole of
        # that window. Measured: a manually started bridge survived alongside
        # the supervised one for the full interval.
        if [ "$_n" -gt 1 ]; then
            log "found $_n bridges mid-poll; killing all (duplicates double every frame)"
            kill_bridges; sleep 2
            break
        fi
        after=$(wc -c < "$LOG" 2>/dev/null || echo 0)
        if [ "$after" -gt "$before" ]; then stall=0; before=$after
        else stall=$((stall+1)); fi
    done
    if [ "$stall" -ge "$HANG_CHECKS" ]; then
        log "HUNG: no statistics for $((HANG_CHECKS*POLL))s while the process is alive"
        log "HUNG: killing it so the IIO device is released, then restarting"
        kill_bridges; sleep 2
    fi
done
