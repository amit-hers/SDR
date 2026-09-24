#!/bin/sh
# config_schema.sh: the radio parameters are validated, required, and migrated.
#
# Schema 2 moved TX_RF_BANDWIDTH, RX_RF_BANDWIDTH, TX_ATTENUATION_DB,
# RX_GAIN_MODE and RX_GAIN_DB out of the bring-up scripts. Three things must
# hold: a schema-1 file migrates to exactly the values it was running (nothing
# chosen for the unit), every field is range-checked, and a value the AD9363
# would clamp or reject never validates.
set -u
S=${1:?config_schema.sh}
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
fails=0
ok()  { "$S" validate "$1" >/dev/null 2>&1 || { echo "  FAIL: expected valid: $2"; fails=$((fails+1)); }; }
bad() { "$S" validate "$1" >/dev/null 2>&1 && { echo "  FAIL: expected INVALID: $2"; fails=$((fails+1)); }; }

# A schema-1 file as deployed on the units today.
cat > "$W/v1" <<'C'
CONFIG_VERSION=1
MODE=raw-eth
IFACE=eth0
SAMPLE_RATE=15360000
FREQUENCY=444000000
RX_FREQUENCY=434000000
DIFF_MODE=1
STATS_S=10
NODE_ID=465429558
C
bad "$W/v1" "schema-1 file against schema 2 (radio keys missing)"
"$S" migrate "$W/v1" >/dev/null || { echo "  FAIL: migrate returned error"; fails=$((fails+1)); }
ok  "$W/v1" "migrated file"
for kv in TX_RF_BANDWIDTH=4000000 RX_RF_BANDWIDTH=4000000 TX_ATTENUATION_DB=0 RX_GAIN_MODE=slow_attack CONFIG_VERSION=2; do
  grep -q "^$kv$" "$W/v1" || { echo "  FAIL: migrate did not write $kv"; fails=$((fails+1)); }
done
[ "$(grep -c '^NODE_ID=465429558$' "$W/v1")" = 1 ] || { echo "  FAIL: migrate altered NODE_ID"; fails=$((fails+1)); }
"$S" migrate "$W/v1" 2>&1 | grep -q "already" || { echo "  FAIL: second migrate not idempotent"; fails=$((fails+1)); }

# Field checks, each from the good file with one line changed.
mut() { sed "s|^$2=.*|$2=$3|" "$W/v1" > "$W/m"; }
mut x TX_RF_BANDWIDTH 100000;      bad "$W/m" "TX_RF_BANDWIDTH below 200 kHz"
mut x TX_RF_BANDWIDTH 20000001;    bad "$W/m" "TX_RF_BANDWIDTH above 20 MHz"
mut x RX_RF_BANDWIDTH 16000000;    bad "$W/m" "RX_RF_BANDWIDTH wider than SAMPLE_RATE"
mut x RX_RF_BANDWIDTH 15360000;    ok  "$W/m" "RX_RF_BANDWIDTH equal to SAMPLE_RATE"
mut x TX_ATTENUATION_DB 12.5;      ok  "$W/m" "TX_ATTENUATION_DB 12.5"
mut x TX_ATTENUATION_DB 0.25;      ok  "$W/m" "TX_ATTENUATION_DB 0.25 (leading zero, not octal)"
mut x TX_ATTENUATION_DB 89.75;     ok  "$W/m" "TX_ATTENUATION_DB 89.75"
mut x TX_ATTENUATION_DB 90;        bad "$W/m" "TX_ATTENUATION_DB above 89.75"
mut x TX_ATTENUATION_DB 10.1;      bad "$W/m" "TX_ATTENUATION_DB not a 0.25 step"
mut x TX_ATTENUATION_DB "10 dB";   bad "$W/m" "TX_ATTENUATION_DB with unit text"
mut x TX_ATTENUATION_DB -3;        bad "$W/m" "TX_ATTENUATION_DB negative"
mut x RX_GAIN_MODE auto;           bad "$W/m" "RX_GAIN_MODE not an AD9363 mode"
mut x RX_GAIN_MODE manual;         bad "$W/m" "RX_GAIN_MODE=manual without RX_GAIN_DB"
{ cat "$W/m"; echo "RX_GAIN_DB=40"; } > "$W/m2"; ok "$W/m2" "manual gain with RX_GAIN_DB"
{ cat "$W/m"; echo "RX_GAIN_DB=74"; } > "$W/m2"; bad "$W/m2" "RX_GAIN_DB above 73"
grep -v '^TX_ATTENUATION_DB=' "$W/v1" > "$W/m"; bad "$W/m" "TX_ATTENUATION_DB missing"
mut x TX_RF_BANDWIDTH '$(reboot)'; bad "$W/m" "shell metacharacters"

[ "$fails" -eq 0 ] || { echo "config_schema: $fails failures"; exit 1; }
echo "config_schema: migration, ranges, steps, required keys: PASS"
