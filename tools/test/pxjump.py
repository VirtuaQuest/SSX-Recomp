# Usage: pxjump.py <log> <frame> [...] — with AR_SHADOW_PIXELS: draws (by cascade) issued in a frame and both
# neighbours whose sample count in the frame is far from both neighbours while those agree.
import re, sys
from collections import defaultdict
px = defaultdict(dict)
for l in open(sys.argv[1], errors='replace'):
    m = re.search(r'PX (\d+) (\d+) (\w+) (\w+) (-?\d+) (\d+)(?: v .*)? (\d+)$', l.rstrip())
    if m:
        px[int(m.group(1))][m.group(2, 3, 4, 5, 6)] = int(m.group(7))
for f in map(int, sys.argv[2:]):
    a, b, c = px[f - 1], px[f], px[f + 1]
    rows = []
    for k in set(a) & set(b) & set(c):
        near = (a[k] + c[k]) / 2
        if abs(a[k] - c[k]) <= 0.25 * max(near, 50) and abs(b[k] - near) > 0.5 * max(near, 200):
            rows.append((abs(b[k] - near), k, a[k], b[k], c[k]))
    rows.sort(reverse=True)
    print(f, 'draws', len(a), len(b), len(c))
    for r in rows[:12]:
        print('  cascade', r[1][0], *r[1][1:], ':', r[2], r[3], r[4])
