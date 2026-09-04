import harness_common
"""Switch the desktop test game cfg between 'bench' and 'play' settings (idempotent).
Usage: cfgmode.py bench|play"""
import io, os, re, sys
P = os.path.join(harness_common.GAME, "fallout2.cfg")
mode = sys.argv[1]
vals = {
    'bench': {'edge_scroll': '0', 'follow_hero': '0', 'master_volume': '0', 'music_volume': '0', 'sndfx_volume': '0', 'speech_volume': '0'},
    'play': {'edge_scroll': '1', 'follow_hero': '1', 'master_volume': '22281', 'music_volume': '22281', 'sndfx_volume': '22281', 'speech_volume': '23340'},
}[mode]
t = io.open(P, encoding='utf-8', newline='').read()
for k, v in vals.items():
    t, n = re.subn(r'(?m)^' + k + r'=.*$', k + '=' + v, t)
    assert n == 1, k
io.open(P, 'w', encoding='utf-8', newline='').write(t)
print("cfg ->", mode)
