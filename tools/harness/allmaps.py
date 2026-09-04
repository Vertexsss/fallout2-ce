import harness_common
import io, os, sys
os.chdir(harness_common.REPO)
def rd(p): return io.open(p, encoding='utf-8', newline='').read()
def wr(p, t): io.open(p, 'w', encoding='utf-8', newline='').write(t)
def repl(p, old, new, tag, n=1):
    t = rd(p)
    if t.count(old) != n:
        print("FAIL", tag, t.count(old)); sys.exit(1)
    wr(p, t.replace(old, new)); print("OK", tag)

# ---------- 1. bench flag: no freeze / no 5fps / no present early-out when unfocused ----------
repl('src/input.cc',
'''    if (!gProgramIsActive) {
        _GNW95_lost_focus();
    }''',
'''    if (!gProgramIsActive && getenv("FALLOUT_BENCH") == nullptr) {
        _GNW95_lost_focus();
    }''', "bench-input")
t = rd('src/input.cc')
if '#include <stdlib.h>' not in t:
    t = t.replace('#include "input.h"\n', '#include "input.h"\n\n#include <stdlib.h>\n', 1)
    wr('src/input.cc', t); print("OK bench-input-inc")

repl('src/fps_limiter.cc',
'''    if (!gProgramIsActive) {
        // Window is not focused (backgrounded, Stage Manager, Slide Over).
        targetFps = 5;
    } else if (now - _lastActivityTicks > _idleGraceMs) {''',
'''    if (!gProgramIsActive && getenv("FALLOUT_BENCH") == nullptr) {
        // Window is not focused (backgrounded, Stage Manager, Slide Over).
        targetFps = 5;
    } else if (now - _lastActivityTicks > _idleGraceMs) {''', "bench-fps")
t = rd('src/fps_limiter.cc')
if '#include <stdlib.h>' not in t:
    t = t.replace('#include "fps_limiter.h"\n', '#include "fps_limiter.h"\n\n#include <stdlib.h>\n', 1)
    wr('src/fps_limiter.cc', t); print("OK bench-fps-inc")

repl('src/svga.cc',
'''    if (!gProgramIsActive) {
        return;
    }

    // GPU iso mode: entering, leaving or (re)creating the ring needs one''',
'''    if (!gProgramIsActive && getenv("FALLOUT_BENCH") == nullptr) {
        return;
    }

    // GPU iso mode: entering, leaving or (re)creating the ring needs one''', "bench-present")

# ---------- 2. svga: profiling globals + compact per-present log + Klamath seam shots ----------
repl('src/svga.cc',
"static bool gIsoZoomDirty = false;",
"static bool gIsoZoomDirty = false;\nint gProfMap = -1;   // TEMP profiling: current map id\nint gProfAxis = -1;  // TEMP profiling: 0=X 1=Y 2=diag 3=anti", "prof-globals")

repl('src/svga.cc',
'''    SDL_RenderPresent(gSdlRenderer);
    gIsoZoomDirty = false;''',
'''    SDL_RenderPresent(gSdlRenderer);
    if (gProfMap >= 0 && gProfAxis >= 0) {
        long long uplNow = gStatUploadBytes - profBytes0;
        int fullup = (uplNow >= 1500 * 1024) ? 1 : 0;
        fprintf(stderr, "[p] m=%d a=%d u=%lld f=%d d=%d s=%d\\n",
            gProfMap, gProfAxis, uplNow / 1024, fullup, gDirtyRectCount, gIsoShiftsSincePresent);
        // Seam check: one frame per axis on Klamath (map 9).
        if (gProfMap == 9) {
            static int shotMask = 0;
            if ((shotMask & (1 << gProfAxis)) == 0) {
                shotMask |= 1 << gProfAxis;
                int sw = gSdlTextureSurface->w, sh = gSdlTextureSurface->h;
                unsigned char* rgb = (unsigned char*)malloc((size_t)sw * sh * 3);
                if (rgb && SDL_RenderReadPixels(gSdlRenderer, nullptr, SDL_PIXELFORMAT_RGB24, rgb, sw * 3) == 0) {
                    char nm[48]; snprintf(nm, sizeof(nm), "seam_a%d.ppm", gProfAxis);
                    FILE* zf = fopen(nm, "wb");
                    if (zf) { fprintf(zf, "P6 %d %d 255 ", sw, sh); fwrite(rgb, 1, (size_t)sw * sh * 3, zf); fclose(zf); }
                }
                free(rgb);
            }
        }
    }
    gIsoZoomDirty = false;''', "prof-log")

# capture upload baseline at present entry (after the identical-frame early-out)
repl('src/svga.cc',
'''    if (gDirtyRectCount == 0 && !gIsoZoomDirty) {
        return;
    }''',
'''    if (gDirtyRectCount == 0 && !gIsoZoomDirty) {
        return;
    }
    long long profBytes0 = gStatUploadBytes;  // TEMP profiling''', "prof-entry")

# ---------- 3. game_sound: all-maps center-crossing driver ----------
repl('src/game_sound.cc',
"void _gsound_bkg_proc()\n{",
'''void _gsound_bkg_proc()
{
    // TEMP TEST HOOK (not committed): FALLOUT_ALLMAPS walks every map id,
    // recenters on the map center, then sweeps the camera edge-to-edge
    // THROUGH the center on 4 axes (X, Y, diag, anti-diag), profiling each.
    {
        extern int gProfMap;
        extern int gProfAxis;
        static int started = 0;
        static int mapIdx = 0;
        static unsigned int mapStart = 0;
        static int recentered = 0;
        static int dir = 1;
        static double carry = 0.0;
        static unsigned int last = 0;
        const int LOAD_MS = 900;
        const int SWEEP_MS = 2400;     // 4 axes * 600ms
        const int MAP_MS = LOAD_MS + SWEEP_MS;
        const int MAP_COUNT = 151;
        if (getenv("FALLOUT_ALLMAPS") != nullptr) {
            unsigned int now = SDL_GetTicks();
            if (started == 0 && now > 34000) {
                started = 1;
                mapStart = 0;
            }
            if (started == 1) {
                if (mapStart == 0) {
                    if (mapIdx >= MAP_COUNT) {
                        gProfAxis = -1; gProfMap = -1;
                        exit(0);
                    }
                    MapTransition tr;
                    memset(&tr, 0, sizeof(tr));
                    tr.map = static_cast<Map>(mapIdx);
                    tr.elevation = 0;
                    tr.tile = -1;
                    tr.rotation = ROTATION_NE;
                    mapSetTransition(&tr);
                    gProfMap = mapIdx;
                    gProfAxis = -1;
                    mapStart = now;
                    recentered = 0;
                    last = now;
                    carry = 0.0;
                } else {
                    unsigned int el = now - mapStart;
                    if (el >= (unsigned)MAP_MS) {
                        mapIdx++;
                        mapStart = 0;
                    } else if (el < (unsigned)LOAD_MS) {
                        gProfAxis = -1;
                        last = now;
                    } else {
                        if (!recentered) {
                            recentered = 1;
                            tileSetCenter(20100, TILE_SET_CENTER_REFRESH_WINDOW | TILE_SET_CENTER_FLAG_IGNORE_SCROLL_RESTRICTIONS);
                            last = now;
                            dir = 1;
                        }
                        int axis = ((el - LOAD_MS) / 600) % 4;
                        gProfAxis = axis;
                        unsigned int dt = now - last;
                        last = now;
                        if (dt == 0 || dt > 100) dt = 16;
                        carry += (1000.0 / 33.0) * dt / 1000.0;
                        int step = static_cast<int>(carry);
                        carry -= step;
                        if (step > 0) {
                            int dx = 0, dy = 0;
                            if (axis == 0) dx = dir * step * 32;
                            else if (axis == 1) dy = dir * step * 24;
                            else if (axis == 2) { dx = dir * step * 32; dy = dir * step * 24; }
                            else { dx = dir * step * 32; dy = -dir * step * 24; }
                            int rc = mapScrollPixels(dx, dy);
                            if (rc == -1) dir = -dir;
                        }
                    }
                }
            }
        }
    }
''', "allmaps-driver")
t = rd('src/game_sound.cc')
if '#include <stdlib.h>' not in t:
    t = t.replace('#include "game_sound.h"\n', '#include "game_sound.h"\n\n#include <stdlib.h>\n', 1)
for inc in ['#include "map.h"', '#include "map_defs.h"', '#include "obj_types.h"', '#include "tile.h"']:
    if inc not in t:
        t = t.replace('#include <stdlib.h>\n', '#include <stdlib.h>\n' + inc + '\n', 1)
wr('src/game_sound.cc', t); print("OK allmaps-inc")

print("done")
