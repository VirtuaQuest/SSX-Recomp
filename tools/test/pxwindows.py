# Usage: pxwindows.py <log> [...] — with AR_SHADOW_PIXELS: in the far cascade, the draws that write nothing although
# the same draw (shader, vertex buffer, index offset, count) writes samples in both neighbouring frames, grouped into
# windows of consecutive draws, with the draws just before each window.
import re, sys
from collections import defaultdict

for path in sys.argv[1:]:
    px = defaultdict(list)
    for l in open(path, errors='replace'):
        m = re.search(r'PX (.*)', l.rstrip())
        if m:
            t = m.group(1).split()
            if t[1] == '0':
                px[int(t[0])].append((t[2], t[3], t[4], t[5], int(t[-1])))
    print('==', path)
    for f in sorted(px):
        if f - 1 not in px or f + 1 not in px:
            continue
        def best(frame):
            d = defaultdict(int)
            for k in px[frame]:
                d[k[:4]] = max(d[k[:4]], k[4])
            return d
        a, c = best(f - 1), best(f + 1)
        dropped = [i for i, k in enumerate(px[f]) if k[4] == 0 and a[k[:4]] > 300 and c[k[:4]] > 300]
        if not dropped:
            continue
        # The window: from the first dropped draw back over zero-sample draws, forward to the last one.
        start, end = dropped[0], dropped[-1]
        while start > 0 and px[f][start - 1][4] == 0:
            start -= 1
        before = px[f][max(0, start - 3):start]
        print(f'frame {f}: dropped {len(dropped)} in draws {dropped[0]}-{end} (zero run from {start}); before:',
              ' '.join(f'{k[0][:4]}/{k[1]}/{k[2]}/{k[3]}={k[4]}' for k in before),
              '| first dropped', '/'.join(px[f][dropped[0]][:4]))
