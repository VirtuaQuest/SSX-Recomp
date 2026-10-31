# Usage: markcmp.py <log> <frame> [<frame> ...] — with AR_SHADOW_LOG and AR_MARK_DRAWS: shadow map draws (shader,
# vertex buffer, index offset, count, call chain marker) whose count at a frame differs from both neighbours while the
# neighbours agree, with the call chains (MK lines) of those markers. MARKCMP_COARSE=1 leaves out the vertex buffer
# and index offset (dynamic buffers move every frame).
import os, re, sys
from collections import Counter, defaultdict

frames = defaultdict(Counter)
objects = defaultdict(set)
chains = {}
last = None
cascade = 0  # Shadow map resolves seen in the frame (SR lines): 0 far, 1 middle, 2 near.
for l in open(sys.argv[1], errors='replace'):
    m = re.search(r': SR (\d+) (\w+)', l)
    if m:
        cascade = {'0F801000': 1, '0F401000': 2, '0F001000': 3}.get(m.group(2), 0) if m.group(2) != '10C22000' else 0
        continue
    m = re.search(r': S (\d+) (\w+) (\w+) (-?\d+) (\d+) ', l)
    if m:
        last = m.groups()
        if not os.environ.get('MARKCMP_NO_MARKERS'):
            continue
        l = ': IB 0 x marker - -'
    m = re.search(r': IB (\d+) .* marker (\w+) (\w+)', l)
    if m and last:
        key = (cascade, last[1], last[2], last[3], last[4], m.group(2))
        if os.environ.get('MARKCMP_COARSE'):
            key = (cascade, last[1], '-', '-', last[4], m.group(2))
        frames[int(last[0])][key] += 1
        objects[(int(last[0]), key)].add(m.group(3))
        last = None
        continue
    m = re.search(r'MK (\w+):(.*)', l)
    if m:
        chains[m.group(1)] = m.group(2).strip()

used = set()
for target in map(int, sys.argv[2:]):
    a, b, c = frames[target - 1], frames[target], frames[target + 1]
    print(f'frame {target}: {sum(b.values())} draws (neighbours {sum(a.values())}, {sum(c.values())})')
    for key in sorted(set(a) | set(b) | set(c)):
        if a[key] == c[key] and b[key] != a[key]:
            print(f'  cascade {key[0]} {key[1]} vb {key[2]} off {key[3]} n {key[4]} mark {key[5]}: {a[key]} -> {b[key]} -> {c[key]}',
                  'objects', sorted(objects[(target, key)] | objects[(target - 1, key)]))
            used.add(key[5])
for marker in sorted(used):
    print(marker, chains.get(marker, '?'))
