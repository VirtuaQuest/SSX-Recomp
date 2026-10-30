# Usage: altscore.py <folder> — counts far-cascade frames that match two frames back but not one frame back (A/B flicker).
import glob, re, sys
import numpy as np
from PIL import Image
data = {}
for f in sorted(glob.glob(sys.argv[1] + '/resolve_*_0F801000.bmp')):
    data[int(re.search(r'resolve_(\d+)_', f).group(1))] = f
frames = sorted(data)
imgs = {fr: np.asarray(Image.open(data[fr]).convert('L'), dtype=np.int16) for fr in frames}
alt = []
for fr in frames:
    if fr - 1 in imgs and fr - 2 in imgs:
        d1 = np.abs(imgs[fr] - imgs[fr - 1]).mean(); d2 = np.abs(imgs[fr] - imgs[fr - 2]).mean()
        if d1 > 8 and d2 < d1 * 0.5:
            alt.append((fr, round(d1, 1), round(d2, 1)))
print(sys.argv[1].split('/')[-1], 'frames', len(frames), 'alternating', len(alt), alt[:10])
