# Usage: worldcmp.py <log> <frame> — compares each shadow draw's world/light matrix hashes at frame vs frame-1 and frame+1.
import sys, re
from collections import defaultdict
target = int(sys.argv[2])
draws = defaultdict(dict)
for l in open(sys.argv[1], errors='replace'):
    m = re.search(r': S (\d+) (\w+) (\w+) (-?\d+) (\d+) (\w+) (\w+)', l)
    if not m: continue
    fr = int(m.group(1))
    if abs(fr - target) > 1: continue
    key = (m.group(2), m.group(3), m.group(4), m.group(5))
    draws[fr].setdefault(key, []).append((m.group(6), m.group(7)))
a, b, c = draws[target - 1], draws[target], draws[target + 1]
print('draws', len(a), len(b), len(c))
world_changed = [k for k in b if k in a and k in c and a[k][0][0] == c[k][0][0] and b[k][0][0] != a[k][0][0]]
light = {x[0][1] for x in b.values()}
print('world matrix differs only in this frame:', len(world_changed), world_changed[:5])
print('light matrices in frame:', len(light))
