#!/usr/bin/env bash
# Install sdr-datalink as a systemd service on the current host.
# Usage: sudo ./setup-service.sh [--binary PATH] [--config PATH] [--monitor-dir PATH]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
if [[ -x "$SCRIPT_DIR/sdr-datalink" ]]; then
    BINARY="$SCRIPT_DIR/sdr-datalink"
else
    BINARY="$SCRIPT_DIR/../build/src/daemon/sdr-datalink"
fi
CONFIG="/etc/sdr-datalink/config.json"
MONITOR_PORT=8080
if [[ -f "$SCRIPT_DIR/monitor/server.py" ]]; then
    MONITOR_DIR="$SCRIPT_DIR/monitor"
else
    MONITOR_DIR="$SCRIPT_DIR/../src/monitor"
fi

while [[ $# -gt 0 ]]; do
    case $1 in
        --binary) BINARY="$2"; shift 2 ;;
        --config)  CONFIG="$2";       shift 2 ;;
        --monitor-dir) MONITOR_DIR="$2"; shift 2 ;;
        --monitor-port) MONITOR_PORT="$2"; shift 2 ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done
BINARY="$(realpath "$BINARY")"
MONITOR_DIR="$(realpath "$MONITOR_DIR")"
[[ -x "$BINARY" ]] || { echo "Binary not executable: $BINARY" >&2; exit 1; }
[[ -f "$MONITOR_DIR/server.py" ]] || {
    echo "Monitor server not found: $MONITOR_DIR/server.py" >&2
    exit 1
}

# Install a colocated config only when the requested path does not already
# exist. A remote deploy passes its final config path directly; copying that
# file onto itself fails under `set -e`.
if [[ ! -f "$CONFIG" ]]; then
    if [[ -f "$SCRIPT_DIR/config.json" ]]; then
        sudo mkdir -p "$(dirname "$CONFIG")"
        sudo cp "$SCRIPT_DIR/config.json" "$CONFIG"
    else
        echo "Config not found: $CONFIG" >&2
        exit 1
    fi
fi

# ── sdr-datalink.service ──────────────────────────────────────────────────
cat <<EOF | sudo tee /etc/systemd/system/sdr-datalink.service >/dev/null
[Unit]
Description=SDR Datalink RF Bridge
After=network.target

[Service]
Type=simple
ExecStart=${BINARY} --config ${CONFIG}
Restart=on-failure
RestartSec=5s
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

# ── sdr-monitor.service ───────────────────────────────────────────────────
cat <<EOF | sudo tee /etc/systemd/system/sdr-monitor.service >/dev/null
[Unit]
Description=SDR Datalink Monitor Web Server
After=network.target

[Service]
Type=simple
ExecStart=/usr/bin/python3 ${MONITOR_DIR}/server.py --port ${MONITOR_PORT} --config ${CONFIG}
Restart=on-failure
RestartSec=5s
Environment=SDR_CONFIG=${CONFIG}
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable sdr-datalink sdr-monitor
sudo systemctl start  sdr-datalink sdr-monitor

echo "[service] Installed and started."
echo "  Status: sudo systemctl status sdr-datalink"
echo "  Logs:   sudo journalctl -fu sdr-datalink"
echo "  Monitor: http://$(hostname -I | awk '{print $1}'):${MONITOR_PORT}"
