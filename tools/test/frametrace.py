# Usage: frametrace.py <file> [summary]  --  reads AR_FRAME_TRACE files. As a module: load(path) gives {tag: (frames, array)}: planes for the
# final picture and masks (brightness, alpha) or raw words for anything else, one row per frame.
import struct, sys
import numpy as np

def load(path):
    records = {}
    with open(path, 'rb') as f:
        while True:
            head = f.read(24)
            if len(head) < 24:
                break
            tag, fmt, w, h, frame = struct.unpack('<IIIIQ', head)
            planes = fmt == 28 and tag < 0x0F001000
            n = w * h * 2 if planes else w * h * 4
            data = f.read(n)
            if len(data) < n:
                break
            records.setdefault(tag, []).append((frame, w, h, fmt, data))
    out = {}
    for tag, items in records.items():
        items = [i for i in items if (i[1], i[2]) == (items[-1][1], items[-1][2])]
        frames = np.array([i[0] for i in items])
        w, h, fmt = items[-1][1:4]
        if fmt == 28 and tag < 0x0F001000:
            a = np.frombuffer(b''.join(i[4] for i in items), dtype=np.uint8).reshape(len(items), 2, h, w)
        else:
            a = np.frombuffer(b''.join(i[4] for i in items), dtype=np.uint32).reshape(len(items), h, w)
        out[tag] = (frames, a)
    return out

if __name__ == '__main__':
    t = load(sys.argv[1])
    for tag, (frames, a) in sorted(t.items()):
        print('%08X frames %d..%d (%d, gaps %d) shape %s' % (tag, frames[0], frames[-1], len(frames), int((np.diff(frames) != 1).sum()), a.shape[1:]))
