#!/usr/bin/env python3
"""Accelerated deterministic network qualification; never labels simulation as RF."""
import argparse
import collections
import json
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
from test_mission import Network

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--seconds',type=int,default=120)
parser.add_argument('--output',type=Path)
args=parser.parse_args()
if not 60<=args.seconds<=86400:parser.error('duration 60..86400 simulated seconds')
net=Network([(1,2),(2,4),(1,3),(3,4)])
net.capture=collections.deque(maxlen=10000)
net.run(5)
assert net.nodes[1].routes[4]['path'][0]==1
sent=received=0
last_received=0
max_queue=0
seen=set()
for step in range(args.seconds*100):
    # Move one branch out of range, restore it, then reboot another relay.
    if step==1000:net.edges.remove((2,4))
    if step==2000:net.edges.add((2,4))
    if step==3000:net.nodes[3]=net.create(3)
    if step%10==0:
        sent+=1
        net.nodes[1].submit(4,'data',str(sent).encode(),net.now,ttl_ms=1000)
    if step%100==0:
        degraded=1000<=step<1500
        net.nodes[1].observe(dict(per=.3 if degraded else 0,snr_db=3 if degraded else 25),net.now)
    net.step(.01)
    while net.nodes[4].delivered:
        packet=net.nodes[4].delivered.popleft()
        identity=(packet['origin'],packet['id'])
        assert identity not in seen,'duplicate delivered'
        seen.add(identity);received+=1;last_received=step
    for node in net.nodes.values():
        assert len(node.seen)<=4096 and len(node.queues[0])<=64 and len(node.queues[1])<=128
        max_queue=max(max_queue,sum(map(len,node.queues)))
net.run(1)
while net.nodes[4].delivered:
    packet=net.nodes[4].delivered.popleft()
    identity=(packet['origin'],packet['id'])
    assert identity not in seen
    seen.add(identity);received+=1
assert received>0 and sent>0
assert received/sent>=.90, 'delivery below predeclared 90% including fault intervals'
assert last_received>args.seconds*100-100, 'stimulus/reception stopped before end'
assert net.nodes[1].routes[4]['path'][-1]==4, 'route did not recover'
result=dict(environment='accelerated_simulation',seconds=args.seconds,nodes=4,
            sent=sent,received=received,delivery_fraction=received/sent,max_queue=max_queue,
            faults=['mobility/link loss','RF degradation','relay reboot'],result='PASS',
            hardware_qualified=False)
output=json.dumps(result,indent=2)+'\n'
if args.output:args.output.write_text(output)
print(output,end='')
