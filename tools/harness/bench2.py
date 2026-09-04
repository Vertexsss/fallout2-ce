import harness_common
import io, os, sys, re
os.chdir(harness_common.REPO)
def rd(p): return io.open(p, encoding='utf-8', newline='').read()
def wr(p, t): io.open(p, 'w', encoding='utf-8', newline='').write(t)
def ensure_stdlib(p):
    t = rd(p)
    if '#include <stdlib.h>' not in t and '#include <cstdlib>' not in t:
        m = re.search(r'#include [^\n]+\n', t)
        t = t[:m.end()] + '#include <stdlib.h>\n' + t[m.end():]
        wr(p, t)
def repl(p, old, new, tag):
    t = rd(p)
    if t.count(old) != 1:
        print("FAIL", tag, t.count(old)); sys.exit(1)
    wr(p, t.replace(old, new)); print("OK", tag)

# 1. map-enter scripts: the map's baked content loads, but nothing reacts to
#    the dude's arrival (dialogs, cutscenes, spawned combat).
repl('src/map.cc',
"    scriptsExecMapEnterProc();",
"    if (getenv(\"FALLOUT_BENCH\") == nullptr) {  // TEMP BENCH: no arrival scripts\n        scriptsExecMapEnterProc();\n    }", "bench-mapenter")
ensure_stdlib('src/map.cc')

# 2. dialog start: safety net
repl('src/game_dialog.cc',
"void gameDialogEnter(Object* speaker, int mode)\n{",
"void gameDialogEnter(Object* speaker, int mode)\n{\n    if (getenv(\"FALLOUT_BENCH\") != nullptr) {  // TEMP BENCH: never open a dialog\n        return;\n    }", "bench-dialog")
ensure_stdlib('src/game_dialog.cc')

# 3. combat start: safety net
repl('src/combat.cc',
"void _combat(CombatStartData* csd)\n{",
"void _combat(CombatStartData* csd)\n{\n    if (getenv(\"FALLOUT_BENCH\") != nullptr) {  // TEMP BENCH: never start combat\n        return;\n    }", "bench-combat")
ensure_stdlib('src/combat.cc')
print("done")
