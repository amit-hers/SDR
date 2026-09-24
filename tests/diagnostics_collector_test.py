#!/usr/bin/env python3
import http.server, json
from pathlib import Path
import socketserver, subprocess, sys, tempfile, threading
TOKEN = "0123456789abcdef0123456789abcdef"
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.headers.get("Authorization") != f"Bearer {TOKEN}": self.send_response(401); self.end_headers(); return
        body = b"test log\n" if self.path.endswith("/logs") else b'{"ok":true}\n'
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def log_message(self, *_args): pass
def main():
    with tempfile.TemporaryDirectory() as temporary:
        work=Path(temporary); token=work/"token"; token.write_text(TOKEN+"\n"); token.chmod(0o600)
        with socketserver.TCPServer(("127.0.0.1",0), Handler) as server:
            threading.Thread(target=server.serve_forever, daemon=True).start()
            url=f"http://127.0.0.1:{server.server_address[1]}"
            result=subprocess.run([sys.executable,sys.argv[1],"--unit",f"UNIT-T,{url},{token}","--output",str(work/"archive")],check=True,text=True,capture_output=True)
            root=Path(result.stdout.splitlines()[0]); manifest=json.loads((root/"manifest.json").read_text())
            assert manifest["units"][0]["ok"] and len(manifest["units"][0]["files"])==6
            assert TOKEN not in (root/"manifest.json").read_text()
            assert (root/"UNIT-T"/"logs.txt").read_text()=="test log\n"
            server.shutdown()
    print("diagnostics collector: PASS")
if __name__ == "__main__": main()
