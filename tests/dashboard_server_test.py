"""Proxy authorization tests; all upstream device requests are mocked."""
import importlib.util
import io
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('dashboard_server', Path(__file__).resolve().parents[1] / 'web/serve_dashboard.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class ProxyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        Path(self.temp.name, 'test.token').write_text('fixture-token')
        self.handler = object.__new__(module.Handler)
        self.handler.server = SimpleNamespace(server_port=8000, units={'b': ('192.0.2.1', 'test')}, token_dir=Path(self.temp.name))
        self.handler.headers = {'Host': '127.0.0.1:8000', 'Origin': 'http://127.0.0.1:8000', 'X-SDR-Apply': 'yes', 'Content-Length': '4'}
        self.handler.path = '/devices/b/api/v1/config?confirm=yes'
        self.handler.rfile = io.BytesIO(b'X=1\n')
        self.handler.connection = SimpleNamespace(settimeout=lambda _: None)
        self.handler.reply = lambda code, body, content_type=None: setattr(self, 'response', (code, body))

    def test_reject_cross_origin(self):
        self.handler.headers['Origin'] = 'https://untrusted.example'
        self.handler.do_POST()
        self.assertEqual(self.response[0], 403)

    def test_require_explicit_action(self):
        del self.handler.headers['X-SDR-Apply']
        self.handler.do_POST()
        self.assertEqual(self.response[0], 403)

    def test_reject_other_actions(self):
        self.handler.path = '/devices/b/api/v1/control/restart_appliance?confirm=yes'
        self.handler.do_POST()
        self.assertEqual(self.response[0], 404)

    def test_reject_large_config(self):
        self.handler.headers['Content-Length'] = '4097'
        self.handler.do_POST()
        self.assertEqual(self.response[0], 400)

    def test_reject_rebinding(self):
        self.handler.headers = {'Host': 'attacker.example:8000'}
        self.handler.do_GET()
        self.assertEqual(self.response[0], 403)

    @patch.object(module, 'build_opener')
    def test_apply_forwards_exact_body(self, opener):
        response = opener.return_value.open.return_value.__enter__.return_value
        response.status = 200
        response.read.return_value = b'{"verified":true}'
        self.handler.do_POST()
        request = opener.return_value.open.call_args.args[0]
        self.assertEqual(request.full_url, 'http://192.0.2.1:8088/api/v1/config?confirm=yes')
        self.assertEqual(request.data, b'X=1\n')
        self.assertEqual(request.get_header('Authorization'), 'Bearer fixture-token')
        self.assertEqual(self.response, (200, b'{"verified":true}'))

    @patch.object(module, 'build_opener')
    def test_load_raw_config(self, opener):
        self.handler.path = '/devices/b/api/v1/config/raw'
        response = opener.return_value.open.return_value.__enter__.return_value
        response.status = 200
        response.read.return_value = b'{"config":"X=1\\n"}'
        self.handler.do_GET()
        self.assertTrue(opener.return_value.open.call_args.args[0].full_url.endswith('/config/raw'))
        self.assertEqual(self.response[0], 200)

    def test_metadata_has_no_token(self):
        self.handler.path = '/dashboard-connection.json'
        self.handler.do_GET()
        self.assertNotIn('fixture-token', self.response[1])

if __name__ == '__main__':
    unittest.main()
