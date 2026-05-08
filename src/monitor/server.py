#!/usr/bin/env python3
"""SDR Datalink monitoring server — Flask + WebSocket, no npm required."""

import argparse
import json
import os
import subprocess
import threading
import time

from flask import Flask, jsonify, request, render_template, abort
from flask_sock import Sock

STATS_FILE = "/tmp/sdr_stats.json"
SCAN_FILE  = "/tmp/sdr_scan.json"
RELOAD_CFG = "/tmp/sdr_reload.json"
CONFIG_PATH = os.environ.get("SDR_CONFIG", "config.json")

app = Flask(__name__, template_folder="templates", static_folder="static")
sock = Sock(app)

_daemon_proc: subprocess.Popen | None = None
_daemon_lock = threading.Lock()


# ── helpers ──────────────────────────────────────────────────────────────────

def _read_json(path: str, default=None):
    try:
        with open(path) as f:
            return json.load(f)
    except Exception:
        return default


def _read_config() -> dict:
    return _read_json(CONFIG_PATH, {})


def _write_config(data: dict):
    with open(CONFIG_PATH, "w") as f:
        json.dump(data, f, indent=2)


def _daemon_running() -> bool:
    with _daemon_lock:
        return _daemon_proc is not None and _daemon_proc.poll() is None


def _start_daemon(mode: str | None = None):
    global _daemon_proc
    with _daemon_lock:
        if _daemon_proc and _daemon_proc.poll() is None:
            return False, "already running"
        cfg = _read_config()
        if mode:
            cfg["mode"] = mode
            _write_config(cfg)
        cmd = ["sudo", "./sdr-datalink", "--config", CONFIG_PATH]
        binary = os.path.join(os.path.dirname(__file__), "..", "..", "build", "sdr-datalink")
        if os.path.exists(binary):
            cmd = ["sudo", binary, "--config", CONFIG_PATH]
        _daemon_proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        return True, "started"


def _stop_daemon():
    global _daemon_proc
    with _daemon_lock:
        if _daemon_proc and _daemon_proc.poll() is None:
            _daemon_proc.terminate()
            try:
                _daemon_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                _daemon_proc.kill()
            _daemon_proc = None
            return True, "stopped"
        return False, "not running"


# ── routes ────────────────────────────────────────────────────────────────────

@app.route("/")
def index():
    return render_template("index.html")


@app.route("/api/status")
def api_status():
    stats = _read_json(STATS_FILE, {})
    return jsonify({
        "running": _daemon_running(),
        "stats": stats,
    })


@app.route("/api/config", methods=["GET"])
def api_config_get():
    return jsonify(_read_config())


@app.route("/api/config", methods=["POST"])
def api_config_post():
    data = request.get_json(force=True)
    if not isinstance(data, dict):
        abort(400)
    _write_config(data)
    return jsonify({"ok": True})


@app.route("/api/start", methods=["POST"])
def api_start():
    body = request.get_json(force=True) or {}
    ok, msg = _start_daemon(body.get("mode"))
    return jsonify({"ok": ok, "msg": msg})


@app.route("/api/stop", methods=["POST"])
def api_stop():
    ok, msg = _stop_daemon()
    return jsonify({"ok": ok, "msg": msg})


@app.route("/api/ctrl", methods=["POST"])
def api_ctrl():
    """Live-tune without restart: {atten, freq_tx, freq_rx, mod}"""
    body = request.get_json(force=True) or {}
    try:
        j = json.dumps(body)
        with open(RELOAD_CFG, "w") as f:
            f.write(j)
        if _daemon_running():
            _daemon_proc.send_signal(__import__("signal").SIGUSR2)
        return jsonify({"ok": True})
    except Exception as e:
        return jsonify({"ok": False, "msg": str(e)}), 500


@app.route("/api/scan")
def api_scan():
    return jsonify(_read_json(SCAN_FILE, []))


# ── WebSocket ─────────────────────────────────────────────────────────────────

@sock.route("/ws")
def ws_stats(ws):
    while True:
        try:
            stats = _read_json(STATS_FILE, {})
            ws.send(json.dumps({
                "running": _daemon_running(),
                "stats": stats,
            }))
            time.sleep(0.2)
        except Exception:
            break


# ── entry point ───────────────────────────────────────────────────────────────

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="SDR monitor server")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--config", default=CONFIG_PATH)
    args = parser.parse_args()
    CONFIG_PATH = args.config
    app.run(host=args.host, port=args.port, debug=False)
