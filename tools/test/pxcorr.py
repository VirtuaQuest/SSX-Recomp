# Usage: pxcorr.py <log> <screenshots> <x0> <y0> <x1> <y1> — with AR_SHADOW_PIXELS: per frame, whether the far cascade
# dump region is bright (a caster there), and the far cascade draws whose sample counts (PX lines) track it.
import re, sys, glob
import numpy as np
from PIL import Image
from collections import defaultdict
x0, y0, x1, y1 = map(int, sys.argv[3:7])
level = {}
for f in glob.glob(sys.argv[2] + '/resolve_*_0F801000.bmp'):
    n = int(re.search(r'resolve_(\d+)', f).group(1))
    level[n] = np.asarray(Image.open(f).convert('L'))[y0:y1, x0:x1].mean()
px = defaultdict(dict)
for l in open(sys.argv[1], errors='replace'):
    m = re.search(r'PX (\d+) 0 (\w+) (\w+) (-?\d+) (\d+)(?: v .*)? (\d+)$', l.rstrip())
    if m:
        px[int(m.group(1))][m.group(2, 3, 4, 5)] = int(m.group(6))
frames = sorted(f for f in level if f in px)
L = np.array([level[f] for f in frames])
print('region level per frame:', ' '.join(f'{f}:{level[f]:.0f}' for f in frames[:400:1]))
keys = set().union(*[px[f].keys() for f in frames])
res = []
for k in keys:
    v = np.array([px[f].get(k, -1) for f in frames], float)
    if v.std() == 0 or L.std() == 0:
        continue
    res.append((abs(np.corrcoef(v, L)[0, 1]), k, int((v < 0).sum()), int(v.min()), int(v.max())))
res.sort(reverse=True)
for r in res[:12]:
    print(r)
