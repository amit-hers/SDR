#!/usr/bin/env python3
"""Generate private loopback mission profiles with unique pairwise keys."""
import argparse
import json
import os
from pathlib import Path
import secrets

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output',required=True,type=Path)
parser.add_argument('--nodes',type=int,default=3)
parser.add_argument('--base-port',type=int,default=5700)
args=parser.parse_args()
if not 2<=args.nodes<=16 or not 1024<=args.base_port<=65535-args.nodes:
    parser.error('require 2..16 nodes and valid base port')
os.umask(0o077)
args.output.mkdir(mode=0o700,parents=False,exist_ok=False)
args.output=args.output.resolve()
keys={(a,b):secrets.token_hex(32) for a in range(1,args.nodes+1) for b in range(a+1,args.nodes+1)}
for node in range(1,args.nodes+1):
    peers={}
    # A chain is deliberate: endpoints have no direct transport in the demo.
    for peer in (node-1,node+1):
        if 1<=peer<=args.nodes:
            peers[str(peer)]={'keys':{'initial':keys[tuple(sorted((node,peer)))]},
                              'active_key':'initial','paths':[['127.0.0.1',args.base_port+peer]]}
    profile={'node_id':node,'bind':['127.0.0.1',args.base_port+node],
             'runtime_dir':str(args.output/f'node{node}'),'peers':peers,
             'capabilities':{'modulations':['BPSK'],'fec':['none']},
             'channels':[0],'home_channel':0,'auto_channel':False,
             'broadcast':False,'video_bytes_per_second':32000}
    path=args.output/f'node{node}.json'
    path.write_text(json.dumps(profile,indent=2)+'\n')
    path.chmod(0o600)
print(f'Created {args.nodes} private profiles in {args.output}')
