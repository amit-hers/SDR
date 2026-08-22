#!/usr/bin/env python3
"""HDL-vs-C++ symbol diff: finds the best integer symbol alignment, then
reports first mismatch and error statistics at that alignment."""
import sys, math

def load(p):
    out=[]
    for ln in open(p):
        t=ln.split()
        if len(t)<2: continue
        i,q=(int(t[0],16),int(t[1],16))
        if i>=1<<15: i-=1<<16
        if q>=1<<15: q-=1<<16
        out.append((i,q))
    return out

a=load(sys.argv[1]); b=load(sys.argv[2])          # a=hdl, b=golden
print(f"hdl={len(a)} golden={len(b)}")

def stats(lag, n=4000):
    m=0; exact=0; near=0; se=0.0; first=None; cnt=0
    for i in range(n):
        j=i+lag
        if j<0 or j>=len(b) or i>=len(a): continue
        cnt+=1
        di=a[i][0]-b[j][0]; dq=a[i][1]-b[j][1]
        se+=di*di+dq*dq
        if di==0 and dq==0: exact+=1
        elif abs(di)<=1 and abs(dq)<=1: near+=1
        elif first is None: first=(i,j,a[i],b[j])
    rms=math.sqrt(se/cnt) if cnt else 9e9
    return cnt,exact,near,rms,first

best=None
print(f"\n{'lag':>5} {'compared':>9} {'exact':>7} {'<=1LSB':>7} {'rms':>9}")
for lag in range(-8,9):
    c,e,nr,r,_=stats(lag)
    print(f"{lag:5d} {c:9d} {e:7d} {nr:7d} {r:9.1f}")
    if best is None or r<best[1]: best=(lag,r)

lag=best[0]
c,e,nr,r,first=stats(lag)
print(f"\nbest lag = {lag}   rms = {r:.1f}")
print(f"  exact matches : {e}/{c} ({100*e/c:.1f}%)")
print(f"  within 1 LSB  : {e+nr}/{c} ({100*(e+nr)/c:.1f}%)")
if first:
    i,j,av,bv=first
    print(f"  first mismatch: hdl[{i}]={av}  golden[{j}]={bv}  delta=({av[0]-bv[0]},{av[1]-bv[1]})")
else:
    print("  no mismatch beyond 1 LSB")
