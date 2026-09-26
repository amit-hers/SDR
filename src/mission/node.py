"""Transport-independent mission router. All entry points run on one event loop."""
import collections
import secrets

from .control import AMC, ChannelManager, MissionClock, RFMonitor, finite
from .protocol import Authenticator, MAX_BODY, b64, integer, pack, unb64


class Node:
    def __init__(self, config, send, now, apply_channel=lambda _: None):
        self.config = config
        self.id = config['node_id']
        self.send_wire = send
        self.peers = config['peers']
        self.caps = config.get('capabilities', {'modulations': ['BPSK'], 'fec': ['none']})
        self.auth = Authenticator(self.id, self.peers, self.caps)
        self.clock = MissionClock()
        self.rf = RFMonitor()
        self.channel = ChannelManager(self.id, config.get('channels', [0]),
                                      config.get('home_channel', 0), self.clock, apply_channel)
        self.live = {}
        self.path_seen = {}
        self.path_rtt = {}
        self.feedbacks = {}
        self.active_path = {}
        self.advertised = {}
        self.routes = {}
        self.amc = {p: AMC(self.caps['modulations']) for p in self.peers}
        self.events = collections.deque(maxlen=128)
        self.delivered = collections.deque(maxlen=128)
        self.awareness = {}
        self.seen = collections.OrderedDict()
        self.queues = [collections.deque(), collections.deque()]
        self.counters = collections.Counter()
        self.max_queue_ms = 0
        self.max_latency_ms = None
        self.last_tick = now - 1
        self.last_hello = {}
        self.last_position = {}
        self.probes = {}
        self.tokens = config.get('video_bytes_per_second', 32000)
        self.token_time = now
        self.boot = secrets.token_hex(8)
        self.sequence = 0
        self.safe = False
        self.local_feedback = {}
        self.channel_scores = {}
        self._clock_now = now

    def event(self, name, now, **fields):
        self.events.append(dict(event=name, time=now, **fields))

    def _send_raw(self, peer, raw, path=None, mode=1, fec=0):
        if path is None:
            path = self.active_path.get(peer, 0)
        try:
            self.send_wire(peer, path, raw, mode, fec)
            self.counters['wire_tx_bytes'] += len(raw)
            return True
        except OSError:
            self.counters['send_errors'] += 1
            return False

    def control(self, peer, body, path=None):
        session = self.auth.sessions.get(peer)
        if session:
            mode, fec = 1, 0
            if body.get('kind') == 'packet':
                mode = AMC.modes.index(self.amc[peer].mode) + 1
                fec = int(self.amc[peer].fec == 'rs255223')
            return self._send_raw(peer, session.seal(body), path, mode, fec)
        return False

    def broadcast_control(self, body):
        for peer in self.live:
            self.control(peer, body)

    def _recompute(self, now):
        routes = {}
        direct_costs = {}
        for peer, last in self.live.items():
            if now - last > 3:
                continue
            path = self.active_path.get(peer, 0)
            feedback = self.feedbacks.get(peer)
            per = feedback[1]['per'] if feedback and now-feedback[0] <= 3 else 0
            link = 1 / max(.1, 1-per) + self.path_rtt.get((peer, path), .1)
            direct_costs[peer] = link
            routes[peer] = dict(next=peer, cost=link, path=[self.id, peer])
        for peer, (stamp, entries) in self.advertised.items():
            if peer not in routes or now - stamp > 3:
                continue
            link = direct_costs[peer]
            for dest, cost, path in entries:
                if (dest == self.id or self.id in path or len(path) >= 8 or
                        not path or path[0] != peer or path[-1] != dest or
                        len(path) != len(set(path))):
                    continue
                candidate = dict(next=peer, cost=cost + link, path=[self.id, *path])
                if dest not in routes or (candidate['cost'], peer) < (routes[dest]['cost'], routes[dest]['next']):
                    routes[dest] = candidate
        self.routes = routes

    def receive(self, raw, path, now):
        self._clock_now = now
        try:
            peer, body, reply = self.auth.receive(raw, now)
            if not 0 <= path < len(self.peers[peer]['paths']):
                raise ValueError('unconfigured path')
            if reply is not None:
                self._send_raw(peer, reply, path)
            if body is None:
                return
            kind = body['kind']
            if not isinstance(kind, str):
                raise ValueError('control kind must be a string')
            self.counters['wire_rx_bytes'] += len(raw)
            if kind == 'welcome':
                self.live[peer] = now
                self.path_seen[(peer, path)] = now
                self.event('PEER_AUTHENTICATED', now, peer=peer)
            elif kind == 'ping':
                nonce = body['nonce']
                if not isinstance(nonce, str) or len(nonce) != 16:
                    raise ValueError('ping nonce')
                self.control(peer, dict(kind='pong', nonce=nonce), path)
            elif kind == 'pong':
                key = (peer, path, body['nonce'])
                sent = self.probes.pop(key, None)
                if sent is not None and now - sent <= 3:
                    self.path_seen[(peer, path)] = now
                    self.path_rtt[(peer, path)] = max(0, now - sent)
                    self.live[peer] = now
            elif peer not in self.live or now - self.live[peer] > 3:
                raise ValueError('bidirectional liveness required')
            elif kind == 'routes':
                entries = body['entries']
                if not isinstance(entries, list) or len(entries) > 16:
                    raise ValueError('route count')
                validated = []
                for entry in entries:
                    dest, cost, hops = entry
                    integer(dest, 1, 0xffffffff)
                    finite(cost, 0, 10000)
                    if not isinstance(hops, list) or not 1 <= len(hops) <= 8:
                        raise ValueError('path length')
                    for hop in hops:
                        integer(hop, 1, 0xffffffff)
                    validated.append((dest, cost, hops))
                self.advertised[peer] = (now, validated)
            elif kind == 'feedback':
                # Feedback measures what the remote receiver heard from us.
                self.amc[peer].update(body['sample'], self.auth.peer_caps[peer], now)
                self.feedbacks[peer] = (now, body['sample'])
            elif kind.startswith('channel_'):
                response = self.channel.receive(peer, body, now, self.live)
                if response:
                    if response['kind'] == 'channel_commit':
                        self.broadcast_control(response)
                    else:
                        self.control(peer, response)
            elif kind == 'packet':
                if not self.safe:
                    self._packet(body, peer, now)
            else:
                raise ValueError('unknown control kind')
        except (ValueError, KeyError, TypeError, IndexError, OverflowError, RecursionError):
            self.counters['rejected'] += 1
        except Exception as exc:
            # cryptography.InvalidTag does not inherit ValueError.
            from cryptography.exceptions import InvalidTag
            if isinstance(exc, InvalidTag):
                self.counters['auth_failed'] += 1
            else:
                raise

    def submit(self, destination, service, payload, now, ttl_ms=1000):
        if self.safe:
            raise ValueError('SAFE: forwarding disabled')
        integer(destination, 0, 0xffffffff)
        integer(ttl_ms, 1, 10000)
        if service not in ('data', 'video', 'safety', 'position', 'ethernet', 'command', 'telemetry'):
            raise ValueError('service')
        if destination == 0 and (service in ('data', 'video', 'ethernet') or not self.config.get('broadcast', False)):
            raise ValueError('broadcast disabled')
        if not isinstance(payload, bytes) or len(payload) > 400:
            raise ValueError('application payload limit 400 bytes; fragment above this API')
        if service in ('position', 'safety'):
            stamp = self.clock.status(now)
            if stamp['state'] == 'UNSYNCED' or stamp['uncertainty_ns'] >= ttl_ms * 1_000_000:
                raise ValueError('safety/position requires a clock within the message deadline')
        else:
            stamp = self.clock.status(now)
        self.sequence += 1
        message = dict(kind='packet', origin=self.id, dst=destination,
                       id=f'{self.boot}:{self.sequence}', hops=0, ttl=8,
                       service=service, payload=b64(payload), budget_ms=ttl_ms, remaining_ms=ttl_ms,
                       utc_ns=stamp['utc_ns'], uncertainty_ns=stamp['uncertainty_ns'])
        if len(pack(message)) > MAX_BODY:
            raise ValueError('encoded application packet too large')
        if not self._packet(message, None, now):
            raise ValueError('local packet admission failed; inspect drop counters')
        return message['id']

    def _packet(self, msg, incoming, now):
        integer(msg['origin'], 1, 0xffffffff)
        integer(msg['dst'], 0, 0xffffffff)
        integer(msg['ttl'], 1, 8)
        integer(msg['hops'], 0, 8)
        finite(msg['budget_ms'], 0, 10000)
        finite(msg['remaining_ms'], 0, msg['budget_ms'])
        if msg['hops'] + msg['ttl'] != 8:
            raise ValueError('hop accounting')
        if msg['service'] not in ('data','video','position','safety','ethernet','command','telemetry'):
            raise ValueError('service')
        if not isinstance(msg['id'], str) or len(msg['id']) > 48:
            raise ValueError('message identity')
        if msg['dst'] == 0 and (not self.config.get('broadcast', False) or msg['service'] not in ('position','safety')):
            raise ValueError('broadcast policy')
        payload = unb64(msg['payload'], 400)
        key = (msg['origin'], msg['id'])
        if key in self.seen:
            self.counters['duplicates'] += 1
            return False
        # A full cache drops NEW packets; evicting live entries would permit a
        # captured broadcast to circulate again before its hop budget expires.
        if len(self.seen) >= 4096:
            self.counters['dedup_full'] += 1
            return False
        stamp = self.clock.status(now)
        if msg['utc_ns'] is not None:
            integer(msg['utc_ns'], 1, (1 << 63) - 1)
            integer(msg['uncertainty_ns'], 0, 1_000_000_000)
        if msg['service'] in ('safety','position'):
            if stamp['state'] == 'UNSYNCED' or msg['utc_ns'] is None:
                raise ValueError('unverifiable safety age')
        if stamp['utc_ns'] is not None and msg['utc_ns'] is not None:
            uncertainty = stamp['uncertainty_ns'] + msg['uncertainty_ns']
            age_ms = (stamp['utc_ns'] - msg['utc_ns']) / 1e6
            if age_ms < -uncertainty / 1e6 or age_ms + uncertainty / 1e6 > msg['budget_ms']:
                self.counters['expired'] += 1
                return False
            self.max_latency_ms = max(self.max_latency_ms or 0, age_ms + uncertainty / 1e6)
        self.seen[key] = now + 12
        if msg['dst'] in (self.id, 0):
            if msg['service'] == 'position':
                self._position(msg['origin'], payload, now)
            if len(self.delivered) == self.delivered.maxlen:
                self.counters['delivery_overflow'] += 1
            self.delivered.append(dict(msg, payload=payload, received=now))
            self.counters['delivered'] += 1
            if msg['dst'] == self.id:
                return True
        if msg['ttl'] <= 1:
            self.counters['hop_limit'] += 1
            return False
        forwarded = dict(msg, ttl=msg['ttl'] - 1, hops=msg['hops'] + 1)
        priority = 0 if msg['service'] in ('position', 'safety', 'command', 'telemetry') else 1
        queue = self.queues[priority]
        if len(queue) >= (64 if priority == 0 else 128):
            self.counters['queue_drops'] += 1
            return False
        queue.append((now, incoming, forwarded))
        return True

    def _position(self, origin, raw, now):
        from .protocol import unpack
        p = unpack(raw)
        if set(p) != {'node_id','lat','lon','alt_m','velocity_mps','heading_deg','utc_ns','valid_ms','valid'}:
            raise ValueError('position schema')
        if p['node_id'] != origin or type(p['valid']) is not bool:
            raise ValueError('position identity/validity')
        for key, lo, hi in [('lat',-90,90), ('lon',-180,180), ('alt_m',-1000,100000),
                            ('heading_deg',0,360)]:
            finite(p[key], lo, hi)
        if not isinstance(p['velocity_mps'], list) or len(p['velocity_mps']) != 3:
            raise ValueError('velocity ENU required')
        for v in p['velocity_mps']:
            finite(v, -2000, 2000)
        integer(p['utc_ns'], 1, (1 << 63) - 1)
        integer(p['valid_ms'], 1, 10000)
        utc = self.clock.status(now)['utc_ns']
        if utc is None or p['utc_ns'] > utc + 5_000_000 or utc - p['utc_ns'] > p['valid_ms'] * 1_000_000:
            raise ValueError('stale/future position')
        old = self.awareness.get(origin)
        if old and old['utc_ns'] >= p['utc_ns']:
            raise ValueError('reordered position')
        if now - self.last_position.get(origin, -100) < .1:
            raise ValueError('position rate limit')
        if len(self.awareness) >= 128 and origin not in self.awareness:
            raise ValueError('peer table full')
        self.last_position[origin] = now
        self.awareness[origin] = dict(p, expires=now + p['valid_ms'] / 1000)

    def observe(self, sample, now):
        self.rf.update(sample, now)
        peer = sample.get('peer')
        if peer is not None:
            integer(peer, 1, 0xffffffff)
            if peer not in self.peers:
                raise ValueError('feedback peer not provisioned')
            self.local_feedback[peer] = (now, sample)

    def _drain(self, now):
        rate = self.config.get('video_bytes_per_second', 32000)
        self.tokens = min(rate, self.tokens + max(0, now - self.token_time) * rate)
        self.token_time = now
        for _ in range(16):
            queue = self.queues[0] if self.queues[0] else self.queues[1]
            if not queue:
                break
            queued, incoming, msg = queue.popleft()
            age = (now - queued) * 1000
            self.max_queue_ms = max(self.max_queue_ms, age)
            budget = min(msg['remaining_ms'], 20 if msg['service'] in ('safety','position','command','telemetry') else 500)
            if age >= budget:
                self.counters['deadline_misses'] += 1
                continue
            if msg['service'] == 'video':
                size = len(msg['payload']) * 3 // 4
                if self.tokens < size:
                    self.counters['video_limited'] += 1
                    continue
                self.tokens -= size
            if msg['dst'] == 0:
                targets = [p for p in self.live if p != incoming]
            else:
                route = self.routes.get(msg['dst'])
                targets = [route['next']] if route and route['next'] != incoming else []
            if not targets:
                self.counters['no_route'] += 1
            msg = dict(msg, remaining_ms=msg['remaining_ms']-age)
            for peer in targets:
                if self.control(peer, msg):
                    self.counters['forwarded'] += 1
                else:
                    self.counters['forward_failed'] += 1

    def tick(self, now):
        self._clock_now = now
        self.auth.expire(now)
        for key in list(self.seen):
            if self.seen[key] <= now:
                del self.seen[key]
        self.awareness = {p: v for p, v in self.awareness.items() if v['expires'] > now}
        if self.clock.status(now)['state'] == 'UNSYNCED':
            self.awareness.clear()
        self.last_position = {p: t for p, t in self.last_position.items() if now - t < 12}
        self.live = {p: t for p, t in self.live.items() if now - t <= 3}
        self.probes = {k: t for k, t in self.probes.items() if now - t <= 3}
        for peer, spec in self.peers.items():
            for_path = [i for i in range(len(spec['paths'])) if now - self.path_seen.get((peer,i), -100) <= 2]
            old = self.active_path.get(peer, 0)
            chosen = old if old in for_path else (min(for_path) if for_path else 0)
            # Sticky failover avoids flip-flopping as the primary comes back.
            self.active_path[peer] = chosen
            if chosen != old:
                self.event('LINK_FAILOVER', now, peer=peer, old=old, path=chosen)
            self.amc[peer].tick(now)
        self._recompute(now)
        if not self.safe and self.channel.state not in ('SWITCHING', 'FAULT', 'FAILED'):
            self._drain(now)
        if now - self.last_tick < .5:
            return
        self.last_tick = now
        for peer, spec in self.peers.items():
            session = self.auth.sessions.get(peer)
            if (session is None or now - session.last_rx > 3 or now - session.created > 3500):
                if now - self.last_hello.get(peer, -100) >= 3.2:
                    # Deterministic tie-break prevents simultaneous handshakes
                    # replacing opposite session directions.
                    if self.id < peer:
                        raw = self.auth.hello(peer, now)
                        for path in range(len(spec['paths'])):
                            self._send_raw(peer, raw, path)
                        self.last_hello[peer] = now
            if session:
                for path in range(len(spec['paths'])):
                    nonce = secrets.token_hex(8)
                    self.probes[(peer,path,nonce)] = now
                    self.control(peer, dict(kind='ping', nonce=nonce), path)
            if peer in self.live:
                entries = [[self.id, 0, [self.id]]]
                entries += [[d, round(r['cost'], 3), r['path']] for d, r in sorted(self.routes.items())
                            if r['next'] != peer][:15]
                while len(pack(dict(kind='routes', entries=entries))) > MAX_BODY:
                    entries.pop()
                self.control(peer, dict(kind='routes', entries=entries))
                feedback = self.local_feedback.get(peer)
                if feedback and now - feedback[0] <= 2:
                    sample = feedback[1]
                    if sample.get('per') is not None and sample.get('snr_db') is not None:
                        self.control(peer, dict(kind='feedback', sample={k: sample[k] for k in
                                     ('per','snr_db','evm','retry_rate') if k in sample}))
        probe = self.channel.tick(now)
        if probe:
            self.broadcast_control(probe)
        if self.config.get('auto_channel', False):
            extension = self.channel.renew(now)
            if extension:
                self.broadcast_control(extension)
        if (self.config.get('auto_channel', False) and self.rf.degraded and self.live and
                self.id < min(self.live) and not self.channel.txn and now >= self.channel.cooldown):
            home = self.channel_scores.get(self.channel.home)
            candidates = [(score, ch) for ch, (stamp, score) in self.channel_scores.items()
                          if home and now-home[0] <= 30 and score < home[1] and
                          ch != self.channel.home and now-stamp <= 30]
            if candidates:
                try:
                    msg = self.channel.propose(min(candidates)[1], self.live, now, secrets.token_hex(8))
                    self.broadcast_control(msg)
                except ValueError:
                    self.counters['channel_preconditions'] += 1

    def status(self, now):
        return dict(version=1, node_id=self.id, state='SAFE' if self.safe or self.channel.state == 'FAULT' else 'ACTIVE' if self.live else 'ISOLATED',
                    routes=self.routes, peers={p: dict(last_seen=t, path=self.active_path.get(p,0),
                    rtt_ms=self.path_rtt.get((p,self.active_path.get(p,0)),0)*1000,
                    modulation=self.amc[p].mode, fec=self.amc[p].fec, reason=self.amc[p].reason,
                    receiver_feedback=self.feedbacks[p][1] if p in self.feedbacks and now-self.feedbacks[p][0] <= 3 else None)
                    for p,t in self.live.items()}, counters=dict(self.counters), events=list(self.events),
                    rf=self.rf.status(now), clock=self.clock.status(now), awareness=self.awareness,
                    channel=dict(current=self.channel.current, state=self.channel.state, result=self.channel.result),
                    queues=[len(q) for q in self.queues], max_queue_ms=self.max_queue_ms,
                    max_one_way_upper_ms=self.max_latency_ms)
