# Usage: speed.py <log> — world speed of the far shadow cascade's light matrix translation (follows the camera), per wall second.
import re, sys
pts = []; last_frame = None
for l in open(sys.argv[1], errors='replace'):
    m = re.search(r': L (\d+) \w+ (.*) t (\d+)', l)
    if not m: continue
    fr = int(m.group(1))
    if fr == last_frame: continue  # first light matrix of a frame = far cascade
    last_frame = fr
    v = [float(x) for x in m.group(2).replace('|', '').split()]
    # Far cascade scale: x row length; translation in light space divided by scale gives world units.
    if v[0] == 0 and v[1] == 0 and v[2] == 0: continue
    sx = (v[0] ** 2 + v[1] ** 2 + v[2] ** 2) ** 0.5
    sy = (v[4] ** 2 + v[5] ** 2 + v[6] ** 2) ** 0.5
    if sy == 0: continue
    pts.append((int(m.group(3)) / 1000.0, v[3] / sx, v[7] / sy, fr))
# Speed over 1 s windows.
speeds = []; i = 0
for j in range(len(pts)):
    while pts[j][0] - pts[i][0] > 1.0: i += 1
    if pts[j][0] - pts[i][0] > 0.9:
        dx = pts[j][1] - pts[i][1]; dy = pts[j][2] - pts[i][2]
        speeds.append(((dx * dx + dy * dy) ** 0.5 / (pts[j][0] - pts[i][0]), (pts[j][3] - pts[i][3]) / (pts[j][0] - pts[i][0])))
moving = sorted(s for s, f in speeds if s > 1.0)
fps = sorted(f for s, f in speeds)
if moving:
    print('windows', len(speeds), 'moving', len(moving), 'speed median %.2f p75 %.2f p90 %.2f' % (moving[len(moving) // 2], moving[len(moving) * 3 // 4], moving[len(moving) * 9 // 10]), 'fps median %.1f' % fps[len(fps) // 2])
import collections
hist = collections.Counter(int(s) for s in moving)
print(' '.join('%d:%d' % (k, hist[k]) for k in range(0, 30)))
