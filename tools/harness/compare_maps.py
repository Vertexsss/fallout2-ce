import harness_common
import re
from collections import defaultdict
G = harness_common.GAME

def load(fn):
    per = defaultdict(lambda: defaultdict(list))
    for ln in open(G + "\\" + fn, encoding='utf-8', errors='replace'):
        m = re.match(r'\[p\] m=(\d+) a=(\d+) u=(-?\d+) f=(\d+) d=(\d+) s=(\d+)', ln)
        if m:
            mp, a, u, f, d, s = [int(x) for x in m.groups()]
            per[mp][a].append((u, f))
    return per

def stats(per, maps):
    # overall + per-axis over the common map set
    tot = []; ax = defaultdict(list)
    for mp in maps:
        for a in per[mp]:
            for u, f in per[mp][a]:
                tot.append((u, f)); ax[a].append((u, f))
    def s(L):
        n = len(L)
        if n == 0: return (0, 0, 0)
        avg = sum(u for u, f in L) / n
        full = 100.0 * sum(f for u, f in L) / n
        return (n, avg, full)
    return s(tot), {a: s(ax[a]) for a in sorted(ax)}

before = load("allmaps_before.txt")
after = load("allmaps.txt")
common = sorted(set(before) & set(after))
print("common maps:", len(common), " before-only:", len(set(before) - set(after)), " after-only:", len(set(after) - set(before)))
AX = {0: 'X', 1: 'Y', 2: 'DIAG', 3: 'ANTI'}
tb, ab = stats(before, common)
ta, aa = stats(after, common)
print("\n%-6s | %-26s | %-26s" % ("", "BEFORE (guard40)", "AFTER (guard40+merge)"))
print("%-6s | %6s %9s %7s | %6s %9s %7s" % ("axis", "n", "avg_kB", "full%", "n", "avg_kB", "full%"))
print("%-6s | %6d %9.0f %6.1f%% | %6d %9.0f %6.1f%%" % ("ALL", tb[0], tb[1], tb[2], ta[0], ta[1], ta[2]))
for a in sorted(set(ab) | set(aa)):
    b = ab.get(a, (0, 0, 0)); c = aa.get(a, (0, 0, 0))
    print("%-6s | %6d %9.0f %6.1f%% | %6d %9.0f %6.1f%%" % (AX.get(a, a), b[0], b[1], b[2], c[0], c[1], c[2]))
print("\nper-map avg kB (before -> after), sorted by improvement:")
rows = []
for mp in common:
    def avg(per):
        L = [u for a in per[mp] for u, f in per[mp][a]]
        return sum(L) / len(L) if L else 0
    rows.append((mp, avg(before), avg(after)))
for mp, b, a in sorted(rows, key=lambda r: (r[1] - r[2]), reverse=True)[:40]:
    print("  %-4d %7.0f -> %7.0f  (%+.0f%%)" % (mp, b, a, (a - b) / b * 100 if b else 0))
