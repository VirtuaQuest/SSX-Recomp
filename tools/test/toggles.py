# Usage: toggles.py <log> <frame> [...] — with AR_SHADOW_LOG: far cascade draws (shader, vertex buffer, index offset,
# count) present in a frame but in neither neighbour ('+'), or in both neighbours but not the frame ('-').
import re, sys
from collections import defaultdict
far = defaultdict(set)
cascade = 0
for l in open(sys.argv[1], errors='replace'):
    m = re.search(r': SR (\d+) (\w+)', l)
    if m:
        cascade = 0 if m.group(2) == '10C22000' else cascade + 1
        continue
    m = re.search(r': S (\d+) (\w+) (\w+) (-?\d+) (\d+) ', l)
    if m and cascade == 0:
        far[int(m.group(1))].add(m.groups()[1:])
for f in map(int, sys.argv[2:]):
    a, b, c = far[f - 1], far[f], far[f + 1]
    print(f, 'draws', len(a), len(b), len(c))
    for k in sorted(b - a - c):
        print('  +', *k)
    for k in sorted((a & c) - b):
        print('  -', *k)
