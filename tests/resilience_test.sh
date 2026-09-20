#!/bin/sh
# Software-controllable power-loss and crash resilience.
#
# SCOPE, stated first. This exercises everything reachable without removing
# power: interrupted writes, truncated and empty configs, orphaned temporaries,
# stale processes, kills during startup, peer-absent boot, and the safe-fault
# path. Actual hard-power and brownout cycling is NOT simulated here and remains
# an open hardware acceptance -- a kill is not a power cut, and claiming
# otherwise would be the same error as calling a capture without a transmitter
# a PER measurement.
#
# THE INVARIANT under test, after every fault: the unit is either forwarding
# with a validated configuration, or explicitly not forwarding with a reason.
# An ambiguous middle -- a bridge running on a configuration nobody validated,
# or a silent exit with no record -- is a failure even if traffic happens to
# flow.
#
#   usage: resilience_test.sh <unit-ip>
set -u
IP=${1:?unit ip}
SSH="sshpass -p analog ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10"
on() { timeout 120 $SSH "root@$IP" "$*" </dev/null 2>/dev/null; }
pass=0; fail=0
ck() { if [ "$2" = "$3" ]; then printf "  %-58s PASS\n" "$1"; pass=$((pass+1));
       else printf "  %-58s FAIL (got '%s' want '%s')\n" "$1" "$2" "$3"; fail=$((fail+1)); fi; }

bridges() { on 'n=0; for d in /proc/[0-9]*; do e=$(readlink $d/exe 2>/dev/null)||continue; case "$e" in */sdr_bridge*) n=$((n+1));; esac; done; echo $n'; }
stop_all() {
  on 'me=$$
for d in /proc/[0-9]*; do p=${d#/proc/}; [ "$p" = "$me" ] && continue
  e=$(readlink "$d/exe" 2>/dev/null)||continue
  case "$e" in */sdr_bridge*|*/iio_readdev*|*/iio_writedev*) kill -9 "$p" 2>/dev/null;; esac
done
for d in /proc/[0-9]*; do p=${d#/proc/}; [ "$p" = "$me" ] && continue
  c=$(tr "\0" " " < "$d/cmdline" 2>/dev/null); case "$c" in *supervise*) kill -9 "$p" 2>/dev/null;; esac
done
sleep 2; echo done' >/dev/null
}
start() { on 'rm -f /tmp/appliance.log; setsid /mnt/jffs2/appliance_start.sh </dev/null >/dev/null 2>&1 &' >/dev/null; sleep "${1:-30}"; }

echo "preconditions"
CONF=$(on 'cat /mnt/jffs2/bridge.conf 2>/dev/null | wc -l')
[ "${CONF:-0}" -gt 3 ] || { echo "  ABORT: no usable bridge.conf on $IP"; exit 2; }
on '/mnt/jffs2/config_schema.sh validate /mnt/jffs2/bridge.conf' >/dev/null 2>&1 \
  || { echo "  ABORT: the starting config does not validate; nothing below would mean anything"; exit 2; }
echo "  [precondition] $IP has a valid starting config            ok"
on 'cp /mnt/jffs2/bridge.conf /tmp/resil.good' >/dev/null
stop_all; start 32
b=$(bridges); [ "$b" = "1" ] || { echo "  ABORT: baseline did not reach a forwarding state (bridges=$b)"; exit 2; }
echo "  [precondition] baseline reaches validated forwarding      ok"
echo ""

echo "configuration faults must give an explicit safe state"
stop_all
on 'printf "MODE=raw" >> /mnt/jffs2/bridge.conf'; start 12
ck "truncated config -> not forwarding" "$(bridges)" "0"
ck "truncated config -> reason recorded" "$(on 'grep -c "CONFIGURATION REJECTED" /tmp/appliance.log')" "1"
on 'cp /tmp/resil.good /mnt/jffs2/bridge.conf'

stop_all; on ': > /mnt/jffs2/bridge.conf'; start 12
ck "empty config -> not forwarding" "$(bridges)" "0"
ck "empty config -> reason recorded" "$(on 'grep -c "CONFIGURATION REJECTED" /tmp/appliance.log')" "1"
on 'cp /tmp/resil.good /mnt/jffs2/bridge.conf'

echo ""
echo "interrupted atomic write"
stop_all
# Simulate power loss mid-write: the temp exists, the rename never happened.
on 'cp /mnt/jffs2/bridge.conf /mnt/jffs2/bridge.conf.new.9999; printf "NODE_ID=trunc" >> /mnt/jffs2/bridge.conf.new.9999'
ck "the live config is untouched by an abandoned write" \
   "$(on '/mnt/jffs2/config_schema.sh validate /mnt/jffs2/bridge.conf >/dev/null 2>&1 && echo ok || echo bad')" "ok"
start 32
ck "an orphaned temp file does not prevent forwarding" "$(bridges)" "1"
on 'rm -f /mnt/jffs2/bridge.conf.new.*'

echo ""
echo "stale processes and duplicate ownership"
stop_all; start 32
on 'setsid /mnt/jffs2/sdr_bridge --raw-eth eth0 --node-id 99 >/dev/null 2>&1 &' >/dev/null
sleep 25
n=$(bridges)
ck "a second bridge does not persist alongside the first" "$( [ "$n" -le 1 ] && echo ok || echo "n=$n" )" "ok"

echo ""
echo "killed during startup"
stop_all
on 'rm -f /tmp/appliance.log; setsid /mnt/jffs2/appliance_start.sh </dev/null >/dev/null 2>&1 &' >/dev/null
sleep 6
on 'me=$$; for d in /proc/[0-9]*; do p=${d#/proc/}; [ "$p" = "$me" ] && continue
  c=$(tr "\0" " " < "$d/cmdline" 2>/dev/null); case "$c" in *appliance_start*) kill -9 "$p" 2>/dev/null;; esac; done' >/dev/null
sleep 6
stop_all; start 32
ck "a kill during startup leaves a recoverable unit" "$(bridges)" "1"
ck "and the config still validates afterwards" \
   "$(on '/mnt/jffs2/config_schema.sh validate /mnt/jffs2/bridge.conf >/dev/null 2>&1 && echo ok || echo bad')" "ok"

echo ""
echo "identity and record stability"
id1=$(on '/mnt/jffs2/provision.sh show | grep derived_node')
stop_all; start 32
id2=$(on '/mnt/jffs2/provision.sh show | grep derived_node')
ck "derived identity is stable across a restart" "$id1" "$id2"
ck "provisioning record survives" "$(on '[ -s /mnt/jffs2/provisioning.txt ] && echo ok || echo missing')" "ok"

echo ""
echo "peer absent"
ck "a unit with no peer still reaches validated forwarding" "$(bridges)" "1"
ck "and says so rather than faulting" "$(on 'grep -c "preflight OK" /tmp/appliance.log')" "1"

echo ""
echo "----"
echo "passed $pass, failed $fail"
echo "NOT COVERED: hard power removal and brownout. A kill is not a power cut;"
echo "that remains an open hardware acceptance."
[ "$fail" -eq 0 ]
