# Usage: period.py <folder> — for flicker events, compares far cascade frame N with N-1, N-2 and N-3.
import glob, re, sys
import numpy as np
from PIL import Image
data = {}
for f in sorted(glob.glob(sys.argv[1] + '/resolve_*_0F801000.bmp')):
    data[int(re.search(r'resolve_(\d+)_', f).group(1))] = np.asarray(Image.open(f).convert('L'), dtype=np.int16)
for fr in sorted(data):
    if all(fr - k in data for k in (1, 2, 3)):
        d = [np.abs(data[fr] - data[fr - k]).mean() for k in (1, 2, 3)]
        if d[0] > 20 and min(d[1], d[2]) < d[0] * 0.4:
            print(fr, ' '.join('%.1f' % x for x in d))
