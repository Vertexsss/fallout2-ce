"""TEMP targeted profiler for the slow maps. Applies on top of bench.patch
(allmaps.py + allmaps2.py + bench2.py + autogame.py already applied):
- driver map list from env FALLOUT_MAPS="79,92,93,109,9" (default), 5s sweep/map
- per-present log gains: dt since last present, refresh-rect calls and
  summed world-repaint area since last present, light updates since last present.
"""
import io, os, sys, re
os.chdir(r"C:\Users\45247\AppData\Local\Temp\claude\c--Users-45247-cursor-projects-fallout\139ff2f2-55f1-4a38-ab82-942554b6d5ad\scratchpad\f2ce")
def rd(p): return io.open(p, encoding='utf-8', newline='').read()
def wr(p, t): io.open(p, 'w', encoding='utf-8', newline='').write(t)
def repl(p, old, new, tag, n=1):
    t = rd(p)
    if t.count(old) != n:
        print("FAIL", tag, t.count(old)); sys.exit(1)
    wr(p, t.replace(old, new)); print("OK", tag)

# --- counters (svga.cc globals, non-static) ---
repl('src/svga.cc',
"int gProfAxis = -1;  // TEMP profiling: 0=X 1=Y 2=diag 3=anti",
"int gProfAxis = -1;  // TEMP profiling: 0=X 1=Y 2=diag 3=anti\nint gProfRefreshCalls = 0;\nlong long gProfRefreshArea = 0;\nint gProfLightUpdates = 0;", "counters")

# --- tile.cc: count refresh calls + area in tileRefreshGame ---
repl('src/tile.cc',
"static void tileRefreshGame(Rect* rect, int elevation)\n{\n    Rect rectToUpdate;\n",
"static void tileRefreshGame(Rect* rect, int elevation)\n{\n    Rect rectToUpdate;\n    {\n        extern int gProfRefreshCalls; extern long long gProfRefreshArea;\n        gProfRefreshCalls++;\n        gProfRefreshArea += (long long)(rect->right - rect->left + 1) * (rect->bottom - rect->top + 1);\n    }\n", "tile-count")

# --- light.cc: count light intensity updates ---
t = rd('src/light.cc')
m = re.search(r'\n(int|void) lightSetIntensity\([^)]*\)\n\{\n', t)
if not m:
    m = re.search(r'\n(int|void) lightSetTileIntensity\([^)]*\)\n\{\n', t)
assert m, "light setter not found"
t = t[:m.end()] + "    { extern int gProfLightUpdates; gProfLightUpdates++; }\n" + t[m.end():]
wr('src/light.cc', t); print("OK light-count")

# --- present log: add dt + counters, reset per present ---
repl('src/svga.cc',
'''        fprintf(stderr, "[p] m=%d a=%d u=%lld f=%d d=%d s=%d\\n",
            gProfMap, gProfAxis, uplNow / 1024, fullup, gDirtyRectCount, gIsoShiftsSincePresent);''',
'''        static unsigned int profPrevT = 0;
        unsigned int profT = SDL_GetTicks();
        fprintf(stderr, "[p] m=%d a=%d u=%lld f=%d d=%d s=%d dt=%u rc=%d ra=%lld lu=%d\\n",
            gProfMap, gProfAxis, uplNow / 1024, fullup, gDirtyRectCount, gIsoShiftsSincePresent,
            profPrevT ? profT - profPrevT : 0, gProfRefreshCalls, gProfRefreshArea / 1024, gProfLightUpdates);
        profPrevT = profT;
        gProfRefreshCalls = 0; gProfRefreshArea = 0; gProfLightUpdates = 0;''', "present-log")

# --- driver: map list from env, 5s sweep ---
repl('src/game_sound.cc',
"        const int kMapCount = sizeof(kMaps) / sizeof(kMaps[0]);",
'''        static int kMapsEnv[64]; static int kMapsEnvCount = -1;
        if (kMapsEnvCount < 0) {
            kMapsEnvCount = 0;
            const char* e = getenv("FALLOUT_MAPS");
            if (e) { const char* q = e; while (*q && kMapsEnvCount < 64) { kMapsEnv[kMapsEnvCount++] = atoi(q); while (*q && *q != ',') q++; if (*q == ',') q++; } }
        }
        const int* kMapsUse = kMapsEnvCount > 0 ? kMapsEnv : kMaps;
        const int kMapCount = kMapsEnvCount > 0 ? kMapsEnvCount : (int)(sizeof(kMaps) / sizeof(kMaps[0]));''', "driver-env")
t = rd('src/game_sound.cc')
t = t.replace("kMaps[mapIdx < kMapCount ? mapIdx : kMapCount - 1]", "kMapsUse[mapIdx < kMapCount ? mapIdx : kMapCount - 1]")
t = t.replace("tr.map = static_cast<Map>(kMaps[mapIdx]);", "tr.map = static_cast<Map>(kMapsUse[mapIdx]);")
t = t.replace("int target = kMaps[mapIdx];", "int target = kMapsUse[mapIdx];")
t = t.replace("const int SWEEP_MS = 2400;     // 4 axes * 600ms", "const int SWEEP_MS = 4800;     // 4 axes * 1200ms")
t = t.replace("int axis = ((el - LOAD_MS) / 600) % 4;", "int axis = ((el - LOAD_MS) / 1200) % 4;")
wr('src/game_sound.cc', t); print("OK driver-list")
print("done")
