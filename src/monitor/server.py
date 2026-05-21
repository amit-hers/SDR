#!/usr/bin/env python3
"""SDR Datalink monitoring server — Flask + WebSocket, no npm required."""

import argparse
import json
import os
import signal
import subprocess
import threading
import time

from flask import Flask, jsonify, request, render_template, abort
from flask_sock import Sock

# ── Paths ─────────────────────────────────────────────────────────────────────
_HERE       = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.abspath(os.path.join(_HERE, "..", ".."))
BINARY      = os.path.join(PROJECT_ROOT, "build", "src", "daemon", "sdr-datalink")

# Per-node runtime state
# Each node: { proc, cfg_path, stats_file, scan_file, reload_file, lock }
_NODES: dict[str, dict] = {}

CONFIG_PATH = os.environ.get("SDR_CONFIG", os.path.join(PROJECT_ROOT, "config.json"))

app = Flask(__name__, template_folder="templates", static_folder="static")
sock = Sock(app)


# ── helpers ───────────────────────────────────────────────────────────────────

def _read_json(path: str, default=None):
    try:
        with open(path) as f:
            return json.load(f)
    except Exception:
        return default


def _node(name: str) -> dict:
    """Return or create node state dict."""
    if name not in _NODES:
        cfg = os.path.join(PROJECT_ROOT, f"config_{name}.json")
        if name == "node1" and not os.path.exists(cfg):
            cfg = CONFIG_PATH
        _NODES[name] = {
            "proc":        None,
            "lock":        threading.Lock(),
            "cfg_path":    cfg,
            "stats_file":  f"/tmp/sdr_stats_{name}.json",
            "scan_file":   f"/tmp/sdr_scan_{name}.json",
            "reload_file": f"/tmp/sdr_reload_{name}.json",
        }
    return _NODES[name]


def _running(nd: dict) -> bool:
    with nd["lock"]:
        return nd["proc"] is not None and nd["proc"].poll() is None


def _start(nd: dict, mode: str | None = None) -> tuple[bool, str]:
    with nd["lock"]:
        if nd["proc"] and nd["proc"].poll() is None:
            return False, "already running"
        cfg = _read_json(nd["cfg_path"], {})
        if mode:
            cfg["mode"] = mode
            with open(nd["cfg_path"], "w") as f:
                json.dump(cfg, f, indent=2)

        # Patch stats/scan output paths into env so daemon writes per-node files
        env = os.environ.copy()
        env["SDR_STATS_FILE"] = nd["stats_file"]
        env["SDR_SCAN_FILE"]  = nd["scan_file"]

        if not os.path.exists(BINARY):
            return False, f"binary not found: {BINARY}"

        # bridge/mesh modes need root for TAP; run via sudo if not already root
        cmd = [BINARY, "--config", nd["cfg_path"]]
        if os.geteuid() != 0:
            cmd = ["sudo", "-n"] + cmd

        nd["proc"] = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            cwd=PROJECT_ROOT, env=env,
        )
        return True, "started"


def _stop(nd: dict) -> tuple[bool, str]:
    with nd["lock"]:
        if nd["proc"] and nd["proc"].poll() is None:
            nd["proc"].terminate()
            try:
                nd["proc"].wait(timeout=5)
            except subprocess.TimeoutExpired:
                nd["proc"].kill()
            nd["proc"] = None
            return True, "stopped"
        return False, "not running"


def _get_node_name(req) -> str:
    return request.args.get("node", "node1")


# ── routes ────────────────────────────────────────────────────────────────────

@app.route("/")
def index():
    return render_template("index.html")


@app.route("/api/nodes")
def api_nodes():
    """List all known nodes and their running state."""
    result = {}
    for name in ["node1", "node2"]:
        nd = _node(name)
        cfg = _read_json(nd["cfg_path"], {})
        result[name] = {
            "running":   _running(nd),
            "cfg_path":  nd["cfg_path"],
            "pluto_ip":  cfg.get("pluto_ip", "?"),
            "mode":      cfg.get("mode", "?"),
            "node_id":   cfg.get("node_id", "?"),
        }
    return jsonify(result)


@app.route("/api/status")
def api_status():
    name = _get_node_name(request)
    nd   = _node(name)
    stats = _read_json(nd["stats_file"], {})
    return jsonify({"running": _running(nd), "stats": stats, "node": name})


@app.route("/api/config", methods=["GET"])
def api_config_get():
    nd = _node(_get_node_name(request))
    return jsonify(_read_json(nd["cfg_path"], {}))


@app.route("/api/config", methods=["POST"])
def api_config_post():
    nd   = _node(_get_node_name(request))
    data = request.get_json(force=True)
    if not isinstance(data, dict):
        abort(400)
    with open(nd["cfg_path"], "w") as f:
        json.dump(data, f, indent=2)
    return jsonify({"ok": True})


@app.route("/api/start", methods=["POST"])
def api_start():
    nd   = _node(_get_node_name(request))
    body = request.get_json(force=True) or {}
    ok, msg = _start(nd, body.get("mode"))
    return jsonify({"ok": ok, "msg": msg})


@app.route("/api/stop", methods=["POST"])
def api_stop():
    nd = _node(_get_node_name(request))
    ok, msg = _stop(nd)
    return jsonify({"ok": ok, "msg": msg})


@app.route("/api/start_all", methods=["POST"])
def api_start_all():
    results = {}
    for name in ["node1", "node2"]:
        nd = _node(name)
        if os.path.exists(nd["cfg_path"]):
            ok, msg = _start(nd)
            results[name] = {"ok": ok, "msg": msg}
    return jsonify(results)


@app.route("/api/stop_all", methods=["POST"])
def api_stop_all():
    results = {}
    for name in ["node1", "node2"]:
        nd = _node(name)
        ok, msg = _stop(nd)
        results[name] = {"ok": ok, "msg": msg}
    return jsonify(results)


@app.route("/api/ctrl", methods=["POST"])
def api_ctrl():
    nd   = _node(_get_node_name(request))
    body = request.get_json(force=True) or {}
    try:
        with open(nd["reload_file"], "w") as f:
            json.dump(body, f)
        with nd["lock"]:
            if nd["proc"] and nd["proc"].poll() is None:
                nd["proc"].send_signal(signal.SIGUSR2)
        return jsonify({"ok": True})
    except Exception as e:
        return jsonify({"ok": False, "msg": str(e)}), 500


@app.route("/api/scan")
def api_scan():
    nd = _node(_get_node_name(request))
    return jsonify(_read_json(nd["scan_file"], []))


# ── WebSocket — streams both nodes ───────────────────────────────────────────

@sock.route("/ws")
def ws_stats(ws):
    while True:
        try:
            payload = {}
            for name in ["node1", "node2"]:
                nd = _node(name)
                payload[name] = {
                    "running": _running(nd),
                    "stats":   _read_json(nd["stats_file"], {}),
                }
            ws.send(json.dumps(payload))
            time.sleep(0.2)
        except Exception:
            break


# ── entry point ───────────────────────────────────────────────────────────────

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="SDR monitor server")
    parser.add_argument("--port",   type=int, default=8080)
    parser.add_argument("--host",   default="0.0.0.0")
    parser.add_argument("--config", default=CONFIG_PATH, help="node1 config path")
    args = parser.parse_args()
    CONFIG_PATH = os.path.abspath(args.config)
    app.run(host=args.host, port=args.port, debug=False)
