#!/usr/bin/env python3
import copy
import json
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'src'))
from mission.control import AMC, ChannelManager, MissionClock, RFMonitor
from mission.node import Node
from mission.protocol import Authenticator, ReplayWindow, pack, unpack, b64
from mission.service import load_config

KEY = '82' * 32
CAPS = {'modulations':['BPSK','QPSK','16QAM','64QAM'], 'fec':['none','rs255223']}


def config(node, peers):
    return dict(node_id=node, peers={p:dict(keys={'k1':KEY}, active_key='k1',
                paths=[['127.0.0.1',5700+p],['127.0.0.1',6700+p]]) for p in peers},
                capabilities=CAPS, broadcast=True, channels=[0,1], home_channel=0)


class Network:
    def __init__(self, edges):
        self.edges = {tuple(sorted(e)) for e in edges}
        self.down = set()
        self.queue = []
        self.capture = []
        self.now = 0.
        ids = sorted(set(p for e in edges for p in e))
        self.configs = {i:config(i,[p for p in ids if p!=i]) for i in ids}
        self.nodes = {i:self.create(i) for i in ids}
        assert len(ids) >= 2 and len(set(ids)) == len(ids)

    def create(self, identity):
        def send(peer,path,raw,mode,fec):
            self.capture.append((identity,peer,path,raw))
            if tuple(sorted((identity,peer))) in self.edges and (identity,peer,path) not in self.down:
                self.queue.append((identity,peer,path,raw))
        return Node(self.configs[identity],send,self.now)

    def step(self, seconds=.1):
        self.now += seconds
        for node in self.nodes.values():
            node.clock.update(self.now, 1_000_000_000_000+int(self.now*1e9), 1000, 'PTP')
            node.tick(self.now)
        count=0
        while self.queue:
            batch,self.queue=self.queue,[]
            for src,dst,path,raw in batch:
                if dst in self.nodes:
                    self.nodes[dst].receive(raw,path,self.now)
                count+=1
                assert count<10000, 'message loop'

    def run(self, seconds):
        for _ in range(round(seconds/.1)):
            self.step()


class SecurityTests(unittest.TestCase):
    def pair(self):
        a=Authenticator(1,config(1,[2])['peers'],CAPS)
        b=Authenticator(2,config(2,[1])['peers'],CAPS)
        hello=a.hello(2,0)
        _,_,challenge=b.receive(hello,0)
        _,_,confirm=a.receive(challenge,0)
        _,_,welcome=b.receive(confirm,0)
        a.receive(welcome,0)
        return a,b,hello,challenge,confirm

    def test_auth_encrypt_replay_tampering(self):
        a,b,*_=self.pair()
        raw=a.sessions[2].seal({'kind':'packet','value':'secret'})
        self.assertNotIn(b'secret',raw)
        self.assertEqual(b.receive(raw,1)[1]['value'],'secret')
        with self.assertRaises(ValueError): b.receive(raw,1)
        fresh=a.sessions[2].seal({'kind':'packet','value':'next'})
        bad=unpack(fresh);bad['seq']+=10
        with self.assertRaises(Exception): b.receive(pack(bad),1)
        self.assertEqual(b.receive(fresh,1)[1]['value'],'next')

    def test_replay_handshake_after_reboot(self):
        a,b,hello,challenge,confirm=self.pair()
        old=a.sessions[2].seal({'kind':'packet'})
        reboot=Authenticator(2,config(2,[1])['peers'],CAPS)
        reboot.receive(hello,1)
        with self.assertRaises((ValueError,KeyError)): reboot.receive(confirm,1)
        with self.assertRaises((ValueError,KeyError)): reboot.receive(old,1)
        self.assertFalse(reboot.sessions)

    def test_key_rejection_rotation(self):
        a,b,*_=self.pair()
        altered=config(2,[1])['peers'];altered[1]['keys']['k1']='11'*32
        wrong=Authenticator(2,altered,CAPS)
        with self.assertRaises(ValueError):wrong.receive(a.hello(2,2),2)
        old=a.sessions[2].seal({'kind':'packet'})
        peers_a=config(1,[2])['peers'];peers_b=config(2,[1])['peers']
        for peers in (peers_a,peers_b):
            spec=next(iter(peers.values()));spec['keys']={'k2':'44'*32};spec['active_key']='k2'
        rotated=Authenticator(2,peers_b,CAPS)
        with self.assertRaises(KeyError):rotated.receive(old,3)
        new=Authenticator(1,peers_a,CAPS)
        _,_,reply=rotated.receive(new.hello(2,3),3)
        _,_,confirm=new.receive(reply,3)
        rotated.receive(confirm,3)
        self.assertIn(1,rotated.sessions)

    def test_window_reordering_and_bounds(self):
        w=ReplayWindow()
        for seq in (4,2,3,200):w.commit(seq)
        self.assertFalse(w.allowed(4))
        self.assertTrue(w.allowed(199))
        with self.assertRaises(ValueError):w.allowed(-1)
        with self.assertRaises(ValueError):w.allowed(True)

    def test_json_duplicate_and_nonfinite(self):
        for raw in (b'{"a":1,"a":2}',b'{"a":NaN}',b'[]',b'x'*1515):
            with self.assertRaises(ValueError):unpack(raw)


class RoutingTests(unittest.TestCase):
    def test_link_cost_uses_receiver_loss(self):
        net=Network([(1,2),(2,4),(1,3),(3,4)]);net.run(5)
        self.assertEqual(net.nodes[1].routes[4]['next'],2)
        net.nodes[2].control(1,dict(kind='feedback',sample=dict(per=.8,snr_db=3,evm=.8)))
        net.run(.2)
        self.assertEqual(net.nodes[1].routes[4]['next'],3)


    def test_three_node_relay_isolation_and_loss(self):
        net=Network([(1,2),(2,3)])
        net.run(5)
        self.assertNotIn(3,net.nodes[1].live)
        self.assertEqual(net.nodes[1].routes[3]['path'],[1,2,3])
        net.nodes[1].submit(3,'data',b'hello',net.now)
        net.run(.5)
        self.assertEqual(net.nodes[3].delivered[-1]['payload'],b'hello')
        self.assertEqual(net.nodes[3].delivered[-1]['hops'],2)
        net.edges.remove((2,3));net.run(5)
        self.assertNotIn(3,net.nodes[1].routes)
        net.edges.add((2,3));net.run(6)
        self.assertIn(3,net.nodes[1].routes)

    def test_alternate_route_and_reboot(self):
        net=Network([(1,2),(2,4),(1,3),(3,4)])
        net.run(5)
        next_hop=net.nodes[1].routes[4]['next']
        net.edges.remove(tuple(sorted((next_hop,4))))
        net.run(5)
        self.assertNotEqual(net.nodes[1].routes[4]['next'],next_hop)
        net.nodes[4]=net.create(4)
        net.run(8)
        net.nodes[1].submit(4,'data',b'after reboot',net.now)
        net.run(.5)
        self.assertEqual(net.nodes[4].delivered[-1]['payload'],b'after reboot')

    def test_duplicate_broadcast_loop_and_safe(self):
        net=Network([(1,2),(2,3),(1,3)])
        net.run(5)
        net.nodes[1].submit(0,'safety',b'obstacle',net.now,1000)
        for _ in range(10):net.step(.005)
        self.assertEqual(len(net.nodes[3].delivered),1)
        net.nodes[1].safe=True
        with self.assertRaises(ValueError):net.nodes[1].submit(2,'data',b'x',net.now)

    def test_path_failover_no_duplicate_delivery(self):
        net=Network([(1,2)])
        net.run(4)
        net.down.update({(1,2,0),(2,1,0)})
        net.run(3)
        self.assertEqual(net.nodes[1].active_path[2],1)
        net.nodes[1].submit(2,'data',b'backup',net.now)
        net.run(.2)
        self.assertEqual([m['payload'] for m in net.nodes[2].delivered],[b'backup'])
        self.assertTrue(any(e['event']=='LINK_FAILOVER' for e in net.nodes[1].events))

    def test_priority_deadline_congestion_and_position(self):
        net=Network([(1,2)])
        net.run(4)
        for i in range(200):
            try:net.nodes[1].submit(2,'video',b'x'*100,net.now)
            except ValueError:pass
        self.assertGreater(net.nodes[1].counters['queue_drops'],0)
        net.nodes[1].submit(2,'safety',b'urgent',net.now,1000)
        net.step(.005)
        self.assertEqual(net.nodes[2].delivered[0]['payload'],b'urgent')
        net.nodes[1].submit(2,'safety',b'too late',net.now,1000)
        net.step(.03)
        self.assertGreater(net.nodes[1].counters['deadline_misses'],0)
        position=dict(node_id=1,lat=32.,lon=34.,alt_m=10,velocity_mps=[0,0,0],heading_deg=0,
                      utc_ns=net.nodes[1].clock.status(net.now)['utc_ns'],valid_ms=1000,valid=True)
        net.nodes[1].submit(2,'position',pack(position),net.now)
        net.step(.005)
        self.assertIn(1,net.nodes[2].awareness)
        net.run(2)
        self.assertNotIn(1,net.nodes[2].awareness)

    def test_malformed_and_bounded_state(self):
        net=Network([(1,2)])
        net.run(4)
        before=net.nodes[1].counters['rejected']
        for raw in [b'garbage',b'{}',b'{"v":1,"src":3}',b'x'*1600]:
            net.nodes[1].receive(raw,0,net.now)
        self.assertEqual(net.nodes[1].counters['rejected']-before,4)
        self.assertLessEqual(len(net.nodes[1].probes),24)


class ControlTests(unittest.TestCase):
    def test_motion_correlated_rf_history(self):
        rf=RFMonitor()
        for n in range(10):
            rf.update(dict(per=n/10,snr_db=20-n,position=[32+n*.001,34]),n)
        status=rf.status(10)
        self.assertGreater(status['displacement_m'],900)
        self.assertGreater(status['per_displacement_correlation'],.99)


    def test_rf_evidence_and_recovery(self):
        rf=RFMonitor(10)
        good=dict(per=0,snr_db=25,noise_dbm=-100,occupancy=.2)
        rf.update(good,0)
        bad=dict(per=.5,snr_db=2,noise_dbm=-80,occupancy=.9)
        rf.update(bad,1);rf.update(bad,2)
        self.assertEqual([e['event'] for e in rf.events],['CHANNEL_DEGRADED','RF_INTERFERENCE'])
        for t in (3,4,5):rf.update(good,t)
        self.assertEqual(rf.events[-1]['event'],'RF_RECOVERED')
        for t in range(6,30):rf.update(good,t)
        self.assertEqual(len(rf.history),10)
        self.assertTrue(rf.status(100)['stale'])
        with self.assertRaises(ValueError):rf.update({'per':float('nan')},101)

    def test_amc_sustained_upgrade_fast_fallback_stale_and_capability(self):
        amc=AMC(CAPS['modulations'])
        good=dict(per=0,snr_db=30,evm=.03,retry_rate=0)
        for i in range(4):amc.update(good,CAPS,i)
        self.assertEqual(amc.mode,'BPSK')
        amc.update(good,CAPS,4)
        self.assertEqual(amc.mode,'QPSK')
        for i in range(5,15):amc.update(good,CAPS,i)
        self.assertEqual(amc.mode,'64QAM')
        amc.update(dict(good,per=.5),CAPS,15)
        self.assertEqual(amc.mode,'16QAM')
        self.assertEqual(amc.fec,'rs255223')
        amc.tick(19)
        self.assertEqual(amc.mode,'BPSK')
        for i in range(20):amc.update(good,{'modulations':['BPSK'],'fec':['none']},20+i)
        self.assertEqual(amc.mode,'BPSK')

    def test_clock_holdover_loss_and_monotonic(self):
        clock=MissionClock()
        self.assertEqual(clock.status(0)['state'],'UNSYNCED')
        clock.update(1,1000000000000,1000,'PTP')
        self.assertEqual(clock.status(2)['utc_ns'],1001000000000)
        self.assertEqual(clock.status(4)['state'],'HOLDOVER')
        self.assertEqual(clock.status(12)['state'],'UNSYNCED')
        self.assertEqual(clock.status(0)['state'],'UNSYNCED')

    def test_channel_agreement_acquisition_timeout_and_lost_commit(self):
        clocks=[MissionClock(),MissionClock()]
        applied=[[],[]]
        a=ChannelManager(1,[0,1],0,clocks[0],applied[0].append)
        b=ChannelManager(2,[0,1],0,clocks[1],applied[1].append)
        for c in clocks:c.update(0,1_000_000_000_000,1000,'PTP')
        msg=a.propose(1,[2],0,'trial')
        ready=b.receive(1,msg,0,[1]);self.assertIsNotNone(ready)
        commit=a.receive(2,ready,0,[2]);self.assertIsNotNone(commit)
        b.receive(1,commit,0,[1])
        for c in clocks:c.update(2.1,1_002_100_000_000,1000,'PTP')
        pa=a.tick(2.1);pb=b.tick(2.1)
        a.receive(2,pb,2.1,[2]);b.receive(1,pa,2.1,[1])
        for c in clocks:c.update(12.1,1_012_100_000_000,1000,'PTP')
        a.tick(12.1);b.tick(12.1)
        self.assertEqual(applied,[[1,0],[1,0]])
        self.assertEqual(a.result,'ACQUIRED')
        # Lost COMMIT: only initiator retunes; BOTH recover on the same deadline.
        for c in clocks:c.update(20,1_020_000_000_000,1000,'PTP')
        msg=a.propose(1,[2],20,'lost')
        ready=b.receive(1,msg,20,[1]);a.receive(2,ready,20,[2])
        for c in clocks:c.update(22.1,1_022_100_000_000,1000,'PTP')
        a.tick(22.1);b.tick(22.1)
        self.assertEqual((a.current,b.current),(1,0))
        for c in clocks:c.update(32.1,1_032_100_000_000,1000,'PTP')
        a.tick(32.1);b.tick(32.1)
        self.assertEqual((a.current,b.current),(0,0))
        self.assertEqual(a.result,'ROLLED_BACK')

    def test_channel_lease_renewal_requires_fresh_quorum(self):
        clocks=[MissionClock(),MissionClock()]
        a=ChannelManager(1,[0,1],0,clocks[0],lambda _:None)
        b=ChannelManager(2,[0,1],0,clocks[1],lambda _:None)
        def time_at(t):
            for clock in clocks:clock.update(t,1_000_000_000_000+int(t*1e9),1000,'PTP')
        time_at(0)
        proposal=a.propose(1,[2],0,'lease')
        ready=b.receive(1,proposal,0,[1])
        commit=a.receive(2,ready,0,[2]);b.receive(1,commit,0,[1])
        time_at(2.1);a.tick(2.1);b.tick(2.1)
        time_at(9.5)
        self.assertIsNone(a.renew(9.5))
        a.receive(2,b.tick(9.5),9.5,[2])
        extension=a.renew(9.5);self.assertIsNotNone(extension)
        b.receive(1,extension,9.5,[1])
        time_at(12.1);a.tick(12.1);b.tick(12.1)
        self.assertEqual((a.current,b.current),(1,1))
        time_at(20);a.tick(20);b.tick(20)
        self.assertEqual((a.current,b.current),(0,0))

    def test_config_keys_permissions_unknown_fields(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'config.json'
            c=config(1,[2]);c['runtime_dir']=directory+'/run'
            path.write_text(json.dumps(c));path.chmod(0o600)
            self.assertEqual(load_config(path)['node_id'],1)
            path.chmod(0o644)
            with self.assertRaises(ValueError):load_config(path)
            path.chmod(0o600);c['typo']=1;path.write_text(json.dumps(c))
            with self.assertRaises(ValueError):load_config(path)


class FailureTests(unittest.TestCase):
    def test_capability_mismatch_and_untrusted_feedback(self):
        peers=config(2,[1])['peers']
        a=Authenticator(1,config(1,[2])['peers'],CAPS)
        b=Authenticator(2,peers,CAPS)
        hello=unpack(a.hello(2,0))
        hello.pop('mac');hello['caps']={'modulations':['64QAM'],'fec':['none']}
        forged=a._signed(hello,bytes.fromhex(KEY))
        with self.assertRaises(ValueError):b.receive(forged,0)
        self.assertFalse(b.sessions)
        net=Network([(1,2)]);net.run(4)
        # An RF sample without a transmitter identity cannot select any peer's TX mode.
        net.nodes[1].observe({'per':0,'snr_db':40,'evm':.01},net.now)
        net.run(4)
        self.assertEqual(net.nodes[2].amc[1].mode,'BPSK')

    def test_no_common_intermediate_mode(self):
        amc=AMC(['BPSK','64QAM']);amc.mode='64QAM'
        amc.update(dict(per=.5,snr_db=30,evm=.01),
                   {'modulations':['BPSK','64QAM'],'fec':['none']},0)
        self.assertEqual(amc.mode,'BPSK')

    def test_channel_clock_step_and_adapter_ack(self):
        clock=MissionClock();clock.update(0,1_000_000_000_000,1000,'PTP')
        applied=[]
        def apply(channel):applied.append(channel);return False
        a=ChannelManager(1,[0,1],0,clock,apply)
        a.propose(1,[2],0,'async')
        a.receive(2,dict(kind='channel_ready',id='async'),0,[2])
        clock.update(2.1,1_002_100_000_000,1000,'PTP')
        self.assertIsNone(a.tick(2.1));self.assertEqual(a.current,0)
        a.applied(1);self.assertEqual(a.state,'TRIAL')
        # Wall time stepped backwards: monotonic deadline still forces rollback.
        clock.update(13,900_000_000_000,1000,'PTP')
        a.tick(13);self.assertEqual(a.current,0);self.assertEqual(applied,[1,0])

    def test_adapter_reboot_cannot_confirm_old_channel_trial(self):
        clock=MissionClock();clock.update(0,1_000_000_000_000,1000,'PTP')
        channel=ChannelManager(1,[0,1],0,clock,lambda _:None)
        channel.propose(1,[2],0,'reboot')
        channel.receive(2,dict(kind='channel_ready',id='reboot'),0,[2])
        clock.update(2.1,1_002_100_000_000,1000,'PTP');channel.tick(2.1)
        self.assertEqual(channel.state,'TRIAL')
        channel.applied(0)
        channel.receive(2,dict(kind='channel_probe',id='reboot',channel=1),2.2,[2])
        self.assertEqual(channel.state,'FAILED')
        self.assertFalse(channel.acquired)
        self.assertIsNone(channel.tick(2.3))

    def test_chrony_clock_preconditions(self):
        from mission.clock_source import uncertainty
        valid='ABCD1234,2,1700000000,0.001,0,0,0,0,0,0.002,0.003,1,Normal'
        self.assertEqual(uncertainty(valid),5_000_000)
        for invalid in (valid.replace('Normal','Not synchronised'),
                        valid.replace('ABCD1234','7F7F0101'),valid.replace('0.003','nan'),'garbage'):
            with self.assertRaises(ValueError):uncertainty(invalid)

    def test_authenticated_malformed_messages_do_not_crash(self):
        net=Network([(1,2)]);net.run(4)
        rng=random.Random(8271)
        bodies=[{'kind':'routes','entries':[[3,1,[2,2,3]]]},
                {'kind':'packet'}, {'kind':'feedback','sample':None},
                {'kind':'channel_propose','members':[]}, {'kind':'pong','nonce':None},
                {'kind':7}, {'kind':None}, {'kind':['packet']}]
        for _ in range(200):
            body=copy.deepcopy(rng.choice(bodies))
            raw=net.nodes[1].auth.sessions[2].seal(body)
            net.nodes[2].receive(raw,0,net.now)
        self.assertNotIn(3,net.nodes[2].routes)
        self.assertGreater(net.nodes[2].counters['rejected'],100)


class EthernetTests(unittest.TestCase):
    def test_fragment_reassembly_and_real_frame_reflection(self):
        from mission.service import Service
        from unittest.mock import patch
        import collections
        net=Network([(1,2),(2,3)]);net.run(4)
        tx=Service.__new__(Service);rx=Service.__new__(Service)
        tx.node=net.nodes[1];tx.config={'ethernet_peer':3};tx.tap_seq=0;tx.tap_boot=b'12345678'
        tx.l2_seen=collections.OrderedDict()
        rx.node=net.nodes[3];rx.config={'ethernet_peer':1};rx.fragments={};rx.tap=42
        rx.l2_seen=collections.OrderedDict();rx.tap_seq=0;rx.tap_boot=b'87654321'
        frame=bytes.fromhex('0200000000030200000000010800')+bytes(range(250))*6
        self.assertEqual(len(frame),1514)
        tx.ethernet_tx(frame,net.now);net.run(.3)
        written=[]
        with patch('mission.service.os.write',side_effect=lambda fd,data:written.append(data) or len(data)):
            rx.ethernet_rx(net.now)
        self.assertEqual(written,[frame]);self.assertFalse(rx.fragments)
        # Inject the same actual Ethernet bytes back through the local interface.
        rx.ethernet_tx(frame,net.now)
        self.assertEqual(rx.node.counters['l2_loop_suppressed'],1)
        self.assertFalse(rx.node.queues[1])


if __name__=='__main__':
    unittest.main()
