#!/usr/bin/env python3
"""
SDR Data Link Web Management Server
Run: python3 server.py [--port 8080]
Open: http://localhost:8080
"""
import flask
from flask import Flask, render_template, jsonify, request, Response, stream_with_context
import subprocess, json, os, time, signal, threading, shlex, sys

app = Flask(__name__)

BASE         = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CFG_FILE     = os.path.join(BASE, "config.json")
STATS_FILE   = "/tmp/sdr_link_stats.json"
SOL8_STATS   = "/tmp/sol8_stats.json"
SOL8_SCAN    = "/tmp/sol8_scan.json"
SOL8_CTRL    = "/tmp/sol8_ctrl"

# Running link processes (tx/rx can run simultaneously for full-duplex)
procs = {"tx": None, "rx": None, "sol8": None}
proc_lock = threading.Lock()

# ── Config helpers ─────────────────────────────────────────────────────────

def load_config():
    try:
        with open(CFG_FILE) as f:
            return json.load(f)
    except Exception:
        return {}

def save_config(cfg):
    with open(CFG_FILE, "w") as f:
        json.dump(cfg, f, indent=2)

# ── Build CLI args from config ─────────────────────────────────────────────

def build_tx_args(cfg):
    r, l, s = cfg.get("radio", {}), cfg.get("link", {}), cfg.get("security", {})
    args = [
        os.path.join(BASE, "link_tx"),
        "--freq",   str(r.get("freq_mhz", 434.0)),
        "--rate",   str(r.get("rate_msps", 20)),
        "--mod",    r.get("mod", "QPSK"),
        "--atten",  str(r.get("tx_atten_db", 10)),
        "--pluto",  l.get("pluto_ip", "192.168.2.1"),
    ]
    if l.get("source") == "udp":
        args += ["--source", "udp", "--port", str(l.get("udp_source_port", 5005))]
    if s.get("encrypt"):
        args += ["--encrypt", "--key", s.get("key_hex", "00" * 32)]
    return args

def build_rx_args(cfg):
    r, l, s = cfg.get("radio", {}), cfg.get("link", {}), cfg.get("security", {})
    args = [
        os.path.join(BASE, "link_rx"),
        "--freq",   str(r.get("freq_mhz", 434.0)),
        "--rate",   str(r.get("rate_msps", 20)),
        "--mod",    r.get("mod", "QPSK"),
        "--pluto",  l.get("pluto_ip", "192.168.2.1"),
    ]
    if l.get("sink") == "udp":
        args += ["--sink", "udp",
                 "--sink-ip", l.get("udp_sink_ip", "127.0.0.1"),
                 "--port", str(l.get("udp_sink_port", 5006))]
    if s.get("encrypt"):
        args += ["--encrypt", "--key", s.get("key_hex", "00" * 32)]
    return args

# ── Process management ─────────────────────────────────────────────────────

def proc_status(name):
    p = procs.get(name)
    if p is None:
        return "stopped"
    if p.poll() is None:
        return "running"
    return "stopped"

def start_proc(name, args):
    with proc_lock:
        if procs.get(name) and procs[name].poll() is None:
            return False, "already running"
        try:
            p = subprocess.Popen(
                args,
                stdin  = subprocess.DEVNULL if name == "rx" else subprocess.PIPE,
                stdout = subprocess.DEVNULL if name == "tx" else subprocess.PIPE,
                stderr = subprocess.PIPE,
            )
            procs[name] = p
            # Drain stderr in background so it doesn't block
            def drain():
                for line in p.stderr:
                    pass
            threading.Thread(target=drain, daemon=True).start()
            return True, "started"
        except Exception as e:
            return False, str(e)

def stop_proc(name):
    with proc_lock:
        p = procs.get(name)
        if p and p.poll() is None:
            p.terminate()
            try:
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                p.kill()
            procs[name] = None
            return True, "stopped"
        return False, "not running"

# ── Routes ─────────────────────────────────────────────────────────────────

@app.route("/")
def index():
    return render_template("index.html")

@app.route("/api/config", methods=["GET"])
def api_config_get():
    return jsonify(load_config())

@app.route("/api/config", methods=["POST"])
def api_config_set():
    data = request.get_json(force=True)
    save_config(data)
    return jsonify({"ok": True})

@app.route("/api/status")
def api_status():
    stats = {}
    try:
        with open(STATS_FILE) as f:
            stats = json.load(f)
    except Exception:
        pass
    return jsonify({
        "tx": proc_status("tx"),
        "rx": proc_status("rx"),
        "stats": stats,
    })

@app.route("/api/start/<mode>", methods=["POST"])
def api_start(mode):
    if mode not in ("tx", "rx", "duplex"):
        return jsonify({"ok": False, "msg": "unknown mode"}), 400
    cfg = load_config()
    results = {}
    if mode in ("tx", "duplex"):
        ok, msg = start_proc("tx", build_tx_args(cfg))
        results["tx"] = msg
    if mode in ("rx", "duplex"):
        ok, msg = start_proc("rx", build_rx_args(cfg))
        results["rx"] = msg
    return jsonify({"ok": True, "results": results})

@app.route("/api/stop/<mode>", methods=["POST"])
def api_stop(mode):
    if mode not in ("tx", "rx", "all"):
        return jsonify({"ok": False, "msg": "unknown mode"}), 400
    results = {}
    if mode in ("tx", "all"):
        ok, msg = stop_proc("tx")
        results["tx"] = msg
    if mode in ("rx", "all"):
        ok, msg = stop_proc("rx")
        results["rx"] = msg
    return jsonify({"ok": True, "results": results})

@app.route("/api/events")
def api_events():
    """Server-Sent Events: push stats every second to browser."""
    def generate():
        while True:
            yield f"data: {json.dumps(_merge_stats())}\n\n"
            time.sleep(1)
    return Response(stream_with_context(generate()),
                    mimetype="text/event-stream",
                    headers={"Cache-Control": "no-cache",
                             "X-Accel-Buffering": "no"})

@app.route("/api/config/export")
def api_export():
    cfg = load_config()
    return Response(
        json.dumps(cfg, indent=2),
        mimetype="application/json",
        headers={"Content-Disposition": "attachment; filename=sdr_link_config.json"}
    )

@app.route("/api/config/import", methods=["POST"])
def api_import():
    try:
        data = request.get_json(force=True)
        save_config(data)
        return jsonify({"ok": True})
    except Exception as e:
        return jsonify({"ok": False, "msg": str(e)}), 400

@app.route("/api/power/tx", methods=["POST"])
def api_power_tx():
    """Change TX attenuation and/or frequency live (no restart needed)."""
    d = request.get_json(force=True)
    atten = d.get("atten_db")
    freq  = d.get("freq_mhz")
    errors = []

    if atten is not None:
        atten = int(max(0, min(89, atten)))
        freq_cur = freq if freq is not None else (load_config().get("radio",{}).get("freq_mhz", 434.0))
        # Write control file for link_tx
        with open("/tmp/sdr_tx_ctrl", "w") as f:
            f.write(f"{atten} {float(freq_cur):.6f}\n")
        # Update config
        cfg = load_config()
        cfg.setdefault("radio", {})["tx_atten_db"] = atten
        if freq is not None:
            cfg["radio"]["freq_mhz"] = float(freq)
        save_config(cfg)

    p = procs.get("tx")
    if p and p.poll() is None:
        try:
            import os as _os
            _os.kill(p.pid, 10)  # SIGUSR1 = 10
        except Exception as e:
            errors.append(str(e))
        msg = "applied live"
    else:
        msg = "saved (take effect on next TX start)"

    return jsonify({"ok": True, "msg": msg, "errors": errors})


@app.route("/api/freq/rx", methods=["POST"])
def api_freq_rx():
    """Retune RX frequency live."""
    d = request.get_json(force=True)
    freq = float(d.get("freq_mhz", 434.0))
    with open("/tmp/sdr_rx_ctrl", "w") as f:
        f.write(f"{freq:.6f}\n")
    cfg = load_config()
    cfg.setdefault("radio", {})["freq_mhz"] = freq
    save_config(cfg)

    p = procs.get("rx")
    if p and p.poll() is None:
        try:
            import os as _os
            _os.kill(p.pid, 10)
            msg = "applied live"
        except Exception as e:
            msg = f"saved (signal failed: {e})"
    else:
        msg = "saved (take effect on next RX start)"
    return jsonify({"ok": True, "msg": msg})


@app.route("/api/loopback", methods=["POST"])
def api_loopback():
    """Run link_test loopback (no hardware needed)."""
    exe = os.path.join(BASE, "link_test")
    if not os.path.exists(exe):
        return jsonify({"ok": False, "msg": "link_test not built — run make link-test first"})
    try:
        r = subprocess.run([exe, "200", "40.0"], capture_output=True, text=True, timeout=60)
        return jsonify({"ok": r.returncode == 0, "output": r.stdout + r.stderr})
    except Exception as e:
        return jsonify({"ok": False, "msg": str(e)})


# ── SOL8 process management ────────────────────────────────────────────────

def build_sol8_args(cfg, mode, extra=None):
    """Build sol8 CLI args from config + mode."""
    s8 = cfg.get("sol8", {})
    r  = cfg.get("radio", {})
    exe = os.path.join(BASE, "sol8")
    args = [exe, "--mode", mode]

    # Frequencies
    freq_tx = s8.get("freq_tx", r.get("freq_mhz", 434.0))
    freq_rx = s8.get("freq_rx", freq_tx)
    if mode in ("mesh", "l2bridge"):
        args += ["--freq-tx", str(freq_tx), "--freq-rx", str(freq_rx)]
    else:
        args += ["--freq", str(freq_tx)]

    # BW, node ID, TX power
    bw      = s8.get("bw_mhz", 10)
    node_id = s8.get("node_id", "0x00000001")
    atten   = r.get("tx_atten_db", 10)
    mod     = s8.get("mod", "AUTO")
    pluto   = cfg.get("link", {}).get("pluto_ip", "192.168.2.1")

    args += ["--bw", str(bw),
             "--node-id", str(node_id),
             "--tx-power", str(-atten),
             "--mod", mod,
             "--pluto-ip", pluto]

    # Encryption
    sec = cfg.get("security", {})
    if sec.get("encrypt") and sec.get("key_hex"):
        args += ["--key", sec["key_hex"]]

    # Scan options
    if mode == "scan":
        args += ["--scan-step", str(s8.get("scan_step", 1.0)),
                 "--scan-n",    str(s8.get("scan_n", 20))]

    if extra:
        args += extra
    return args


@app.route("/api/sol8/start", methods=["POST"])
def api_sol8_start():
    d    = request.get_json(force=True) or {}
    mode = d.get("mode", "p2p-rx")
    cfg  = load_config()
    # Merge any extra sol8 config from the request
    if "sol8" in d:
        cfg.setdefault("sol8", {}).update(d["sol8"])
        save_config(cfg)
    args = build_sol8_args(cfg, mode)
    ok, msg = start_proc("sol8", args)
    return jsonify({"ok": ok, "msg": msg, "mode": mode, "args": args})


@app.route("/api/sol8/stop", methods=["POST"])
def api_sol8_stop():
    ok, msg = stop_proc("sol8")
    return jsonify({"ok": ok, "msg": msg})


@app.route("/api/sol8/status")
def api_sol8_status():
    stats = {}
    try:
        with open(SOL8_STATS) as f:
            stats = json.load(f)
    except Exception:
        pass
    return jsonify({
        "running": proc_status("sol8") == "running",
        "stats": stats,
    })


@app.route("/api/sol8/scan")
def api_sol8_scan():
    """Return last channel scan results from sol8."""
    try:
        with open(SOL8_SCAN) as f:
            return jsonify({"ok": True, "scan": json.load(f)})
    except Exception as e:
        return jsonify({"ok": False, "msg": str(e), "scan": []})


@app.route("/api/sol8/ctrl", methods=["POST"])
def api_sol8_ctrl():
    """Send runtime control to running sol8 (atten, freq_tx, freq_rx, mod)."""
    d     = request.get_json(force=True) or {}
    atten = d.get("atten_db", -1)
    ftx   = d.get("freq_tx", -1.0)
    frx   = d.get("freq_rx", -1.0)
    mod   = d.get("mod_code", -1)
    with open(SOL8_CTRL, "w") as f:
        f.write(f"{int(atten)} {float(ftx):.6f} {float(frx):.6f} {int(mod)}\n")
    p = procs.get("sol8")
    if p and p.poll() is None:
        try:
            os.kill(p.pid, 10)   # SIGUSR1
            msg = "applied live"
        except Exception as e:
            msg = f"signal failed: {e}"
    else:
        msg = "saved (sol8 not running)"
    cfg = load_config()
    r   = cfg.setdefault("radio", {})
    s8  = cfg.setdefault("sol8",  {})
    if atten >= 0:   r["tx_atten_db"] = atten
    if ftx   > 0:    s8["freq_tx"]    = ftx
    if frx   > 0:    s8["freq_rx"]    = frx
    save_config(cfg)
    return jsonify({"ok": True, "msg": msg})


@app.route("/api/sol8/test", methods=["POST"])
def api_sol8_test():
    """Run sol8_test suite (no hardware)."""
    exe = os.path.join(BASE, "sol8_test")
    if not os.path.exists(exe):
        return jsonify({"ok": False, "msg": "sol8_test not built — run: make sol8-test"})
    try:
        r = subprocess.run([exe], capture_output=True, text=True, timeout=120)
        return jsonify({"ok": r.returncode == 0, "output": r.stdout + r.stderr})
    except Exception as e:
        return jsonify({"ok": False, "msg": str(e)})


# ── Extend SSE to include sol8 stats ──────────────────────────────────────

def _merge_stats():
    payload = {"tx": proc_status("tx"), "rx": proc_status("rx"),
               "sol8": proc_status("sol8"), "stats": {}, "sol8_stats": {}}
    try:
        with open(STATS_FILE) as f:
            payload["stats"] = json.load(f)
    except Exception:
        pass
    try:
        with open(SOL8_STATS) as f:
            payload["sol8_stats"] = json.load(f)
    except Exception:
        pass
    return payload


# ── Main ───────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    port = 8080
    if "--port" in sys.argv:
        port = int(sys.argv[sys.argv.index("--port") + 1])
    print(f"SDR Link Web UI → http://localhost:{port}")
    app.run(host="0.0.0.0", port=port, debug=False, threaded=True)
