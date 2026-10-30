# Usage: regdiff.py <log> <frame> — context registers (AR_SHADOW_LOG "R" lines) that differ in this frame from both
# the frame before and after while those two agree.
import re, sys
target = int(sys.argv[2])
regs = {}
for l in open(sys.argv[1], errors='replace'):
    m = re.search(r': R (\d+) (.*)', l)
    if m and abs(int(m.group(1)) - target) <= 1:
        regs[int(m.group(1))] = m.group(2).split()
a, b, c = regs.get(target - 1), regs.get(target), regs.get(target + 1)
if not (a and b and c):
    sys.exit('missing frames')
for i in range(len(b)):
    if a[i] == c[i] and b[i] != a[i]:
        print('register 0x%04X: %s -> %s -> %s' % (0x2000 + i, a[i], b[i], c[i]))
changing = sum(1 for i in range(len(b)) if not (a[i] == b[i] == c[i]))
print('registers not equal in all three frames:', changing)
