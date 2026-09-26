"""Bounded, versioned pairwise authenticated sessions.

Pairwise 256-bit provisioning keys authenticate a fresh two-challenge exchange.
HKDF separates session directions; AES-GCM authenticates headers and payloads.
Sessions and counters are never restored after restart. This protocol requires
independent security review before deployment in a safety-critical system.
"""
import base64
import hashlib
import hmac
import json
import secrets
from dataclasses import dataclass

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

VERSION = 1
MAX_WIRE = 1514
MAX_BODY = 900


def pack(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False).encode()


def unpack(raw, maximum=MAX_WIRE):
    if not isinstance(raw, bytes) or len(raw) > maximum:
        raise ValueError('wire length')
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError('duplicate JSON key')
            result[key] = value
        return result
    value = json.loads(raw, object_pairs_hook=pairs,
                       parse_constant=lambda _: (_ for _ in ()).throw(ValueError('nonfinite')))
    if not isinstance(value, dict):
        raise ValueError('object required')
    return value


def integer(value, low, high):
    if type(value) is not int or not low <= value <= high:
        raise ValueError('integer outside bounds')
    return value


def b64(raw):
    return base64.b64encode(raw).decode('ascii')


def unb64(text, maximum=MAX_BODY):
    if not isinstance(text, str) or len(text) > 4 * ((maximum + 2) // 3):
        raise ValueError('base64 length')
    raw = base64.b64decode(text, validate=True)
    if len(raw) > maximum:
        raise ValueError('decoded length')
    return raw


def validate_capabilities(value):
    if not isinstance(value, dict) or set(value) != {'modulations','fec'}:
        raise ValueError('capability schema')
    for name, allowed, baseline in (('modulations', {'BPSK','QPSK','16QAM','64QAM'}, 'BPSK'),
                                     ('fec', {'none','rs255223'}, 'none')):
        items = value[name]
        if (not isinstance(items, list) or not 1 <= len(items) <= len(allowed) or
                any(not isinstance(item,str) for item in items) or
                len(items) != len(set(items)) or set(items) - allowed or baseline not in items):
            raise ValueError('incompatible capability set')
    return value


class ReplayWindow:
    def __init__(self):
        self.high = -1
        self.bits = 0

    def allowed(self, seq):
        integer(seq, 0, (1 << 64) - 1)
        delta = self.high - seq
        return delta < 0 or (delta < 128 and not self.bits & (1 << delta))

    def commit(self, seq):
        if not self.allowed(seq):
            raise ValueError('replay')
        if seq > self.high:
            shift = min(128, seq - self.high)
            self.bits = ((self.bits << shift) | 1) & ((1 << 128) - 1)
            self.high = seq
        else:
            self.bits |= 1 << (self.high - seq)


class Session:
    def __init__(self, local, peer, key, transcript, created):
        self.local, self.peer = local, peer
        salt = hashlib.sha256(pack(transcript)).digest()
        self.sid = salt[:12].hex()
        def derive(src, dst):
            return HKDF(algorithm=hashes.SHA256(), length=32, salt=salt,
                        info=pack(['sdr-mission-v1', src, dst])).derive(key)
        self.tx = AESGCM(derive(local, peer))
        self.rx = AESGCM(derive(peer, local))
        self.seq = 0
        self.window = ReplayWindow()
        self.created = created
        self.last_rx = created

    def seal(self, body):
        plain = pack(body)
        if len(plain) > MAX_BODY or self.seq >= (1 << 32):
            raise ValueError('session payload or lifetime exceeded')
        head = dict(v=VERSION, t='data', src=self.local, dst=self.peer,
                    sid=self.sid, seq=self.seq)
        cipher = self.tx.encrypt(self.seq.to_bytes(12, 'big'), plain, pack(head))
        self.seq += 1
        wire = pack(dict(head, c=b64(cipher)))
        if len(wire) > MAX_WIRE:
            raise ValueError('wire length')
        return wire

    def open(self, msg, now):
        head = {k: msg[k] for k in ('v', 't', 'src', 'dst', 'sid', 'seq')}
        if (set(msg) != set(head) | {'c'} or head['v'] != VERSION or
                head['t'] != 'data' or head['src'] != self.peer or
                head['dst'] != self.local or head['sid'] != self.sid or
                not self.window.allowed(head['seq'])):
            raise ValueError('session or replay')
        raw = self.rx.decrypt(head['seq'].to_bytes(12, 'big'),
                              unb64(msg['c'], MAX_BODY + 16), pack(head))
        body = unpack(raw)
        self.window.commit(head['seq'])  # Authentication MUST precede commit.
        self.last_rx = now
        return body


@dataclass
class Pending:
    transcript: dict
    session: Session
    expires: float


class Authenticator:
    def __init__(self, node, peers, capabilities):
        self.node = node
        self.peers = peers
        self.capabilities = validate_capabilities(capabilities)
        self.sessions = {}
        self.pending = {}
        self.hellos = {}
        self.peer_caps = {}

    def _key(self, peer, kid):
        return bytes.fromhex(self.peers[peer]['keys'][kid])

    def _signed(self, body, key):
        return pack(dict(body, mac=hmac.new(key, pack(body), hashlib.sha256).hexdigest()))

    def hello(self, peer, now):
        spec = self.peers[peer]
        body = dict(v=VERSION, t='hello', src=self.node, dst=peer,
                    kid=spec['active_key'], a=secrets.token_hex(32), caps=self.capabilities)
        self.hellos[peer] = (body, now + 3)
        return self._signed(body, self._key(peer, body['kid']))

    def receive(self, raw, now):
        msg = unpack(raw)
        peer = integer(msg.get('src'), 1, 0xffffffff)
        if peer not in self.peers or msg.get('dst') != self.node or msg.get('v') != VERSION:
            raise ValueError('unknown identity/version')
        kind = msg.get('t')
        if kind == 'data':
            pending = self.pending.get(peer)
            if pending and pending.expires >= now and msg.get('sid') == pending.session.sid:
                body = pending.session.open(msg, now)
                if body != {'kind': 'confirm'}:
                    raise ValueError('confirmation required')
                self.sessions[peer] = pending.session
                self.peer_caps[peer] = pending.transcript['initiator_caps']
                del self.pending[peer]
                return peer, None, pending.session.seal({'kind': 'welcome'})
            session = self.sessions[peer]
            if now - session.created > 3600:
                del self.sessions[peer]
                raise ValueError('expired session')
            body = session.open(msg, now)
            return peer, body, None
        mac = msg.pop('mac')
        key = self._key(peer, msg['kid'])
        if not isinstance(mac, str) or not hmac.compare_digest(
                mac, hmac.new(key, pack(msg), hashlib.sha256).hexdigest()):
            raise ValueError('authentication failed')
        if kind == 'hello':
            if set(msg) != {'v','t','src','dst','kid','a','caps'}:
                raise ValueError('hello fields')
            validate_capabilities(msg['caps'])
            if len(bytes.fromhex(msg['a'])) != 32:
                raise ValueError('challenge length')
            # One pending exchange per provisioned peer. Replayed HELLOs never
            # replace a live session; completion requires our fresh challenge.
            transcript = dict(a=msg['a'], b=secrets.token_hex(32), kid=msg['kid'],
                              initiator=peer, responder=self.node,
                              initiator_caps=msg['caps'], responder_caps=self.capabilities)
            old = self.pending.get(peer)
            if old and old.expires >= now and old.transcript['a'] == msg['a']:
                transcript = old.transcript
            session = Session(self.node, peer, key, transcript, now)
            self.pending[peer] = Pending(transcript, session, now + 3)
            reply = dict(v=VERSION, t='challenge', src=self.node, dst=peer,
                         kid=msg['kid'], transcript=transcript)
            return peer, None, self._signed(reply, key)
        if kind == 'challenge':
            hello, expiry = self.hellos[peer]
            tr = msg['transcript']
            validate_capabilities(tr['responder_caps'])
            if (now > expiry or tr['a'] != hello['a'] or tr['kid'] != hello['kid'] or
                    tr['initiator'] != self.node or tr['responder'] != peer or
                    tr['initiator_caps'] != self.capabilities or
                    len(bytes.fromhex(tr['b'])) != 32):
                raise ValueError('challenge mismatch')
            session = Session(self.node, peer, key, tr, now)
            self.sessions[peer] = session
            self.peer_caps[peer] = tr['responder_caps']
            del self.hellos[peer]
            # Node only marks the peer live after an encrypted response.
            return peer, None, session.seal({'kind': 'confirm'})
        raise ValueError('unknown handshake')

    def expire(self, now):
        self.pending = {p: v for p, v in self.pending.items() if v.expires >= now}
        self.hellos = {p: v for p, v in self.hellos.items() if v[1] >= now}
