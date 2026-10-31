# Usage: pxdrop.py <log> — with AR_SHADOW_PIXELS: far cascade vertex buffers whose draws write no samples in a frame
# while they write many in both neighbours, with the vertex data records (v: size, upload offset, guest and upload
# hashes; then hashes of the uploaded indices, vertex and system constants, and which changed before the GPU was
# done) of the frame and its neighbours.
import re, sys
from collections import defaultdict
total = defaultdict(lambda: defaultdict(int))
records = defaultdict(lambda: defaultdict(list))
for l in open(sys.argv[1], errors='replace'):
    m = re.search(r'PX (.*)', l.rstrip())
    if m:
        t = m.group(1).split()
        if t[1] != '0':
            continue
        f, vb = int(t[0]), t[3]
        total[f][vb] += int(t[-1])
        records[f][vb].append(tuple(t[4:]))
events = 0
for f in sorted(total):
    for vb, n in total[f].items():
        a, c = total.get(f - 1, {}).get(vb, 0), total.get(f + 1, {}).get(vb, 0)
        if n * 10 < min(a, c) and min(a, c) > 2000:
            events += 1
            print(f'frame {f} vb {vb}: samples {a} {n} {c}')
            for g in (f - 1, f, f + 1):
                for r in records[g][vb][:3]:
                    print('   ', g, *r)
print('events', events)
