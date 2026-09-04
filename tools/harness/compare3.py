import re, os
from collections import defaultdict
G = r"C:\Users\45247\AppData\Local\Temp\claude\c--Users-45247-cursor-projects-fallout\139ff2f2-55f1-4a38-ab82-942554b6d5ad\scratchpad\game"

def load(fn):
    per = defaultdict(lambda: defaultdict(list))
    p = os.path.join(G, fn)
    if not os.path.exists(p): return per
    for ln in open(p, encoding='utf-8', errors='replace'):
        m = re.match(r'\[p\] m=(\d+) a=(\d+) u=(-?\d+) f=(\d+) d=(\d+) s=(\d+)', ln)
        if m:
            mp, a, u, f, d, s = [int(x) for x in m.groups()]
            per[mp][a].append((u, f))
    return per

runs = [("BEFORE(guard40)", load("allmaps_before.txt")),
        ("MERGE v1 (cap16)", load("allmaps_mergev1.txt")),
        ("MERGE+cap64", load("allmaps.txt"))]
common = set.intersection(*[set(r[1]) for r in runs if r[1]]) if all(r[1] for r in runs) else set()
print("common maps across runs:", len(common))
AX = {-1: 'ALL', 0: 'X', 1: 'Y', 2: 'DIAG', 3: 'ANTI'}

def stat(per, axis):
    L = []
    for mp in common:
        for a in per[mp]:
            if axis == -1 or a == axis:
                L.extend(per[mp][a])
    n = len(L)
    if n == 0: return "      -"
    avg = sum(u for u, f in L) / n
    full = 100.0 * sum(f for u, f in L) / n
    return "%5.0fkB %5.1f%%" % (avg, full)

hdr = "%-6s" % "axis" + "".join(" | %-18s" % r[0] for r in runs)
print(hdr)
for axis in (-1, 0, 1, 2, 3):
    print("%-6s" % AX[axis] + "".join(" | %-18s" % stat(r[1], axis) for r in runs))
