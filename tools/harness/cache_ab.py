"""Compare warm-up runs: per map frames/dt/art-cache misses. Usage: cache_ab.py runA.txt [runB.txt]"""
import re, sys, os
from collections import defaultdict
G = r"C:\Users\45247\AppData\Local\Temp\claude\c--Users-45247-cursor-projects-fallout\139ff2f2-55f1-4a38-ab82-942554b6d5ad\scratchpad\game"
names = {}
try:
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import f2scan
    fs = f2scan.FS(); d = fs.read('data\\maps.txt').decode('cp1251', 'replace')
    cur = None
    for ln in d.splitlines():
        m = re.match(r'\[Map (\d+)\]', ln)
        if m: cur = int(m.group(1))
        elif cur is not None and ln.lower().startswith('map_name'): names[cur] = ln.split('=', 1)[1].strip()
except Exception:
    pass

def load(fn):
    per = defaultdict(list); order = []
    for ln in open(os.path.join(G, fn), encoding='utf-8', errors='replace'):
        m = re.match(r'\[p\] m=(\d+) a=(\d+) u=(-?\d+) f=(\d+) d=(\d+) s=(\d+) dt=(\d+) rc=(\d+) ra=(-?\d+) lu=(\d+) am=(\d+)', ln)
        if m:
            v = [int(x) for x in m.groups()]
            if v[0] not in per: order.append(v[0])
            per[v[0]].append(v)
    return per, order

def pct(L, p):
    L = sorted(L); return L[min(len(L) - 1, int(len(L) * p))] if L else 0

runs = [(a, load(a)) for a in sys.argv[1:]]
print("%-4s %-9s" % ("id", "name") + "".join(" | %-30s" % r[0][:30] for r in runs))
print("%-14s" % "" + "".join(" | %6s %6s %6s %8s" % ("frames", "dt_p50", "dt_p90", "artmiss/f") for _ in runs))
order = runs[0][1][1]
for mp in order:
    line = "%-4d %-9s" % (mp, names.get(mp, '?')[:9])
    for _, (per, _o) in runs:
        L = per.get(mp, [])
        if not L: line += " | %30s" % "-"; continue
        dt = [v[6] for v in L if 0 < v[6] < 2000]
        am = sum(v[10] for v in L) / len(L)
        line += " | %6d %6d %6d %8.1f" % (len(L), pct(dt, .5), pct(dt, .9), am)
    print(line)
