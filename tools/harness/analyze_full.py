import harness_common
"""Full-run analysis with per-frame counters. Usage: analyze_full.py full32.txt
Per map in run order: frames, dt p50/p90/max, refresh calls/frame, repaint kpx/frame,
light updates/frame, art misses/frame, upload kB/frame. Flags maps with frames < 80% of median."""
import re, sys, os
from collections import defaultdict
G = harness_common.GAME
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

fn = sys.argv[1] if len(sys.argv) > 1 else "full32.txt"
per = defaultdict(list); order = []
for ln in open(os.path.join(G, fn), encoding='utf-8', errors='replace'):
    m = re.match(r'\[p\] m=(\d+) a=(\d+) u=(-?\d+) f=(\d+) d=(\d+) s=(\d+) dt=(\d+) rc=(\d+) ra=(-?\d+) lu=(\d+) am=(\d+)', ln)
    if m:
        v = [int(x) for x in m.groups()]
        if v[0] not in per: order.append(v[0])
        per[v[0]].append(v)

def pct(L, p):
    L = sorted(L); return L[min(len(L) - 1, int(len(L) * p))] if L else 0

rows = []
for pos, mp in enumerate(order):
    L = per[mp]; n = len(L)
    dt = [v[6] for v in L if 0 < v[6] < 5000]
    rows.append(dict(pos=pos, id=mp, name=names.get(mp, '?')[:9], n=n,
                     dt50=pct(dt, .5), dt90=pct(dt, .9), dtmax=max(dt) if dt else 0,
                     rc=sum(v[7] for v in L) / n, ra=sum(v[8] for v in L) / n,
                     lu=sum(v[9] for v in L) / n, am=sum(v[10] for v in L) / n,
                     u=sum(v[2] for v in L) / n, d=sum(v[4] for v in L) / n))
med = sorted(r['n'] for r in rows)[len(rows) // 2] if rows else 0
print("maps:", len(rows), " median frames/map:", med)
hdr = "%3s %-4s %-9s %6s | %5s %5s %5s | %6s %8s %6s %7s | %6s %5s"
print(hdr % ("pos", "id", "name", "frames", "dt50", "dt90", "dtmax", "rc/f", "ra_kpx/f", "lu/f", "artm/f", "upl_kB", "dirty"))
for r in rows:
    flag = " <<< SLOW" if med and r['n'] < 0.8 * med else ""
    print(("%3d %-4d %-9s %6d | %5d %5d %5d | %6.1f %8.0f %6.1f %7.1f | %6.0f %5.1f" % (
        r['pos'], r['id'], r['name'], r['n'], r['dt50'], r['dt90'], r['dtmax'], r['rc'], r['ra'], r['lu'], r['am'], r['u'], r['d'])) + flag)
slow = [r for r in rows if med and r['n'] < 0.8 * med]
print("\nSLOW maps:", [(r['pos'], r['id'], r['name'], r['n']) for r in slow])
