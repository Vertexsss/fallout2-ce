"""Compare two full runs (e.g. 1x vs 1.5x): per map frames, dt p50/p90, upload kB/frame, full%.
Usage: compare_zoom.py full32.txt fullzoom.txt"""
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
        m = re.match(r'\[p\] m=(\d+) a=(\d+) u=(-?\d+) f=(\d+) d=(\d+) s=(\d+) dt=(\d+)', ln)
        if m:
            v = [int(x) for x in m.groups()]
            if v[0] not in per: order.append(v[0])
            per[v[0]].append(v)
    return per, order

def pct(L, p):
    L = sorted(L); return L[min(len(L) - 1, int(len(L) * p))] if L else 0

def summ(L):
    n = len(L); dt = [v[6] for v in L if 0 < v[6] < 5000]
    return n, pct(dt, .5), pct(dt, .9), sum(v[2] for v in L) / n, 100.0 * sum(v[3] for v in L) / n

runs = [(a, load(a)) for a in sys.argv[1:3]]
order = runs[0][1][1]
print("%-4s %-9s" % ("id", "name") + "".join(" | %-32s" % r[0] for r in runs))
print("%-14s" % "" + "".join(" | %6s %5s %5s %7s %6s" % ("frames", "dt50", "dt90", "upl_kB", "full%") for _ in runs))
tot = [[] for _ in runs]
for mp in order:
    line = "%-4d %-9s" % (mp, names.get(mp, '?')[:9])
    for i, (_, (per, _o)) in enumerate(runs):
        L = per.get(mp)
        if not L: line += " | %32s" % "-"; continue
        n, d50, d90, u, f = summ(L); tot[i].extend(L)
        line += " | %6d %5d %5d %7.0f %5.1f%%" % (n, d50, d90, u, f)
    print(line)
print("\nOVERALL:")
for i, (name, _) in enumerate(runs):
    if tot[i]:
        n, d50, d90, u, f = summ(tot[i])
        print("  %-14s frames=%d dt50=%d dt90=%d upl_kB/frame=%.0f full=%.1f%%" % (name, n, d50, d90, u, f))
