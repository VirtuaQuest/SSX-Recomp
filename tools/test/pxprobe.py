# Usage: pxprobe.py <log> [...] — with AR_SHADOW_PIXELS=3 or more: for the draws that wrote nothing in a frame while
# writing samples in both neighbours (pxdrop.py), what the probe drawn after them covered in that frame ("area"). A probe
# that renders while the draw didn't means the probe's change (AR_SHADOW_DEFAULT_COPY, ...) avoids the failure.
import re, sys
from collections import defaultdict

for path in sys.argv[1:]:
    records = defaultdict(list)
    for l in open(path, errors='replace'):
        m = re.search(r'PX (.*)', l.rstrip())
        if m:
            t = m.group(1).split()
            if t[1] != '0' or 'area' not in t:
                continue
            area = int(t[t.index('area') + 1])
            records[int(t[0])].append((tuple(t[2:6]), area, int(t[-1])))
    rendered = failed = 0
    for f in sorted(records):
        best = lambda frame: {k: s for k, a, s in records.get(frame, [])}
        a, c = best(f - 1), best(f + 1)
        for key, area, samples in records[f]:
            if samples == 0 and a.get(key, 0) > 300 and c.get(key, 0) > 300:
                if area > 0:
                    rendered += 1
                else:
                    failed += 1
    print(f'{path}: dropped draws whose probe rendered {rendered}, whose probe failed too {failed}')
