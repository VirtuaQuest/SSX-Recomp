# Usage: drawsets.py <log> — far cascade static caster draw sets per frame; prints frames whose set differs from both neighbours.
import re, sys
lines = open(sys.argv[1], errors='replace').read().splitlines()
passes = []; cur = None; vf = None; inp = None; starts = []
for n, l in enumerate(lines):
    if 'resolve 0,0-1024,720 from 0 (pitch 1040) to 10C22000' in l: cur = {}; starts.append(n)
    elif cur is not None and 'to 0F801000' in l: passes.append(cur); cur = None
    elif 'vertex fetch 95' in l: vf = re.search(r'at (0x\w+)', l).group(1)
    elif 'inputs vertices' in l: inp = re.search(r'indices (\w+) vs constants \w+ index offset (-?\d+)', l).groups()
    elif cur is not None and 'Draw: draw prim' in l and 'pitch 1040 scissor 0,0-1024,1024' in l and ('vs 741845E2' in l or 'vs DC019066' in l):
        cur[(vf, inp[0], inp[1])] = n
for i in range(1, len(passes) - 1):
    a, b, c = set(passes[i - 1]), set(passes[i]), set(passes[i + 1])
    if b != a and b != c and a == c:
        print('frame', 2950 + i, 'extra', [passes[i][k] for k in b - a], 'missing', len(a - b))
