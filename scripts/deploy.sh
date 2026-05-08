#!/usr/bin/env bash
# Deploy SDR Datalink to two remote hosts (Side A and Side B).
#
# Usage:
#   ./scripts/deploy.sh --side-a HOST_A --side-b HOST_B [OPTIONS]
#
# Options:
#   --side-a  HOST    IP/hostname of PC-A (TX=434MHz / RX=439MHz)
#   --side-b  HOST    IP/hostname of PC-B (TX=439MHz / RX=434MHz)
#   --user    USER    SSH user (default: $USER)
#   --lan     IFACE   LAN interface to bridge (default: eth0)
#   --bw      MHZ     Channel bandwidth MHz (default: 10)
#   --key     HEX     AES-256 key, 64 hex chars (enables encryption)
#   --fec             Enable Reed-Solomon FEC
#   --service         Install + enable systemd services on remote hosts
#   --pluto-ip IP     PlutoSDR IP (default: 192.168.2.1)

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(realpath "$SCRIPT_DIR/..")"
BUILD="$ROOT/build"

SIDE_A=""
SIDE_B=""
SSH_USER="${USER}"
LAN_IFACE="eth0"
BW_MHZ=10
AES_KEY=""
FEC=false
INSTALL_SERVICE=false
PLUTO_IP="192.168.2.1"
REMOTE_DIR="~/sdr-datalink"

while [[ $# -gt 0 ]]; do
    case $1 in
        --side-a)   SIDE_A="$2";    shift 2 ;;
        --side-b)   SIDE_B="$2";    shift 2 ;;
        --user)     SSH_USER="$2";  shift 2 ;;
        --lan)      LAN_IFACE="$2"; shift 2 ;;
        --bw)       BW_MHZ="$2";    shift 2 ;;
        --key)      AES_KEY="$2";   shift 2 ;;
        --fec)      FEC=true;       shift   ;;
        --service)  INSTALL_SERVICE=true; shift ;;
        --pluto-ip) PLUTO_IP="$2";  shift 2 ;;
        *) echo "Unknown argument: $1"; exit 1 ;;
    esac
done

if [[ -z "$SIDE_A" || -z "$SIDE_B" ]]; then
    echo "Usage: $0 --side-a HOST_A --side-b HOST_B [OPTIONS]"
    exit 1
fi

# ── Build ─────────────────────────────────────────────────────────────────
echo "[deploy] Building..."
cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -S "$ROOT" -G Ninja
cmake --build "$BUILD" -j"$(nproc)"
echo "[deploy] Build OK: $BUILD/sdr-datalink"

# ── Generate config JSON ───────────────────────────────────────────────────
make_config() {
    local freq_tx=$1 freq_rx=$2
    local encrypt_flag="false"
    local aes_field=''
    if [[ -n "$AES_KEY" ]]; then
        encrypt_flag="true"
        aes_field='"aes_key_hex": "'"$AES_KEY"'",'
    fi
    local fec_flag="false"
    $FEC && fec_flag="true"

    cat <<JSON
{
  "mode":             "bridge",
  "pluto_ip":         "${PLUTO_IP}",
  "freq_tx_mhz":      ${freq_tx},
  "freq_rx_mhz":      ${freq_rx},
  "bw_mhz":           ${BW_MHZ},
  "tx_atten_db":      10,
  "gain_mode":        "fast_attack",
  "modulation":       "AUTO",
  "tap_iface":        "sdr0",
  "bridge_iface":     "br0",
  "lan_iface":        "${LAN_IFACE}",
  "tap_mtu":          1386,
  "encrypt":          ${encrypt_flag},
  "fec":              ${fec_flag},
  ${aes_field}
  "stats_interval_ms": 1000,
  "monitor_port":     8080,
  "node_id":          "0x00000001",
  "scan_start_mhz":   430.0,
  "scan_step_mhz":    1.0,
  "scan_n":           20
}
JSON
}

CONFIG_A=$(make_config 434.0 439.0)
CONFIG_B=$(make_config 439.0 434.0)

# ── Deploy to a single host ────────────────────────────────────────────────
deploy_host() {
    local HOST=$1 CONFIG=$2 LABEL=$3
    local SSH="${SSH_USER}@${HOST}"

    echo "[deploy] Deploying to ${LABEL} (${HOST})..."
    ssh "$SSH" "mkdir -p $REMOTE_DIR/monitor"

    # Binary
    scp -q "$BUILD/sdr-datalink" "${SSH}:${REMOTE_DIR}/"

    # Config
    echo "$CONFIG" | ssh "$SSH" "cat > ${REMOTE_DIR}/config.json"

    # Monitor
    scp -qr "$ROOT/src/monitor/" "${SSH}:${REMOTE_DIR}/monitor/"

    # Scripts
    scp -q "$ROOT/scripts/setup-service.sh" "${SSH}:${REMOTE_DIR}/"

    if $INSTALL_SERVICE; then
        ssh "$SSH" "cd $REMOTE_DIR && sudo bash setup-service.sh \
            --config ${REMOTE_DIR}/config.json"
    else
        # Just restart daemon
        ssh "$SSH" "pkill -f sdr-datalink 2>/dev/null; sleep 1; \
            cd $REMOTE_DIR && sudo nohup ./sdr-datalink --config config.json \
            > /tmp/sdr.log 2>&1 &"
        echo "[deploy] ${LABEL}: daemon (re)started. Log: /tmp/sdr.log"
        echo "[deploy] ${LABEL}: monitor:  http://${HOST}:8080"
    fi
}

deploy_host "$SIDE_A" "$CONFIG_A" "Side-A"
deploy_host "$SIDE_B" "$CONFIG_B" "Side-B"

echo ""
echo "[deploy] Done."
echo "  Side-A monitor: http://${SIDE_A}:8080"
echo "  Side-B monitor: http://${SIDE_B}:8080"
echo ""
echo "  Verify bridge:"
echo "    ping <Side-B LAN IP>"
echo "    iperf3 -s  (on Side-B)  /  iperf3 -c <Side-B LAN IP>  (on Side-A)"
