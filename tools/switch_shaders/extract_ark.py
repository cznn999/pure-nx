# pure-nx: every shader container in Pure's .ark archives (Black Rock's LZSS: Okumura's, 4096 ring, zero filled,
# start 4078, LSB-first flag bits, 1 = literal; token b0 | (b1 & 0xF0) << 4, length (b1 & 15) + 3).
import os, sys, pickle
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ark import okumura, entries, containers
out = sys.argv[1]; os.makedirs(out, exist_ok=True); n = 0
for p in sys.argv[2:]:
    d = open(p, 'rb').read(); found = 0
    try: ents = entries(d)
    except Exception: print(p, 'not an archive'); continue
    for e in ents:
        s = d[e[2]:e[2]+e[3]]
        try: o = okumura(s) if e[1] else s
        except Exception: continue
        for h, blob in containers(o).items():
            kind = 'vs' if (blob[3] & 0xFF) == 0x01 else 'ps'
            fn = os.path.join(out, f'{kind}_{h:016x}.bin')
            if not os.path.exists(fn):
                open(fn, 'wb').write(blob); found += 1
    print(p, found, flush=True); n += found
print('total new', n)
