# Usage: perdraw.py <fight log>... — command processor cost per draw (microseconds) and frame rate in heavy stretches
# (more than 4000 draws a frame) of fight.sh runs. Fight content varies between runs; cost per draw compares better
# than frame rates.
import re
import sys

for path in sys.argv[1:]:
    rows = []
    for line in open(path, errors='replace'):
        m = re.search(r'profile: ([0-9.]+) fps; per frame ([0-9.]+) ms draws \((\d+) draws', line)
        if m and int(m.group(3)) > 4000:
            rows.append((float(m.group(1)), float(m.group(2)), int(m.group(3))))
    busy = [float(m.group(1)) for m in re.finditer(r'command processor busy \d+% \(([0-9.]+) ms per frame', open(path, errors='replace').read())]
    draws_all = [int(m.group(1)) for m in re.finditer(r'per frame [0-9.]+ ms draws \((\d+) draws', open(path, errors='replace').read())]
    if busy and draws_all:
        # From the second half of the run (fights), total command processor time per draw.
        b = busy[len(busy) // 2:]
        d = draws_all[len(draws_all) // 2:]
        print('%s: command processor %.2f ms per frame, %.0f draws per frame -> %.2f us per draw overall'
              % (path, sum(b) / len(b), sum(d) / len(d), sum(b) / len(b) * 1000 / (sum(d) / len(d))))
    if not rows:
        print(path, 'no heavy stretches')
        continue
    per_draw = sorted(ms * 1000 / draws for _, ms, draws in rows)
    fps = sorted(f for f, _, _ in rows)
    print('%s: %d heavy reports, %.2f us per draw (median), %.1f fps (median), %.1f fps (worst)'
          % (path, len(rows), per_draw[len(per_draw) // 2], fps[len(fps) // 2], fps[0]))
