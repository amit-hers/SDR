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
SCHEMA_VERSION=1

_err=0
fail() { echo "CONFIG INVALID: $*" >&2; _err=1; }

# Known keys. An unknown key is a typo or a config written for a newer schema;
# either way, acting on the keys we recognise and ignoring the rest would apply
# a configuration nobody wrote.
is_known() {
    case "$1" in
        CONFIG_VERSION|MODE|IFACE|SAMPLE_RATE|FREQUENCY|RX_FREQUENCY|DIFF_MODE|\
        NODE_ID|STATS_S|LOCAL_IP|PEER_IP|TUN_IFACE|BULK_MIN|MAX_RESTARTS) return 0 ;;
        *) return 1 ;;
    esac
}
is_uint() { case "$1" in ''|*[!0-9]*) return 1 ;; *) return 0 ;; esac; }
in_range() { is_uint "$1" && [ "$1" -ge "$2" ] && [ "$1" -le "$3" ]; }

validate() {
    f=$1
    [ -r "$f" ] || { fail "cannot read $f"; return 1; }
    # A trailing line without a newline is the signature of a truncated write.
    if [ -n "$(tail -c 1 "$f")" ]; then
        fail "file does not end with a newline -- it looks truncated (interrupted write?)"
    fi
    MODE=; IFACE=; SAMPLE_RATE=; FREQUENCY=; RX_FREQUENCY=; DIFF_MODE=
    NODE_ID=; STATS_S=; CONFIG_VERSION=
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
    for req in MODE IFACE SAMPLE_RATE FREQUENCY NODE_ID; do
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

# Bring an unversioned file up to the current schema.
migrate() {
    f=$1
    grep -q '^CONFIG_VERSION=' "$f" 2>/dev/null && { echo "already versioned"; return 0; }
    t="$f.new.$$"
    { echo "CONFIG_VERSION=$SCHEMA_VERSION"; cat "$f"; } > "$t" || return 1
    grep -q '^NODE_ID=' "$f" || echo "# NODE_ID was absent; it MUST be set and unique per unit" >> "$t"
    sync; mv "$t" "$f"; sync
    echo "migrated to CONFIG_VERSION=$SCHEMA_VERSION"
}

case "${1:-}" in
    validate) validate "${2:?file}" && echo "CONFIG OK (schema $SCHEMA_VERSION)" ;;
    write)    write_atomic "${2:?file}" "${3:?key}" "${4:?value}" ;;
    migrate)  migrate "${2:?file}" ;;
    *) echo "usage: $0 validate|write|migrate <file> [key value]" >&2; exit 2 ;;
esac
