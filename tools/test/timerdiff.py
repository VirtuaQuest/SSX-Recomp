# Usage: timerdiff.py <log> — timers each AR_SCAN_KEY/remote scan found that the run's first scan didn't (press F9 once in
# ordinary play first, then during whatever runs too fast). Lines: address, kind, value, float value, delta per update.
import re, sys
scans = []
for line in open(sys.argv[1], errors='replace'):
    m = re.search(r'ScanTimers: timer scan at frame (\d+)', line)
    if m:
        scans.append((int(m.group(1)), {}))
    m = re.search(r'TIMER (\w+) (.*)', line)
    if m and scans:
        scans[-1][1][m.group(1)] = m.group(2).strip()
for i, (frame, timers) in enumerate(scans):
    print('scan %d at frame %d: %d timers' % (i, frame, len(timers)))
    if i == 0:
        continue
    new = sorted(set(timers) - set(scans[0][1]))
    print('  %d not in the first scan' % len(new))
    for address in new[:150]:
        print('  %s %s' % (address, timers[address]))
