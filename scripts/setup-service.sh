#!/usr/bin/env bash
# Install sdr-datalink as a systemd service on the current host.
# Usage: sudo ./setup-service.sh [--config /path/to/config.json] [--monitor-port 8080]
set -euo pipefail

BINARY="$(dirname "$0")/../build/sdr-datalink"
BINARY="$(realpath "$BINARY")"
CONFIG="/etc/sdr-datalink/config.json"
MONITOR_PORT=8080
MONITOR_DIR="$(dirname "$0")/../src/monitor"
MONITOR_DIR="$(realpath "$MONITOR_DIR")"

while [[ $# -gt 0 ]]; do
    case $1 in
        --config)  CONFIG="$2";       shift 2 ;;
        --monitor-port) MONITOR_PORT="$2"; shift 2 ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done

# Copy config
sudo mkdir -p "$(dirname "$CONFIG")"
[[ -f config.json ]] && sudo cp config.json "$CONFIG"

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
