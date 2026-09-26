#!/bin/sh
# Validate bridge.conf against a versioned schema, WITHOUT sourcing it.
#
# WHY NOT JUST SOURCE IT. `. bridge.conf` executes the file. A half-written
# config -- power lost during a write -- can end mid-token, and the shell will
# either abort the startup or, worse, run whatever the truncation happens to
# spell. Configuration must be parsed as data and checked before any of it is
# allowed to take effect.
#
# WHAT A FAILURE MEANS. A config that does not validate does NOT fall back to
# defaults and start forwarding. Defaults are how two units ended up sharing
# node_id 1 and discarding every frame the other sent. An invalid config leaves
# the appliance in a safe non-forwarding state, loudly.
#
#   usage: config_schema.sh validate <file>
#          config_schema.sh write <file> <KEY> <VALUE>   (atomic)
#          config_schema.sh migrate <file>
set -u
# Schema 2 (2026-09-20) adds the radio parameters that used to be hard-coded in
# tx_fabric.sh / rx_framed.sh: TX_RF_BANDWIDTH, RX_RF_BANDWIDTH,
# TX_ATTENUATION_DB, RX_GAIN_MODE, RX_GAIN_DB. They are REQUIRED, so the file
# is the single source of truth for what the AD9363 is asked to do; `migrate`
# writes the previous hard-coded values into a schema-1 file so an existing
# unit keeps exactly the configuration it was running.
SCHEMA_VERSION=2

_err=0
fail() { echo "CONFIG INVALID: $*" >&2; _err=1; }

# Known keys. An unknown key is a typo or a config written for a newer schema;
# either way, acting on the keys we recognise and ignoring the rest would apply
# a configuration nobody wrote.
is_known() {
    case "$1" in
        CONFIG_VERSION|MODE|IFACE|SAMPLE_RATE|FREQUENCY|RX_FREQUENCY|DIFF_MODE|\
        NODE_ID|STATS_S|PROBE_INTERVAL_S|LOCAL_IP|PEER_IP|TUN_IFACE|BULK_MIN|MAX_RESTARTS|\
        TX_RF_BANDWIDTH|RX_RF_BANDWIDTH|TX_ATTENUATION_DB|RX_GAIN_MODE|RX_GAIN_DB) return 0 ;;
        *) return 1 ;;
    esac
}
is_uint() { case "$1" in ''|*[!0-9]*) return 1 ;; *) return 0 ;; esac; }
in_range() { is_uint "$1" && [ "$1" -ge "$2" ] && [ "$1" -le "$3" ]; }
# TX attenuation is set in 0.25 dB steps; accept an unsigned decimal with at
# most two places and compare in hundredths so no float arithmetic is needed.
is_udec2() { case "$1" in ''|.|*.*.*|*[!0-9.]*) return 1 ;; *.???*) return 1 ;; *) return 0 ;; esac; }
hundredths() {
    case "$1" in *.*) _h="${1%%.*}$(printf '%s00' "${1#*.}" | cut -c1-2)" ;; *) _h="${1}00" ;; esac
    # Leading zeros would make $(( )) read the number as octal.
    _h=$(printf '%s' "$_h" | sed 's/^0*//'); printf '%s' "${_h:-0}"
}

validate() {
    f=$1
    [ -r "$f" ] || { fail "cannot read $f"; return 1; }
    # A trailing line without a newline is the signature of a truncated write.
    if [ -n "$(tail -c 1 "$f")" ]; then
        fail "file does not end with a newline -- it looks truncated (interrupted write?)"
    fi
    MODE=; IFACE=; SAMPLE_RATE=; FREQUENCY=; RX_FREQUENCY=; DIFF_MODE=
    NODE_ID=; STATS_S=; PROBE_INTERVAL_S=; CONFIG_VERSION=
    TX_RF_BANDWIDTH=; RX_RF_BANDWIDTH=; TX_ATTENUATION_DB=; RX_GAIN_MODE=; RX_GAIN_DB=
    ln=0
    while IFS= read -r line; do
        ln=$((ln+1))
        case "$line" in ''|\#*) continue ;; esac
        case "$line" in
            *=*) ;;
            *) fail "line $ln is not KEY=VALUE: '$line'"; continue ;;
        esac
        k=${line%%=*}; v=${line#*=}
        case "$k" in *[!A-Z_0-9]*|'') fail "line $ln has an invalid key '$k'"; continue ;; esac
        # Reject anything that would be interpreted rather than assigned.
        case "$v" in
            *'$'*|*'`'*|*';'*|*'&'*|*'|'*|*'('*|*'<'*|*'>'*)
                fail "line $ln value contains shell metacharacters: '$v'"; continue ;;
        esac
        is_known "$k" || { fail "line $ln has unknown key '$k'"; continue; }
        eval "$k=\$v"
    done < "$f"

    # Required fields. Absent is not "use the default": a missing NODE_ID is
    # exactly how two units collide.
    for req in MODE IFACE SAMPLE_RATE FREQUENCY NODE_ID \
               TX_RF_BANDWIDTH RX_RF_BANDWIDTH TX_ATTENUATION_DB RX_GAIN_MODE; do
        eval "val=\${$req:-}"
        [ -n "$val" ] || fail "required key $req is missing"
    done

    [ -z "${CONFIG_VERSION:-}" ] && fail "CONFIG_VERSION is missing (run: config_schema.sh migrate)"
    if [ -n "${CONFIG_VERSION:-}" ] && [ "${CONFIG_VERSION:-}" != "$SCHEMA_VERSION" ]; then
        fail "CONFIG_VERSION $CONFIG_VERSION, this build understands $SCHEMA_VERSION"
    fi

    case "${MODE:-}" in raw-eth|tun|'') ;; *) fail "MODE '$MODE' is not raw-eth or tun" ;; esac
    [ -n "${SAMPLE_RATE:-}" ] && { in_range "$SAMPLE_RATE" 2083333 17280000 || fail "SAMPLE_RATE $SAMPLE_RATE outside 2083333..17280000"; }
    for fq in FREQUENCY RX_FREQUENCY; do
        eval "val=\${$fq:-}"
        [ -n "$val" ] && { in_range "$val" 325000000 3800000000 || fail "$fq $val outside the AD9363 range 325 MHz..3.8 GHz"; }
    done
    [ -n "${NODE_ID:-}" ] && { in_range "$NODE_ID" 1 4294967295 || fail "NODE_ID '$NODE_ID' must be a number >= 1"; }
    [ -n "${DIFF_MODE:-}" ] && { case "$DIFF_MODE" in 0|1) ;; *) fail "DIFF_MODE '$DIFF_MODE' must be 0 or 1" ;; esac; }
    [ -n "${STATS_S:-}" ] && { in_range "$STATS_S" 1 3600 || fail "STATS_S $STATS_S outside 1..3600"; }
    # 0 is a valid, meaningful value here (explicitly disable the probe),
    # unlike STATS_S above -- so the range allows it rather than treating an
    # explicit 0 as a schema violation.
    [ -n "${PROBE_INTERVAL_S:-}" ] && { in_range "$PROBE_INTERVAL_S" 0 3600 || fail "PROBE_INTERVAL_S $PROBE_INTERVAL_S outside 0..3600"; }

    # Radio parameters. Ranges are the AD9363's: analog RF filter 200 kHz..
    # 20 MHz, TX attenuation 0..89.75 dB in 0.25 dB steps, manual RX gain
    # 0..73 dB (the exact ceiling is LO-dependent; the driver clamps).
    for bw in TX_RF_BANDWIDTH RX_RF_BANDWIDTH; do
        eval "val=\${$bw:-}"
        [ -n "$val" ] && { in_range "$val" 200000 20000000 || fail "$bw $val outside the AD9363 range 200 kHz..20 MHz"; }
        # A filter wider than the sample rate lets aliases through unattenuated.
        [ -n "$val" ] && [ -n "${SAMPLE_RATE:-}" ] && is_uint "$val" && is_uint "$SAMPLE_RATE" \
            && [ "$val" -gt "$SAMPLE_RATE" ] && fail "$bw $val is wider than SAMPLE_RATE $SAMPLE_RATE"
    done
    if [ -n "${TX_ATTENUATION_DB:-}" ]; then
        if ! is_udec2 "$TX_ATTENUATION_DB"; then
            fail "TX_ATTENUATION_DB '$TX_ATTENUATION_DB' is not a decimal in dB (e.g. 0, 10, 12.5)"
        else
            h=$(hundredths "$TX_ATTENUATION_DB")
            [ "$h" -le 8975 ] || fail "TX_ATTENUATION_DB $TX_ATTENUATION_DB outside 0..89.75"
            [ $((h % 25)) -eq 0 ] || fail "TX_ATTENUATION_DB $TX_ATTENUATION_DB is not a multiple of 0.25 dB"
        fi
    fi
    case "${RX_GAIN_MODE:-}" in
        ''|manual|fast_attack|slow_attack|hybrid) ;;
        *) fail "RX_GAIN_MODE '$RX_GAIN_MODE' is not manual, fast_attack, slow_attack or hybrid" ;;
    esac
    if [ "${RX_GAIN_MODE:-}" = manual ]; then
        [ -n "${RX_GAIN_DB:-}" ] || fail "RX_GAIN_MODE=manual requires RX_GAIN_DB"
    fi
    [ -n "${RX_GAIN_DB:-}" ] && { in_range "$RX_GAIN_DB" 0 73 || fail "RX_GAIN_DB $RX_GAIN_DB outside 0..73"; }

    # Combinations that are individually valid and together carry nothing.
    if [ -n "${FREQUENCY:-}" ] && [ -n "${RX_FREQUENCY:-}" ] && [ "$FREQUENCY" = "$RX_FREQUENCY" ]; then
        fail "FREQUENCY and RX_FREQUENCY are both $FREQUENCY -- the unit jams its own receiver"
    fi
    if [ "${MODE:-}" = tun ]; then
        [ -n "${LOCAL_IP:-}" ] || fail "MODE=tun requires LOCAL_IP"
        [ -n "${PEER_IP:-}" ]  || fail "MODE=tun requires PEER_IP"
    fi
    return $_err
}

# Atomic write: a power loss leaves either the old file or the new one, never a
# half-written one. The rename is the commit point, and the directory is synced
# so the rename itself survives.
write_atomic() {
    f=$1; key=$2; val=$3
    d=$(dirname "$f"); t="$f.new.$$"
    if [ -r "$f" ] && grep -q "^$key=" "$f"; then
        sed "s|^$key=.*|$key=$val|" "$f" > "$t" || return 1
    else
        { [ -r "$f" ] && cat "$f"; echo "$key=$val"; } > "$t" || return 1
    fi
    sync
    mv "$t" "$f" || return 1
    sync
    echo "wrote $key=$val atomically"
}

# Bring an older file up to the current schema, atomically.
#
# Schema 1 -> 2 writes the radio values that tx_fabric.sh / rx_framed.sh used
# to hard-code, so a migrated unit runs EXACTLY what it ran before -- nothing
# is chosen for it -- and the values are now visible in the file, which is
# the whole point of moving them there. Nothing else is touched.
migrate() {
    f=$1
    cur=$(sed -n 's/^CONFIG_VERSION=//p' "$f" 2>/dev/null | head -1)
    if [ "$cur" = "$SCHEMA_VERSION" ]; then echo "already at CONFIG_VERSION=$SCHEMA_VERSION"; return 0; fi
    if [ -n "$cur" ] && [ "$cur" -gt "$SCHEMA_VERSION" ] 2>/dev/null; then
        echo "CONFIG_VERSION $cur is newer than this build ($SCHEMA_VERSION); refusing to downgrade" >&2; return 1
    fi
    t="$f.new.$$"
    {
        echo "CONFIG_VERSION=$SCHEMA_VERSION"
        grep -v '^CONFIG_VERSION=' "$f"
        [ -n "$cur" ] || grep -q '^NODE_ID=' "$f" || echo "# NODE_ID was absent; it MUST be set and unique per unit"
        if ! grep -q '^TX_RF_BANDWIDTH=' "$f"; then
            echo "# Radio parameters (schema 2). Values below are what the bring-up scripts"
            echo "# hard-coded before; the file is now the only place they are set."
            echo "TX_RF_BANDWIDTH=4000000"
        fi
        grep -q '^RX_RF_BANDWIDTH=' "$f"   || echo "RX_RF_BANDWIDTH=4000000"
        grep -q '^TX_ATTENUATION_DB=' "$f" || echo "TX_ATTENUATION_DB=0"
        grep -q '^RX_GAIN_MODE=' "$f"      || echo "RX_GAIN_MODE=slow_attack"
    } > "$t" || return 1
    sync; mv "$t" "$f"; sync
    echo "migrated ${cur:-unversioned} -> CONFIG_VERSION=$SCHEMA_VERSION"
}

case "${1:-}" in
    validate) validate "${2:?file}" && echo "CONFIG OK (schema $SCHEMA_VERSION)" ;;
    write)    write_atomic "${2:?file}" "${3:?key}" "${4:?value}" ;;
    migrate)  migrate "${2:?file}" ;;
    *) echo "usage: $0 validate|write|migrate <file> [key value]" >&2; exit 2 ;;
esac
