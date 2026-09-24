#!/usr/bin/env python3
"""Collect bounded, authenticated diagnostic snapshots from SDR units."""
import argparse, concurrent.futures, datetime, hashlib, json, os
from pathlib import Path
import sys, urllib.error, urllib.request

ENDPOINTS = {"health.json":"/api/v1/health", "status.json":"/api/v1/status",
             "metrics.json":"/api/v1/metrics", "events.json":"/api/v1/events",
             "logs.txt":"/api/v1/logs", "bundle.json":"/api/v1/diagnostic-bundle"}
MAX_RESPONSE = 1024 * 1024

def parse_unit(value):
    try: name, url, token_file = value.split(",", 2)
    except ValueError as exc: raise argparse.ArgumentTypeError("expected NAME,URL,TOKEN_FILE") from exc
    if not name or not url.startswith(("http://", "https://")):
        raise argparse.ArgumentTypeError("unit needs a name and HTTP(S) URL")
    return name, url.rstrip("/"), Path(token_file)

def read_token(path):
    if path.stat().st_mode & 0o077: raise PermissionError(f"{path}: token file must be mode 0600")
    token = path.read_text(encoding="utf-8").strip()
    if not 32 <= len(token) <= 256: raise ValueError(f"{path}: token must contain 32..256 characters")
    return token

def fetch(url, token, timeout):
    req = urllib.request.Request(url, headers={"Authorization": f"Bearer {token}"})
    with urllib.request.urlopen(req, timeout=timeout) as response:
        length = response.headers.get("Content-Length")
        if length and int(length) > MAX_RESPONSE: raise ValueError("response too large")
        data = response.read(MAX_RESPONSE + 1)
        if len(data) > MAX_RESPONSE: raise ValueError("response too large")
        return data

def collect(unit, root, timeout):
    name, url, token_file = unit
    if not name or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_" for c in name):
        raise ValueError(f"unsafe unit name: {name!r}")
    target = root / name; target.mkdir(mode=0o700)
    token = read_token(token_file)
    result = {"name":name, "url":url, "ok":True, "files":{}, "errors":{}}
    for filename, endpoint in ENDPOINTS.items():
        try:
            data = fetch(url + endpoint, token, timeout)
            (target / filename).write_bytes(data)
            result["files"][filename] = {"bytes":len(data), "sha256":hashlib.sha256(data).hexdigest()}
        except (OSError, ValueError, urllib.error.URLError) as exc:
            result["ok"] = False; result["errors"][endpoint] = str(exc)
    return result

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--unit", action="append", type=parse_unit, required=True, metavar="NAME,URL,TOKEN_FILE")
    parser.add_argument("--output", type=Path, default=Path("diagnostic-archive"))
    parser.add_argument("--timeout", type=float, default=5.0)
    args = parser.parse_args()
    if not 0 < args.timeout <= 60: parser.error("--timeout must be greater than 0 and at most 60 seconds")
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    root = args.output / stamp; root.mkdir(parents=True, mode=0o700); os.chmod(root, 0o700)
    with concurrent.futures.ThreadPoolExecutor(max_workers=min(8, len(args.unit))) as pool:
        results = list(pool.map(lambda unit: collect(unit, root, args.timeout), args.unit))
    manifest = {"schema":1, "collected_at_utc":stamp, "collector":"collect-diagnostics.py", "units":results}
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(root)
    for result in results:
        print(f"{result['name']}: {'PASS' if result['ok'] else 'PARTIAL'} ({len(result['files'])}/{len(ENDPOINTS)} endpoints)")
        for endpoint, error in result["errors"].items(): print(f"  {endpoint}: {error}", file=sys.stderr)
    return 0 if all(result["ok"] for result in results) else 1

if __name__ == "__main__": raise SystemExit(main())
