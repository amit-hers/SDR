#!/usr/bin/env python3
"""Loopback-only, dashboard with device credentials kept on the host."""
import argparse
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import Request, build_opener, ProxyHandler, HTTPRedirectHandler

ROOT = Path(__file__).resolve().parent
ENDPOINTS = {'status', 'radio', 'events', 'fault-history', 'logs', 'diagnostic-bundle', 'config/raw'}

class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None

class Handler(BaseHTTPRequestHandler):
    def reply(self, code, body, content_type='application/json'):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.end_headers()
        self.wfile.write(body)

    def trusted(self):
        host = self.headers.get('Host', '')
        allowed = {f'127.0.0.1:{self.server.server_port}', f'localhost:{self.server.server_port}'}
        if host not in allowed or self.headers.get('Origin', 'http://' + host) != 'http://' + host or self.headers.get('Sec-Fetch-Site') == 'cross-site':
            self.reply(403, '{"error":"local same-origin access required"}')
            return False
        return True

    def do_GET(self):
        if not self.trusted():
            return
        path = urlsplit(self.path).path
        if path in ('/', '/dashboard.html'):
            self.reply(200, (ROOT / 'dashboard.html').read_bytes(), 'text/html; charset=utf-8')
            return
        if path == '/dashboard_config.js':
            self.reply(200, (ROOT / 'dashboard_config.js').read_bytes(), 'text/javascript; charset=utf-8')
            return
        if path == '/dashboard-connection.json':
            self.reply(200, json.dumps({k: {'name': 'UNIT-' + k.upper(), 'url': '/devices/' + k, 'token': ''} for k in self.server.units}))
            return
        parts = path.strip('/').split('/')
        endpoint = '/'.join(parts[4:])
        if len(parts) < 5 or parts[0] != 'devices' or parts[1] not in self.server.units or parts[2:4] != ['api', 'v1'] or endpoint not in ENDPOINTS:
            self.reply(404, '{"error":"unknown read-only route"}')
            return
        ip, serial = self.server.units[parts[1]]
        try:
            token = (self.server.token_dir / (serial + '.token')).read_text().strip()
            req = Request(f'http://{ip}:8088/api/v1/{endpoint}', headers={'Authorization': 'Bearer ' + token})
            with build_opener(ProxyHandler({}), NoRedirect()).open(req, timeout=12) as response:
                self.reply(response.status, response.read(2 * 1024 * 1024), response.headers.get('Content-Type', 'application/json'))
        except HTTPError as exc:
            self.reply(exc.code, exc.read(65536), exc.headers.get('Content-Type', 'application/json'))
        except OSError:
            self.reply(502, '{"error":"device unavailable or local token file missing"}')
        except URLError:
            self.reply(502, '{"error":"device unreachable"}')

    def do_POST(self):
        if not self.trusted():
            return
        path = urlsplit(self.path)
        parts = path.path.strip('/').split('/')
        if len(parts) != 5 or parts[0] != 'devices' or parts[1] not in self.server.units or parts[2:] != ['api', 'v1', 'config'] or path.query != 'confirm=yes':
            self.reply(404, '{"error":"only confirmed config apply is supported"}')
            return
        host = self.headers.get('Host', '')
        if self.headers.get('Origin') != 'http://' + host or self.headers.get('X-SDR-Apply') != 'yes':
            self.reply(403, '{"error":"explicit same-origin apply required"}')
            return
        try:
            size = int(self.headers.get('Content-Length', '0'))
        except ValueError:
            size = 0
        if not 0 < size <= 4096 or self.headers.get('Transfer-Encoding'):
            self.reply(400, '{"error":"config must contain 1..4096 bytes"}')
            return
        self.connection.settimeout(10)
        body = self.rfile.read(size)
        if len(body) != size:
            self.reply(400, '{"error":"incomplete configuration"}')
            return
        ip, serial = self.server.units[parts[1]]
        try:
            token = (self.server.token_dir / (serial + '.token')).read_text().strip()
            req = Request(f'http://{ip}:8088/api/v1/config?confirm=yes', data=body,
                          headers={'Authorization': 'Bearer ' + token, 'Content-Type': 'text/plain'}, method='POST')
            with build_opener(ProxyHandler({}), NoRedirect()).open(req, timeout=65) as response:
                self.reply(response.status, response.read(65536))
        except HTTPError as exc:
            self.reply(exc.code, exc.read(65536), exc.headers.get('Content-Type', 'application/json'))
        except OSError:
            self.reply(502, '{"error":"apply result unknown; reload configuration and status before retrying"}')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=8000)
    parser.add_argument('--unit-a', default='192.168.2.17')
    parser.add_argument('--unit-b', default='192.168.2.1')
    parser.add_argument('--serial-a', default='AKRLJ24FWXM7H65X')
    parser.add_argument('--serial-b', default='3SXRLJMXS7EL5IBJ')
    parser.add_argument('--token-dir', type=Path, default=Path.home() / '.config/sdr/tokens')
    args = parser.parse_args()
    server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    server.units = {'a': (args.unit_a, args.serial_a), 'b': (args.unit_b, args.serial_b)}
    server.token_dir = args.token_dir
    print(f'Dashboard: http://127.0.0.1:{args.port}/dashboard.html', flush=True)
    server.serve_forever()
