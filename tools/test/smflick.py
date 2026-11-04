# Usage: smflick.py <log> — with AR_SHADOW_READBACK: frames where part of the far cascade changes for that one frame and
# returns (the flicker), from the texels written per grid cell (SM lines). No GPU waits, so it runs at full speed.
import re, sys
cells = {}
for l in open(sys.argv[1], errors='replace'):
    m = re.search(r': SM (\d+) format \d+ clear \w+ written (\d+) (.*)', l)
    if m:
        cells[int(m.group(1))] = [int(v) for v in m.group(3).split()]
events = []
for f, c in sorted(cells.items()):
    a, b = cells.get(f - 1), cells.get(f + 1)
    if not a or not b:
        continue
    lost = sum(max(0, min(a[i], b[i]) - c[i]) for i in range(256) if abs(a[i] - b[i]) * 3 < abs((a[i] + b[i]) / 2 - c[i]))
    gained = sum(max(0, c[i] - max(a[i], b[i])) for i in range(256) if abs(a[i] - b[i]) * 3 < abs((a[i] + b[i]) / 2 - c[i]))
    if lost + gained >= 300:
        events.append((f, lost, gained))
big = [e for e in events if e[1] + e[2] >= 2000]
print('frames', len(cells), 'flicker events', len(events), 'large (2000+ texels)', len(big))
for e in (big if '-v' not in sys.argv else events)[:40]:
    print('frame %d: texels lost %d gained %d' % e)
