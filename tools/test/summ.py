# Usage: summ.py <log> — condenses an AR_TRACE_FRAME log: runs of similar draws collapsed into one line, resolves listed.
import re, sys
lines = open(sys.argv[1], errors='replace').read().splitlines()
prev = None
n = 0
first = 0
def flush():
    if prev:
        print(f"{first:6} x{n:4} {prev}")
for i, l in enumerate(lines, 1):
    m = re.search(r'draw prim (\d+) count \d+ indexed \w+ vs (\w+) ps (\w+) rt (\d) mask (\w+) depth (\w+) pitch (\d+) scissor (\S+) .* blend (\w+)(.*)', l)
    if m:
        texs = ' '.join(re.findall(r'@(\w+)/fmt(\d+)', m.group(10)) and [f"{a}/{b}" for a, b in re.findall(r'@(\w+)/fmt(\d+)', m.group(10))])
        key = f"draw ps {m.group(3)[:6]} rt{m.group(4)} mask {m.group(5)} depth {m.group(6)} pitch {m.group(7)} sc {m.group(8)} blend {m.group(9)} tex {texs}"
        if key == prev:
            n += 1
        else:
            flush(); prev = key; n = 1; first = i
    elif 'Resolve: resolve ' in l:
        flush(); prev = None
        print(f"{i:6}       {l[l.index('resolve '):][:140]}")
flush()
