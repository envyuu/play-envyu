import sys
segs={}; base=0; start=None
for line in open(sys.argv[1]):
    line=line.strip()
    if not line.startswith(':'): continue
    b=bytes.fromhex(line[1:]); n,addr,t=b[0],(b[1]<<8)|b[2],b[3]; data=b[4:4+n]
    assert sum(b)&0xff==0, line
    if t==0: segs[base+addr]=data
    elif t==2: base=int.from_bytes(data,'big')<<4
    elif t==4: base=int.from_bytes(data,'big')<<16
    elif t in (3,5): start=int.from_bytes(data,'big')
    elif t==1: break
# merge contiguous ranges
rng=[]
for a in sorted(segs):
    d=segs[a]
    if rng and rng[-1][0]+len(rng[-1][1])==a: rng[-1][1].extend(d)
    else: rng.append([a,bytearray(d)])
for a,d in rng: print(f"range 0x{a:08x}-0x{a+len(d):08x} ({len(d)} bytes)")
print("start", hex(start) if start is not None else None)
lo=rng[0][0]; hi=rng[-1][0]+len(rng[-1][1])
img=bytearray(b'\xff'*(hi-lo))
for a,d in rng: img[a-lo:a-lo+len(d)]=d
open(sys.argv[2],'wb').write(img); print("wrote", hex(lo), len(img))
