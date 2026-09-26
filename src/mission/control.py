"""Deterministic control policies; time is injected for fault testing."""
import math
from collections import deque


def finite(value, lo, hi):
    if type(value) not in (int, float) or not math.isfinite(value) or not lo <= value <= hi:
        raise ValueError('measurement outside bounds')
    return float(value)


class MissionClock:
    def __init__(self):
        self.sample = None

    def update(self, mono, utc_ns, uncertainty_ns, source):
        if type(utc_ns) is not int or utc_ns <= 0 or type(uncertainty_ns) is not int or uncertainty_ns < 0:
            raise ValueError('invalid clock sample')
        if source not in ('GNSS', 'PPS', 'PTP', 'NTP'):
            raise ValueError('untrusted clock source')
        self.sample = (mono, utc_ns, uncertainty_ns, source)

    def status(self, now):
        if self.sample is None:
            return dict(state='UNSYNCED', utc_ns=None, uncertainty_ns=None)
        mono, utc, uncertainty, source = self.sample
        age = now - mono
        if age < 0 or age > 10:
            return dict(state='UNSYNCED', utc_ns=None, uncertainty_ns=None, source=source)
        return dict(state='SYNCED' if age <= 2 else 'HOLDOVER',
                    utc_ns=utc + int(age * 1e9),
                    uncertainty_ns=uncertainty + int(age * 100_000), source=source)


class RFMonitor:
    def __init__(self, capacity=120):
        self.history = deque(maxlen=capacity)
        self.events = deque(maxlen=64)
        self.degraded = False
        self.bad = self.good = 0

    def update(self, sample, now):
        allowed = {'rssi_dbm': (-180, 30), 'noise_dbm': (-180, 30),
                   'per': (0, 1), 'snr_db': (-40, 100), 'evm': (0, 10),
                   'occupancy': (0, 1), 'acquisition': (0, 1), 'retry_rate': (0, 1)}
        row = {'time': now}
        for key, bounds in allowed.items():
            row[key] = None if sample.get(key) is None else finite(sample[key], *bounds)
        if sample.get('position') is not None:
            pos = sample['position']
            row['position'] = [finite(pos[0], -90, 90), finite(pos[1], -180, 180)]
        for counter in ('clipping', 'fec_corrected', 'fec_failed'):
            if counter in sample:
                if type(sample[counter]) is not int or sample[counter] < 0:
                    raise ValueError('invalid PHY counter')
                row[counter] = sample[counter]
        previous = list(self.history)
        self.history.append(row)
        bad = ((row['per'] is not None and row['per'] > .15) or
               (row['acquisition'] is not None and row['acquisition'] < .5) or
               (row['snr_db'] is not None and row['snr_db'] < 5))
        measurable = any(row[k] is not None for k in ('per', 'snr_db', 'acquisition'))
        self.bad = self.bad + 1 if bad else 0
        self.good = self.good + 1 if measurable and not bad else 0
        noise = [r['noise_dbm'] for r in previous if r['noise_dbm'] is not None]
        interference = (bad and row['noise_dbm'] is not None and noise and
                        row['noise_dbm'] - min(noise) >= 6 and
                        row['occupancy'] is not None and row['occupancy'] > .7)
        if self.bad >= 2 and not self.degraded:
            self.degraded = True
            self.events.append(dict(event='CHANNEL_DEGRADED', evidence=row))
            if interference:
                self.events.append(dict(event='RF_INTERFERENCE', confidence='suspected', evidence=row))
        if self.good >= 3 and self.degraded:
            self.degraded = False
            self.events.append(dict(event='RF_RECOVERED', evidence=row))

    def status(self, now):
        recent = [r for r in self.history if now - r['time'] <= 30]
        values = [r['per'] for r in recent if r['per'] is not None]
        pattern = 'unknown'
        if len(values) >= 6:
            transitions = sum((a > .15) != (b > .15) for a, b in zip(values, values[1:]))
            pattern = 'intermittent' if transitions >= 3 else (
                'changing' if max(values) - min(values) > .25 else 'stationary')
        positioned = [r for r in recent if 'position' in r and r['per'] is not None]
        distance = 0.
        correlation = None
        if len(positioned) >= 6:
            lat0, lon0 = positioned[0]['position']
            distances = []
            for row in positioned:
                lat, lon = row['position']
                dlat = math.radians(lat-lat0)
                dlon = math.radians(lon-lon0)
                a = math.sin(dlat/2)**2 + math.cos(math.radians(lat0))*math.cos(math.radians(lat))*math.sin(dlon/2)**2
                distances.append(6371000 * 2 * math.asin(math.sqrt(min(1.,max(0.,a)))))
            distance = max(distances)
            errors = [r['per'] for r in positioned]
            dx = [x-sum(distances)/len(distances) for x in distances]
            dy = [y-sum(errors)/len(errors) for y in errors]
            norm = math.sqrt(sum(x*x for x in dx)*sum(y*y for y in dy))
            if distance >= 5 and norm > 0:
                correlation = sum(x*y for x,y in zip(dx,dy))/norm
        return dict(degraded=self.degraded, stale=not recent, pattern=pattern,
                    displacement_m=distance, per_displacement_correlation=correlation,
                    history=list(self.history), events=list(self.events))


class AMC:
    modes = ('BPSK', 'QPSK', '16QAM', '64QAM')
    snr = (0, 9, 16, 25)
    evm = (1., .35, .16, .07)

    def __init__(self, qualified=('BPSK', 'QPSK')):
        self.qualified = set(qualified)
        self.mode = 'BPSK'
        self.good = 0
        self.last = None
        self.reason = 'startup'
        self.fec = 'none'

    def update(self, feedback, caps, now):
        common = self.qualified.intersection(caps.get('modulations', []))
        if 'BPSK' not in common:
            raise ValueError('no robust common mode')
        self.last = now
        per = finite(feedback['per'], 0, 1)
        snr = finite(feedback['snr_db'], -40, 100)
        evm = feedback.get('evm')
        retries = finite(feedback.get('retry_rate', 0), 0, 1)
        if evm is not None:
            evm = finite(evm, 0, 10)
        cur = self.modes.index(self.mode)
        candidates = [i for i, m in enumerate(self.modes) if m in common and
                      snr >= self.snr[i] and (i < 2 or evm is not None and evm <= self.evm[i])]
        target = max(candidates, default=0)
        if per > .10 or retries > .20 or target < cur or self.mode not in common:
            limit = min(target, max(0, cur - 1))
            self.mode = self.modes[max(i for i, m in enumerate(self.modes) if m in common and i <= limit)]
            self.good = 0
            self.reason = 'receiver degradation'
        elif per < .02 and retries < .05 and target > cur:
            self.good += 1
            if self.good >= 5:
                self.mode = self.modes[min(i for i in candidates if i > cur)]
                self.good = 0
                self.reason = 'sustained receiver margin'
        else:
            self.good = 0
        self.fec = 'rs255223' if per > .02 and 'rs255223' in caps.get('fec', []) else 'none'
        return self.mode, self.fec

    def tick(self, now):
        if self.last is None or now - self.last > 3:
            self.mode, self.fec, self.good, self.reason = 'BPSK', 'none', 0, 'stale receiver feedback'


class ChannelManager:
    """Clock-scheduled, leased channel trials; all nodes return home on timeout.

    No unbounded commit: a trial has a common deadline even after acquisition.
    This deliberately trades periodic home-channel rendezvous for recovery
    under message loss, asymmetric links, or coordinator failure.
    """
    def __init__(self, node, channels, home, clock, apply):
        self.node, self.channels, self.home = node, set(channels), home
        self.current, self.clock, self.apply = home, clock, apply
        self.txn = None
        self.state = 'HOME'
        self.result = None
        self.cooldown = 0
        self.monotonic_deadline = 0

    def propose(self, channel, peers, now, nonce):
        stamp = self.clock.status(now)
        if (self.txn or now < self.cooldown or not peers or self.node > min(peers) or
                channel not in self.channels or channel == self.home or
                stamp['state'] != 'SYNCED' or stamp['uncertainty_ns'] > 5_000_000):
            raise ValueError('channel proposal preconditions')
        self.txn = dict(id=nonce, channel=channel, leader=self.node,
                        members=sorted([self.node, *peers]),
                        start=stamp['utc_ns'] + 2_000_000_000,
                        end=stamp['utc_ns'] + 12_000_000_000)
        self.monotonic_deadline = now + 12
        self.ready = {self.node}
        self.acquired = {}
        self.state = 'PROPOSING'
        return dict(kind='channel_propose', **self.txn)

    def receive(self, peer, msg, now, live_peers):
        stamp = self.clock.status(now)
        if stamp['state'] != 'SYNCED' or stamp['uncertainty_ns'] > 5_000_000:
            return None
        kind = msg['kind']
        if kind == 'channel_propose':
            members = msg['members']
            if (self.txn or now < self.cooldown or peer != min(members) or msg['leader'] != peer or
                    set(members) != {self.node, *live_peers} or
                    msg['channel'] not in self.channels or msg['channel'] == self.home or
                    not stamp['utc_ns'] + 500_000_000 <= msg['start'] <= stamp['utc_ns'] + 3_000_000_000 or
                    msg['end'] - msg['start'] != 10_000_000_000):
                return None
            self.txn = {k: msg[k] for k in ('id','channel','leader','members','start','end')}
            self.monotonic_deadline = now + (msg['end'] - stamp['utc_ns']) / 1e9
            self.state = 'PREPARED'
            self.acquired = {}
            return dict(kind='channel_ready', id=msg['id'])
        if not self.txn or msg.get('id') != self.txn['id'] or peer not in self.txn['members']:
            return None
        if kind == 'channel_ready' and self.state == 'PROPOSING':
            self.ready.add(peer)
            if self.ready == set(self.txn['members']):
                self.state = 'COMMITTED'
                return dict(kind='channel_commit', id=self.txn['id'])
        if kind == 'channel_commit' and peer == self.txn['leader'] and self.state == 'PREPARED':
            self.state = 'COMMITTED'
        if (kind == 'channel_probe' and self.state == 'TRIAL' and
                msg.get('channel') == self.txn['channel'] == self.current):
            self.acquired[peer] = now
        if kind == 'channel_extend' and peer == self.txn['leader'] and self.state == 'TRIAL':
            end = msg['end']
            if type(end) is int and self.txn['end'] < end <= stamp['utc_ns'] + 12_000_000_000:
                self.txn['end'] = end
                self.monotonic_deadline = now + (end-stamp['utc_ns'])/1e9
        return None

    def renew(self, now):
        stamp = self.clock.status(now)
        if (not self.txn or self.state != 'TRIAL' or self.txn['leader'] != self.node or
                stamp['state'] != 'SYNCED' or stamp['uncertainty_ns'] > 5_000_000):
            return None
        if self.txn['end']-stamp['utc_ns'] > 3_000_000_000:
            return None
        others = set(self.txn['members']) - {self.node}
        if any(now-self.acquired.get(peer, -100) > 1 for peer in others):
            return None
        self.txn['end'] = stamp['utc_ns']+10_000_000_000
        self.monotonic_deadline = now+10
        return dict(kind='channel_extend',id=self.txn['id'],end=self.txn['end'])

    def tick(self, now):
        if not self.txn:
            return None
        stamp = self.clock.status(now)
        utc = stamp['utc_ns']
        if utc is None or utc >= self.txn['end'] or now >= self.monotonic_deadline:
            ok = self.state == 'TRIAL' and set(self.acquired) >= set(self.txn['members']) - {self.node}
            self.result = 'ACQUIRED' if ok else 'ROLLED_BACK'
            if self.current != self.home or self.state in ('SWITCHING', 'FAULT'):
                try:
                    self.apply(self.home)
                except (OSError, ValueError):
                    self.state = 'FAULT'
                    self.result = 'ROLLBACK_FAILED'
                    return None
            self.current, self.txn, self.state = self.home, None, 'HOME'
            self.cooldown = now + 5
            return None
        if utc >= self.txn['start'] and self.state == 'COMMITTED':
            try:
                confirmed = self.apply(self.txn['channel'])
                self.state = 'SWITCHING' if confirmed is False else 'TRIAL'
                if confirmed is not False:
                    self.current = self.txn['channel']
            except (OSError, ValueError):
                self.state = 'FAILED'
                try:
                    self.apply(self.home)
                except (OSError, ValueError):
                    self.state = 'FAULT'
        if self.state == 'TRIAL':
            return dict(kind='channel_probe', id=self.txn['id'], channel=self.current)
        return None

    def applied(self, channel):
        if self.txn and self.state == 'SWITCHING' and channel == self.txn['channel']:
            self.current = channel
            self.state = 'TRIAL'
        elif channel == self.home:
            self.current = self.home
            if self.txn and self.state in ('TRIAL','SWITCHING'):
                self.state = 'FAILED'
                self.acquired.clear()
        elif self.txn and self.state == 'TRIAL' and channel == self.current:
            pass  # Repeated adapter heartbeat acknowledgement.
        else:
            raise ValueError('unexpected physical channel acknowledgement')
