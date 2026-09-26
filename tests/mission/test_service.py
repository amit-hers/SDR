#!/usr/bin/env python3
"""Exercise actual processes, UDP sockets, private APIs and restart recovery."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'src'))
from mission.protocol import pack,b64

ROOT=Path(__file__).resolve().parents[2]


class ProcessTest(unittest.TestCase):
    def test_phy_acknowledgement_and_loss(self):
        with tempfile.TemporaryDirectory(prefix='mission-phy-') as directory:
            root=Path(directory)
            radio=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM)
            radio.bind(str(root/'phy.sock'));radio.settimeout(.1)
            port=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
            port.bind(('127.0.0.1',0));number=port.getsockname()[1];port.close()
            config=dict(node_id=1,runtime_dir=str(root/'run'),bind=['127.0.0.1',number],
                        phy_socket=str(root/'phy.sock'),
                        peers={'2':dict(keys={'key':os.urandom(32).hex()},active_key='key',paths=['rf'])})
            path=root/'config.json';path.write_text(json.dumps(config));path.chmod(0o600)
            done=threading.Event();enabled=threading.Event();transmitted=[];identity=[9]
            def adapter():
                while not done.is_set():
                    try:raw,address=radio.recvfrom(4096)
                    except socket.timeout:continue
                    if raw[:1]==b'T':transmitted.append(raw)
                    if enabled.is_set() and raw[:1] in (b'H',b'C'):
                        try:radio.sendto(f'A0:{identity[0]}'.encode(),address)
                        except OSError:pass
            worker=threading.Thread(target=adapter);worker.start()
            client=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM)
            client.bind(str(root/'client'));client.settimeout(.5)
            log=open(root/'service.log','wb')
            process=subprocess.Popen([sys.executable,str(ROOT/'scripts/sdr-mission'),'--config',str(path)],
                                     stdout=log,stderr=log)
            def status():
                client.sendto(pack({'command':'status'}),str(root/'run'/'api.sock'))
                return json.loads(client.recv(65536))
            def wait(predicate,limit=6):
                deadline=time.monotonic()+limit
                while time.monotonic()<deadline:
                    self.assertIsNone(process.poll(),(root/'service.log').read_text())
                    try:
                        if predicate():return
                    except (FileNotFoundError,ConnectionRefusedError,socket.timeout):pass
                    time.sleep(.03)
                self.fail('PHY state transition timeout')
            try:
                wait(lambda:status()['phy'] is not None)
                self.assertFalse(status()['phy']['ready'])
                self.assertFalse(transmitted)
                enabled.set();wait(lambda:status()['state']=='SAFE')
                self.assertFalse(status()['phy']['ready'])
                self.assertFalse(transmitted)
                identity[0]=1;wait(lambda:status()['phy']['ready'])
                wait(lambda:bool(transmitted))
                self.assertEqual(transmitted[0][:3],b'T\x01\x00')
                enabled.clear();wait(lambda:not status()['phy']['ready'])
                self.assertTrue(any(e['event']=='PHY_STALE' for e in status()['events']))
                count=len(transmitted);time.sleep(.7);self.assertEqual(len(transmitted),count)
            finally:
                process.terminate()
                try:process.wait(timeout=5)
                except subprocess.TimeoutExpired:process.kill();process.wait()
                done.set();worker.join(timeout=1)
                radio.close();client.close();log.close()

    def test_three_process_relay_and_restart(self):
        with tempfile.TemporaryDirectory(prefix='sdr-mission-') as directory:
            root=Path(directory)
            ports=[]
            reservations=[]
            for _ in range(3):
                sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
                sock.bind(('127.0.0.1',0));ports.append(sock.getsockname()[1]);reservations.append(sock)
            self.assertEqual(len(set(ports)),3)
            paths={i:root/str(i) for i in range(1,4)}
            configs={}
            keys={(1,2):os.urandom(32).hex(),(2,3):os.urandom(32).hex()}
            for i in range(1,4):
                peers={}
                for a,b in keys:
                    if i in (a,b):
                        j=b if i==a else a
                        peers[str(j)]=dict(keys={'k':keys[(a,b)]},active_key='k',paths=[['127.0.0.1',ports[j-1]]])
                configs[i]=root/f'{i}.json'
                configs[i].write_text(json.dumps(dict(node_id=i,peers=peers,runtime_dir=str(paths[i]),
                                        bind=['127.0.0.1',ports[i-1]])))
                configs[i].chmod(0o600)
            client=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM)
            client.bind(str(root/'client'));client.settimeout(1)
            procs={};logs={}
            def launch(i):
                logs[i]=open(root/f'{i}.log','ab')
                procs[i]=subprocess.Popen([sys.executable,str(ROOT/'scripts/sdr-mission'),'--config',str(configs[i])],
                                         stdout=logs[i],stderr=logs[i])
            def call(i,command,**fields):
                client.sendto(pack(dict(command=command,**fields)),str(paths[i]/'api.sock'))
                return json.loads(client.recv(65536))
            def wait(predicate,limit=15):
                deadline=time.monotonic()+limit
                while time.monotonic()<deadline:
                    for i,p in procs.items():
                        self.assertIsNone(p.poll(),(root/f'{i}.log').read_text())
                    try:
                        if predicate():return
                    except (FileNotFoundError,ConnectionRefusedError,socket.timeout,KeyError):pass
                    time.sleep(.05)
                self.fail('process acceptance deadline exceeded')
            try:
                for sock in reservations:sock.close()
                for i in (1,2,3):launch(i)
                wait(lambda:'3' in call(1,'status')['routes'])
                self.assertNotIn('3',call(1,'status')['peers'])
                response=call(1,'send',destination=3,service='data',payload=b64(b'real process relay'))
                self.assertIn('id',response)
                received=[]
                def delivered():
                    received.extend(call(3,'receive')['messages'])
                    return bool(received)
                wait(delivered,3)
                self.assertEqual(received[0]['payload'],b64(b'real process relay'))
                self.assertEqual(received[0]['hops'],2)
                # Relay failure actually terminates the process, not only a mocked link.
                procs[2].terminate();procs[2].wait(timeout=5);del procs[2];logs[2].close()
                wait(lambda:'3' not in call(1,'status')['routes'],8)
                launch(2)
                wait(lambda:'3' in call(1,'status')['routes'])
                new_key=os.urandom(32).hex()
                for identity,peer in ((1,2),(2,1)):
                    document=json.loads(configs[identity].read_text())
                    document['peers'][str(peer)]['keys']={'rotated':new_key}
                    document['peers'][str(peer)]['active_key']='rotated'
                    configs[identity].write_text(json.dumps(document))
                    self.assertTrue(call(identity,'reload_keys')['ok'])
                wait(lambda:'3' in call(1,'status')['routes'])
                self.assertTrue(any(e['event']=='KEYS_RELOADED' for e in call(1,'status')['events']))
                call(1,'safe')
                denied=call(1,'send',destination=3,service='data',payload=b64(b'blocked'))
                self.assertIn('error',denied)
                # Wrong identity/key cannot populate a peer or route.
                sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
                sock.sendto(b'{"v":1,"src":999,"dst":1,"t":"data"}',('127.0.0.1',ports[0]))
                sock.close()
                self.assertNotIn('999',call(1,'status')['peers'])
            finally:
                for p in procs.values():p.terminate()
                for p in procs.values():
                    try:p.wait(timeout=5)
                    except subprocess.TimeoutExpired:p.kill();p.wait()
                for log in logs.values():log.close()
                client.close()
                for sock in reservations:sock.close()


if __name__=='__main__':
    try:
        probe=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        probe.close()
    except PermissionError:
        print('SKIP: process test requires permission to open local UDP sockets')
        raise SystemExit(77)
    unittest.main()
