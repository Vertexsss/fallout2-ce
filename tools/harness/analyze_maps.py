import harness_common
import re, sys
from collections import defaultdict
G = harness_common.GAME
names = {}
try:
    sys.argv = ['x']
    sys.path.insert(0, harness_common.HERE)
    import f2scan
    fs = f2scan.FS(); d = fs.read('data\\maps.txt').decode('cp1251', 'replace')
    cur = None
    for ln in d.splitlines():
        m = re.match(r'\[Map (\d+)\]', ln)
        if m: cur = int(m.group(1))
        elif cur is not None and ln.lower().startswith('map_name'): names[cur] = ln.split('=', 1)[1].strip()
except Exception as e:
    pass

rows = []
skips = []
for ln in open(G + r"\allmaps.txt", encoding='utf-8', errors='replace'):
    m = re.match(r'\[p\] m=(\d+) a=(\d+) u=(-?\d+) f=(\d+) d=(\d+) s=(\d+)', ln)
    if m:
        rows.append([int(x) for x in m.groups()])
    elif ln.startswith('[skip]') or ln.startswith('[wd]') or ln.startswith('[done]'):
        skips.append(ln.strip())

AX = {0: 'X', 1: 'Y', 2: 'D', 3: 'A'}
per = defaultdict(lambda: defaultdict(list))
for m_, a, u, f, d, s in rows:
    per[m_][a].append((u, f, d, s))

print("maps profiled:", len(per), " frames:", len(rows))
for s in skips: print("  ", s)
print()
print("%-4s %-10s %6s | %7s %6s %6s | %6s %6s %6s %6s | %s" % ("id", "name", "frames", "avg_kB", "p90kB", "full%", "X_MBs", "Y_MBs", "D_MBs", "A_MBs", "avg_dirty"))
summary = []
for m_ in sorted(per):
    allf = [x for a in per[m_] for x in per[m_][a]]
    n = len(allf)
    if n == 0: continue
    upl = sorted(x[0] for x in allf)
    avg = sum(upl) / n
    p90 = upl[min(n - 1, int(n * 0.9))]
    fullpct = 100.0 * sum(x[1] for x in allf) / n
    dirty = sum(x[2] for x in allf) / n
    # per-axis MB/s assuming ~60fps => kB/frame * 60 / 1024
    def mbs(a):
        L = per[m_].get(a, [])
        return (sum(x[0] for x in L) / len(L)) * 60 / 1024.0 if L else 0.0
    row = (m_, names.get(m_, '?')[:10], n, avg, p90, fullpct, mbs(0), mbs(1), mbs(2), mbs(3), dirty)
    summary.append(row)
    print("%-4d %-10s %6d | %7.0f %6d %5.1f%% | %6.1f %6.1f %6.1f %6.1f | %.1f" % row)

print()
print("=== worst by full-screen-upload % ===")
for r in sorted(summary, key=lambda r: -r[5])[:12]:
    print("%-4d %-10s full=%5.1f%% avg=%5.0fkB dirty=%.1f" % (r[0], r[1], r[5], r[3], r[10]))
print()
print("=== worst by avg upload ===")
for r in sorted(summary, key=lambda r: -r[3])[:12]:
    print("%-4d %-10s avg=%5.0fkB full=%5.1f%% dirty=%.1f" % (r[0], r[1], r[3], r[5], r[10]))
print()
print("=== best (lowest avg upload) ===")
for r in sorted(summary, key=lambda r: r[3])[:6]:
    print("%-4d %-10s avg=%5.0fkB full=%5.1f%%" % (r[0], r[1], r[3], r[5]))
