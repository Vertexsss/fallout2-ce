import harness_common
"""Aggregate [t] phase lines from a FALLOUT_PHASEBENCH run into STATIC vs PAN windows.
Usage: phases.py phase9.txt [phase42.txt ...]"""
import re, sys, os
G = harness_common.GAME
FIELDS = ["loops", "pbk", "tick", "anim", "scr", "gm", "cyc", "snd", "render", "wref", "present", "sleep", "anim_on", "anim_off", "rr_in", "rr_out", "tgt", "tileref", "trcalls", "trarea", "gmpath", "pathcalls",
          "scr_upd", "scr_sfall", "scr_evt", "scr_crit", "scr_timed", "progs", "critcalls", "critmax", "pr_blit", "pr_upc", "pr_ring", "pr_comp", "pr_flip", "presents", "prrects", "prwrects"]
for fn in sys.argv[1:]:
    tele = pan = None
    rows = []
    for ln in open(os.path.join(G, fn), encoding="utf-8", errors="replace"):
        m = re.match(r"\[ph\] teleport \d+ at (\d+)", ln)
        if m: tele = int(m.group(1))
        m = re.match(r"\[ph\] pan ON at (\d+)", ln)
        if m: pan = int(m.group(1))
        if ln.startswith("[t] "):
            d = dict(re.findall(r"(\w+)=(-?[\d.]+)", ln))
            rows.append({k: float(v) for k, v in d.items()})
    print("==", fn, "teleport", tele, "pan", pan, "rows", len(rows))
    def win(lo, hi):
        return [r for r in rows if lo <= r["t"] < hi]
    groups = [("MENU/INTRO", win(0, 30000))]
    if tele:
        groups.append(("STATIC(map)", win(tele + 4000, (pan or 10**9) - 1000)))
    if pan:
        groups.append(("PAN", win(pan + 2000, 10**9)))
    print("%-12s %5s" % ("window", "secs") + "".join(" %8s" % f for f in FIELDS))
    for name, rs in groups:
        if not rs: continue
        avg = {f: sum(r.get(f, 0) for r in rs) / len(rs) for f in FIELDS}
        print("%-12s %5d" % (name, len(rs)) + "".join(" %8.1f" % avg[f] for f in FIELDS))
    # derived: busy = pbk + present (+ wref/render are inside pbk), sleep separate
    for name, rs in groups:
        if not rs: continue
        avg = {f: sum(r.get(f, 0) for r in rs) / len(rs) for f in FIELDS}
        other_tick = avg["tick"] - avg["anim"] - avg["scr"] - avg["gm"] - avg["cyc"] - avg["snd"]
        minfo = avg["pbk"] - avg["tick"]
        busy = avg["pbk"] + avg["present"]
        print("  %-12s busy=%.1f ms/s (%.1f%% core)  of which: render=%.1f wref=%.1f present=%.1f anim(excl render)=%.1f scr=%.1f gm=%.1f cyc=%.1f snd=%.1f other_tick=%.1f mouse/buttons=%.1f | sleep=%.0f | anim on/off=%.0f/%.0f rr in/out=%.0f/%.0f" % (
            name, busy, busy / 10.0, avg["render"], avg["wref"], avg["present"], max(0.0, avg["anim"] - avg["render"] - avg["wref"]), avg["scr"], avg["gm"], avg["cyc"], avg["snd"], other_tick, minfo, avg["sleep"], avg["anim_on"], avg["anim_off"], avg["rr_in"], avg["rr_out"]))
