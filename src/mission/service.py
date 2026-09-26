"""Linux service: UDP or SDR frame adapter, private Unix API, optional TAP."""
import argparse
import errno
import fcntl
import ipaddress
import json
import hashlib
import collections
import os
import selectors
import signal
import socket
import stat
import struct
import sys
import time
from pathlib import Path

from .control import finite
from .node import Node
from .protocol import b64, integer, pack, unb64, unpack, validate_capabilities


def load_config(path):
    with open(path, 'rb') as stream:
        st = os.fstat(stream.fileno())
        if st.st_mode & 0o077 or st.st_uid != os.geteuid():
            raise ValueError('configuration contains keys: require owner-only permissions and current owner')
        raw = stream.read(65537)
    if len(raw) > 65536:
        raise ValueError('configuration too large')
    c = unpack(raw, 65536)
    allowed = {'node_id','peers','capabilities','channels','home_channel','auto_channel',
               'broadcast','video_bytes_per_second','bind','runtime_dir','phy_socket','tap','ethernet_peer'}
    if not isinstance(c, dict) or set(c) - allowed:
        raise ValueError('unknown configuration fields')
    integer(c['node_id'], 1, 0xffffffff)
    if not isinstance(c['peers'], dict) or not 1 <= len(c['peers']) <= 15:
        raise ValueError('require 1..15 provisioned peers')
    peers = {}
    for identity, spec in c['peers'].items():
        peer = int(identity)
        integer(peer, 1, 0xffffffff)
        if str(peer) != identity or peer == c['node_id']:
            raise ValueError('unique canonical peer identity required')
        if (not isinstance(spec, dict) or set(spec) != {'keys','active_key','paths'} or
                not isinstance(spec['keys'], dict) or not isinstance(spec['active_key'], str) or
                spec['active_key'] not in spec['keys']):
            raise ValueError('peer key/path schema')
        if not 1 <= len(spec['keys']) <= 2:
            raise ValueError('at most current and next provisioning keys')
        for kid, key in spec['keys'].items():
            if not isinstance(kid, str) or not 1 <= len(kid) <= 16 or len(bytes.fromhex(key)) != 32:
                raise ValueError('256-bit key and short key ID required')
        if not isinstance(spec['paths'], list) or not 1 <= len(spec['paths']) <= 2:
            raise ValueError('require primary and optional backup')
        if len({json.dumps(p) for p in spec['paths']}) != len(spec['paths']):
            raise ValueError('primary and backup paths must differ')
        for path_spec in spec['paths']:
            if path_spec == 'rf':
                if not c.get('phy_socket'):
                    raise ValueError('rf path needs phy_socket')
            elif not isinstance(path_spec, list) or len(path_spec) != 2:
                raise ValueError('path must be rf or [IPv4, port]')
            else:
                ipaddress.IPv4Address(path_spec[0])
                integer(path_spec[1], 1, 65535)
        peers[peer] = spec
    c['peers'] = peers
    bind = c.get('bind', ['127.0.0.1', 5700])
    if not isinstance(bind, list) or len(bind) != 2:
        raise ValueError('bind must be [IPv4, port]')
    ipaddress.IPv4Address(bind[0])
    integer(bind[1], 1, 65535)
    c['bind'] = bind
    caps = c.setdefault('capabilities', {'modulations': ['BPSK'], 'fec': ['none']})
    validate_capabilities(caps)
    channels = c.setdefault('channels', [0])
    if not isinstance(channels, list) or not 1 <= len(channels) <= 16 or len(set(channels)) != len(channels):
        raise ValueError('channel list')
    for channel in channels:
        integer(channel, 0, 65535)
    integer(c.setdefault('home_channel', 0), 0, 65535)
    if c['home_channel'] not in channels:
        raise ValueError('home channel not authorized')
    for flag in ('auto_channel','broadcast'):
        if type(c.setdefault(flag, False)) is not bool:
            raise ValueError('boolean required')
    integer(c.setdefault('video_bytes_per_second', 32000), 1, 10_000_000)
    if c['auto_channel'] and not c.get('phy_socket'):
        raise ValueError('automatic channels require frame PHY adapter')
    directory = Path(c['runtime_dir'])
    if not directory.is_absolute() or len(str(directory)) > 70:
        raise ValueError('short absolute runtime_dir required')
    if 'tap' in c:
        if not isinstance(c['tap'], str) or not 1 <= len(c['tap']) <= 15 or not c['tap'].isalnum():
            raise ValueError('invalid TAP name')
        integer(c['ethernet_peer'], 1, 0xffffffff)
    return c


class Service:
    def __init__(self, config, config_path=None):
        self.config = config
        self.config_path = config_path
        self.dir = Path(config['runtime_dir'])
        self.dir.mkdir(mode=0o700, parents=False, exist_ok=True)
        st = self.dir.lstat()
        if not stat.S_ISDIR(st.st_mode) or st.st_uid != os.geteuid() or st.st_mode & 0o077:
            raise ValueError('runtime directory must be private and owned by service user')
        self.lock = open(self.dir / 'lock', 'a')
        fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        self.selector = selectors.DefaultSelector()
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.bind(tuple(config['bind']))
        self.udp.setblocking(False)
        self.selector.register(self.udp, selectors.EVENT_READ, 'udp')
        self.api = self._unix('api.sock')
        self.selector.register(self.api, selectors.EVENT_READ, 'api')
        self.rf = None
        self.phy_ready = False
        self.phy_channel = None
        if config.get('phy_socket'):
            self.rf = self._unix('frames.sock')
            self.selector.register(self.rf, selectors.EVENT_READ, 'rf')
        self.node = Node(config, self.send, time.monotonic(), self.channel)
        self.running = True
        self.last_snapshot = 0
        self.last_phy = 0
        self.last_phy_rx = 0
        self.tap = None
        self.fragments = {}
        self.tap_seq = 0
        self.tap_boot = os.urandom(8)
        self.l2_seen = collections.OrderedDict()
        if config.get('tap'):
            self.tap = os.open('/dev/net/tun', os.O_RDWR | os.O_NONBLOCK)
            fcntl.ioctl(self.tap, 0x400454ca,
                        struct.pack('16sH', config['tap'].encode(), 0x0002 | 0x1000))
            self.selector.register(self.tap, selectors.EVENT_READ, 'tap')

    def _unix(self, name):
        path = self.dir / name
        if path.exists():
            if not stat.S_ISSOCK(path.lstat().st_mode):
                raise ValueError('refusing to replace non-socket')
            path.unlink()
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        sock.bind(str(path))
        os.chmod(path, 0o600)
        sock.setblocking(False)
        return sock

    def send(self, peer, path, raw, mode=1, fec=0):
        destination = self.config['peers'][peer]['paths'][path]
        if destination == 'rf':
            if not self.phy_ready:
                raise OSError(errno.ENOTCONN, 'PHY has not acknowledged home channel')
            # Adapter header is outside the encrypted frame; it configures
            # only our local transmitter, never the receiver's interpretation.
            self.rf.sendto(b'T' + bytes([mode,fec]) + raw, self.config['phy_socket'])
        else:
            self.udp.sendto(raw, tuple(destination))

    def channel(self, channel):
        if self.rf is None or not self.phy_ready:
            raise ValueError('no ready physical channel adapter')
        self.rf.sendto(b'C' + struct.pack('!H', channel), self.config['phy_socket'])
        return False  # Wait for the adapter's actual retune acknowledgement.

    def snapshot(self, now):
        status = self.node.status(now)
        status['phy'] = None if self.rf is None else dict(
            ready=self.phy_ready, channel=self.phy_channel,
            last_rx_age_ms=(now-self.last_phy_rx)*1000 if self.last_phy_rx else None)
        return status

    def api_request(self, msg, now):
        command = msg['command']
        if command == 'status':
            return self.snapshot(now)
        if command == 'send':
            return {'id': self.node.submit(msg['destination'], msg['service'], unb64(msg['payload'],400),
                                          now, msg.get('ttl_ms',1000))}
        if command == 'receive':
            return {'messages': [dict(m, payload=b64(m['payload'])) for m in self.drain()]}
        if command == 'rf_sample':
            self.node.observe(msg['sample'], now)
        elif command == 'channel_score':
            ch = integer(msg['channel'], 0, 65535)
            if ch not in self.node.channel.channels:
                raise ValueError('unauthorized channel')
            self.node.channel_scores[ch] = (now, finite(msg['cost'],0,1e6))
        elif command == 'clock':
            self.node.clock.update(now, msg['utc_ns'], msg['uncertainty_ns'], msg['source'])
        elif command == 'reload_keys':
            if self.config_path is None or self.node.channel.txn:
                raise ValueError('key reload unavailable during channel transaction')
            fresh = load_config(self.config_path)
            def without_keys(config):
                return dict(config, peers={p: dict(paths=spec['paths']) for p,spec in config['peers'].items()})
            if without_keys(fresh) != without_keys(self.config):
                raise ValueError('key reload cannot change topology or operating profile')
            from .protocol import Authenticator
            self.node.auth = Authenticator(self.node.id, fresh['peers'], self.node.caps)
            self.config = fresh
            self.node.config = fresh
            self.node.peers = fresh['peers']
            self.node.live.clear()
            self.node.advertised.clear()
            self.node.routes.clear()
            self.node.path_seen.clear()
            self.node.probes.clear()
            self.node.last_hello.clear()
            for queue in self.node.queues:
                queue.clear()
            self.node.event('KEYS_RELOADED', now)
        elif command == 'safe':
            self.node.safe = True
            for queue in self.node.queues:
                queue.clear()
        elif command == 'resume':
            self.node.safe = False
        elif command == 'channel':
            proposal = self.node.channel.propose(msg['channel'], self.node.live, now, os.urandom(8).hex())
            self.node.broadcast_control(proposal)
        else:
            raise ValueError('unknown command')
        return {'ok': True}

    def drain(self):
        result = []
        while self.node.delivered:
            result.append(self.node.delivered.popleft())
        return result

    def ethernet_tx(self, frame, now):
        if len(frame) < 14 or len(frame) > 1518:
            self.node.counters['ethernet_invalid'] += 1
            return
        digest = hashlib.sha256(frame).digest()
        if now - self.l2_seen.get(digest, -100) < 2:
            self.node.counters['l2_loop_suppressed'] += 1
            return
        self.tap_seq = (self.tap_seq + 1) & 0xffffffff
        pieces = [frame[i:i+370] for i in range(0,len(frame),370)]
        for index, piece in enumerate(pieces):
            payload = struct.pack('!8sIBB', self.tap_boot, self.tap_seq, index, len(pieces)) + piece
            self.node.submit(self.config['ethernet_peer'], 'ethernet', payload, now)

    def ethernet_rx(self, now):
        retained = []
        while self.node.delivered:
            msg = self.node.delivered.popleft()
            if msg['service'] != 'ethernet':
                retained.append(msg)
                continue
            raw = msg['payload']
            if len(raw) < 15 or msg['origin'] != self.config['ethernet_peer']:
                self.node.counters['ethernet_invalid'] += 1
                continue
            boot, seq, index, count = struct.unpack('!8sIBB', raw[:14])
            if not 1 <= count <= 5 or index >= count:
                continue
            key = (boot,seq)
            if key not in self.fragments:
                if len(self.fragments) >= 64:
                    self.node.counters['reassembly_full'] += 1
                    continue
                self.fragments[key] = (now, count, {})
            stamp, expected, pieces = self.fragments[key]
            if count != expected:
                continue
            pieces[index] = raw[14:]
            if len(pieces) == count:
                frame = b''.join(pieces[i] for i in range(count))
                if 14 <= len(frame) <= 1518:
                    try:
                        os.write(self.tap, frame)
                        self.l2_seen[hashlib.sha256(frame).digest()] = now
                        while len(self.l2_seen) > 4096:
                            self.l2_seen.popitem(last=False)
                    except BlockingIOError:
                        self.node.counters['tap_dropped'] += 1
                del self.fragments[key]
        self.node.delivered.extend(retained)
        self.fragments = {k:v for k,v in self.fragments.items() if now-v[0] < 1}

    def run(self):
        try:
            while self.running:
                now = time.monotonic()
                for key, _ in self.selector.select(.01):
                    try:
                        if key.data == 'tap':
                            self.ethernet_tx(os.read(self.tap, 2048), now)
                            continue
                        raw, address = key.fileobj.recvfrom(65536)
                        if key.data == 'api':
                            try:
                                reply = self.api_request(unpack(raw), now)
                                # Status is an API document, not a radio frame.
                                encoded = pack(reply)
                                if len(encoded) > 60000:
                                    encoded = pack({'error':'snapshot too large; read status.json'})
                            except (ValueError,KeyError,TypeError,IndexError):
                                encoded = pack({'error':'invalid request or failed precondition'})
                            if address:
                                self.api.sendto(encoded, address)
                        elif key.data == 'udp':
                            sender = unpack(raw).get('src')
                            spec = self.config['peers'].get(sender)
                            if spec:
                                for path, dest in enumerate(spec['paths']):
                                    if dest != 'rf' and tuple(dest) == address:
                                        self.node.receive(raw, path, now)
                                        break
                        elif key.data == 'rf' and address == self.config['phy_socket']:
                            self.last_phy_rx = now
                            if raw[:1] == b'R':
                                body = raw[1:]
                                sender = unpack(body).get('src')
                                spec = self.config['peers'].get(sender)
                                if spec and 'rf' in spec['paths']:
                                    self.node.receive(body, spec['paths'].index('rf'), now)
                            elif raw[:1] == b'S':
                                self.node.observe(unpack(raw[1:]), now)
                            elif raw[:1] == b'A':
                                channel_text, identity_text = raw[1:].split(b':')
                                channel = int(channel_text)
                                if int(identity_text) != self.node.id:
                                    self.phy_ready = False
                                    self.node.safe = True
                                    self.node.event('PHY_IDENTITY_MISMATCH', now)
                                    continue
                                self.phy_channel = channel
                                self.node.channel.applied(channel)
                                if channel == self.node.channel.home:
                                    self.phy_ready = True
                            elif raw[:1] == b'E':
                                self.node.event('PHY_ERROR', now, detail=raw[1:160].decode(errors='replace'))
                                self.node.safe = True
                    except (ValueError,KeyError,TypeError,IndexError,RecursionError):
                        self.node.counters['input_rejected'] += 1
                    except OSError as exc:
                        if exc.errno not in (errno.EAGAIN,errno.ENOENT,errno.ECONNREFUSED,errno.ENOBUFS):
                            raise
                        self.node.counters['io_errors'] += 1
                self.node.tick(now)
                if self.rf and now - self.last_phy >= .5:
                    if self.phy_ready and now-self.last_phy_rx > 2:
                        self.phy_ready = False
                        self.node.event('PHY_STALE', now)
                    try:
                        if not self.phy_ready:
                            self.rf.sendto(b'C' + struct.pack('!H', self.node.channel.home), self.config['phy_socket'])
                        self.rf.sendto(b'H', self.config['phy_socket'])
                    except OSError:
                        self.node.counters['phy_unavailable'] += 1
                    self.last_phy = now
                if self.tap is not None:
                    self.ethernet_rx(now)
                if now - self.last_snapshot >= 1:
                    tmp = self.dir / 'status.tmp'
                    tmp.write_bytes(pack(self.snapshot(now)))
                    os.replace(tmp, self.dir / 'status.json')
                    self.last_snapshot = now
        finally:
            self.selector.close()
            for sock in (self.udp,self.api,self.rf):
                if sock:
                    sock.close()
            if self.tap is not None:
                os.close(self.tap)
            for name in ('api.sock','frames.sock'):
                (self.dir / name).unlink(missing_ok=True)
            self.lock.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', required=True)
    parser.add_argument('--check-config', action='store_true')
    args = parser.parse_args()
    try:
        config = load_config(args.config)
        if args.check_config:
            print('mission configuration valid')
            return 0
        os.umask(0o077)
        service = Service(config, args.config)
        def stop(*_):
            service.running = False
        signal.signal(signal.SIGTERM, stop)
        signal.signal(signal.SIGINT, stop)
        service.run()
        return 0
    except (ValueError, OSError, KeyError, TypeError) as exc:
        print(f'sdr-mission: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
