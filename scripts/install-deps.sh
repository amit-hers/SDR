#!/usr/bin/env bash
# Install build dependencies for SDR Datalink on Ubuntu/Debian
set -euo pipefail

echo "[deps] Updating package lists..."
sudo apt-get update -qq

echo "[deps] Installing system packages..."
sudo apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config git \
    libssl-dev \
    libiio-dev \
    libliquid-dev \
    python3-pip python3-venv \
    net-tools iproute2 bridge-utils

echo "[deps] Installing Python monitor dependencies..."
pip3 install --user flask flask-sock

echo "[deps] All dependencies installed."
echo ""
echo "Build with:"
echo "  cmake -B build -DCMAKE_BUILD_TYPE=Release"
echo "  cmake --build build -j\$(nproc)"
