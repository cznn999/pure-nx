import struct
def okumura(src,hi_first=0,fill=0,rinit=4078,N=4096):
    out_size=struct.unpack('>I',src[:4])[0]; out=bytearray(); i=4
    ring=bytearray([fill])*N; r=rinit
    while len(out)<out_size:
        flags=src[i]; i+=1
        for b in range(8):
            if len(out)>=out_size: break
            if (flags>>b)&1:
                c=src[i]; i+=1; out.append(c); ring[r]=c; r=(r+1)%N
            else:
                b0,b1=src[i],src[i+1]; i+=2
                pos=((b0<<4)|(b1>>4)) if hi_first else (b0|((b1&0xF0)<<4)); ln=(b1&15)+3
                for k in range(ln):
                    c=ring[(pos+k)%N]; out.append(c); ring[r]=c; r=(r+1)%N
    return bytes(out)
def entries(d):
    w=lambda o:struct.unpack('>I',d[o:o+4])[0]
    n=w(8); return [[w(0x14+i*16+j*4) for j in range(4)] for i in range(n)]
import xxhash
def containers(o):
    res={}
    i=o.find(b'\x10\x2a\x11')
    while i>=0:
        # pure-nx (2026-11-07): any alignment. The .fxc effects in the level archives hold their containers at 2-byte
        # offsets; the 4-byte rule lost about half of them (every level's per-section copies, the riders, vehicles).
        if i+12<=len(o):
            vs,ps=struct.unpack('>II',o[i+4:i+12])
            if 28<=vs<=0x40000 and 0<ps<=0x40000 and i+vs+ps<=len(o):
                blob=o[i:i+vs+ps]; h=xxhash.xxh3_64_intdigest(blob)
                res[h]=blob
        i=o.find(b'\x10\x2a\x11',i+1)
    return res
