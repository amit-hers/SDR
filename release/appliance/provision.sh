#!/bin/sh
# Derive this unit's operational identity from immutable hardware, and record
# what it shipped as.
#
# WHY THIS EXISTS. Two units shipped with the default NODE_ID 1. The
# self-reception filter then discarded every frame the other sent, silently:
# frames decoded, counters healthy, zero bytes delivered. A default that has to
# be edited per unit WILL eventually ship unedited, so the identity is derived
# rather than configured -- two factory-fresh boards cannot collide, because
# their MACs cannot.
#
# WHAT IS USED. The Zynq GEM MAC on eth0: burned in, unique per board, readable
# before any configuration exists. The device-tree and /proc/cpuinfo serials
# read all zeros on these units and are unusable. serial.txt lives in the
# writable jffs2, so it is recorded for traceability but never relied on for
# uniqueness.
#
#   usage: provision.sh show            print the derived identity
#          provision.sh apply <conf>    write NODE_ID into a config, atomically
#          provision.sh verify <conf>   check the config matches this hardware
set -u
REC=/mnt/jffs2/provisioning.txt
SCHEMA=/mnt/jffs2/config_schema.sh

mac_of() { cat /sys/class/net/"$1"/address 2>/dev/null | tr -d ':' ; }

derive_node_id() {
    _m=$(mac_of eth0)
    case "$_m" in
        ''|*[!0-9a-fA-F]*) echo "" ; return 1 ;;
    esac
    [ ${#_m} -eq 12 ] || { echo ""; return 1; }
    # FNV-1a over the six MAC bytes, folded to 31 bits and forced non-zero.
    # Deterministic, so the same board always derives the same id across
    # reboots and reflashes, and spread, so two boards from one OUI batch do
    # not land on the same value the way a truncation might.
    _h=2166136261
    _i=0
    while [ $_i -lt 12 ]; do
        _b=$(printf '%d' "0x$(echo "$_m" | cut -c$((_i+1))-$((_i+2)))")
        _h=$(( (_h ^ _b) & 4294967295 ))
        _h=$(( (_h * 16777619) & 4294967295 ))
        _i=$((_i+2))
    done
    _h=$(( _h & 2147483647 ))
    [ "$_h" -eq 0 ] && _h=1
    echo "$_h"
}

show() {
    _mac=$(cat /sys/class/net/eth0/address 2>/dev/null)
    _nid=$(derive_node_id) || { echo "PROVISION FAIL: cannot read a usable eth0 MAC" >&2; return 1; }
    echo "hw_mac_eth0   = $_mac"
    echo "hw_mac_usb0   = $(cat /sys/class/net/usb0/address 2>/dev/null)"
    echo "hw_serial     = $(cat /mnt/jffs2/serial.txt 2>/dev/null | head -1)"
    echo "derived_node  = $_nid"
    echo "fpga_magic    = $(devmem 0x43C50000 32 2>/dev/null)"
    echo "fpga_version  = $(devmem 0x43C50004 32 2>/dev/null)"
    echo "fpga_abi      = $(devmem 0x43C50008 32 2>/dev/null)"
    echo "fpga_regmap   = $(devmem 0x43C5000C 32 2>/dev/null)"
    echo "kernel        = $(uname -r)"
    echo "bridge_md5    = $(md5sum /mnt/jffs2/sdr_bridge 2>/dev/null | cut -d' ' -f1)"
    echo "fit_head      = $(head -c 8 /dev/mtd3 2>/dev/null | od -An -tx1 | tr -d ' \n')"
    echo "provisioned   = $(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null)"
}

apply() {
    conf=${1:?config path}
    _nid=$(derive_node_id) || { echo "PROVISION FAIL: cannot derive an id without an eth0 MAC" >&2; return 1; }
    if [ -x "$SCHEMA" ]; then
        "$SCHEMA" write "$conf" NODE_ID "$_nid" >/dev/null || return 1
    else
        echo "PROVISION FAIL: $SCHEMA missing; refusing a non-atomic write" >&2; return 1
    fi
    show > "$REC".new.$$ && { sync; mv "$REC".new.$$ "$REC"; sync; }
    echo "provisioned NODE_ID=$_nid (from eth0 MAC), record in $REC"
}

verify() {
    conf=${1:?config path}
    _nid=$(derive_node_id) || { echo "VERIFY FAIL: no usable eth0 MAC" >&2; return 1; }
    _cfg=$(grep '^NODE_ID=' "$conf" 2>/dev/null | cut -d= -f2)
    if [ -z "$_cfg" ]; then
        echo "VERIFY FAIL: NODE_ID absent from $conf" >&2; return 1
    fi
    if [ "$_cfg" != "$_nid" ]; then
        # Not necessarily wrong -- an operator may assign ids deliberately --
        # but a value that does not follow from this board is the shape of a
        # copied config, which is how the collision happened.
        echo "VERIFY WARN: NODE_ID=$_cfg does not match this board's derived $_nid"
        echo "VERIFY WARN: if this config was copied from another unit, both will"
        echo "VERIFY WARN: discard each other's frames as self-reception."
        return 2
    fi
    echo "VERIFY OK: NODE_ID=$_cfg matches this board"
}

case "${1:-}" in
    show)   show ;;
    apply)  apply "${2:-/mnt/jffs2/bridge.conf}" ;;
    verify) verify "${2:-/mnt/jffs2/bridge.conf}" ;;
    *) echo "usage: $0 show|apply|verify [config]" >&2; exit 2 ;;
esac
