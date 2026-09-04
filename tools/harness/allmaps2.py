import harness_common
import io, os, sys
os.chdir(harness_common.REPO)
p = 'src/game_sound.cc'
t = io.open(p, encoding='utf-8', newline='').read()
old = "void _gsound_bkg_proc()\n{"
new = '''void _gsound_bkg_proc()
{
    // TEMP TEST HOOK (not committed): FALLOUT_ALLMAPS v2 - walks the real
    // location maps (no random-encounter maps), drops the dude on the map
    // CENTER tile (20100 - never an exit grid, so no worldmap hijack), then
    // sweeps the camera edge-to-edge THROUGH the center on 4 axes. Profile
    // lines are tagged with the ACTUALLY loaded map, and only while the
    // world view is up and no combat is running.
    {
        extern int gProfMap;
        extern int gProfAxis;
        static const int kMaps[] = {
            3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22,
            23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
            41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 54, 55, 56, 57, 58, 59,
            60, 61, 62, 63, 64, 65, 66, 67, 78, 79, 92, 93, 109, 126, 127, 135, 136,
            137, 138, 139, 140, 147, 148 };
        const int kMapCount = sizeof(kMaps) / sizeof(kMaps[0]);
        static int started = 0;
        static int mapIdx = 0;
        static unsigned int mapStart = 0;
        static int dir = 1;
        static double carry = 0.0;
        static unsigned int last = 0;
        static unsigned int isoDownSince = 0;
        const int LOAD_MS = 1000;
        const int SWEEP_MS = 2400;     // 4 axes * 600ms
        const int MAP_MS = LOAD_MS + SWEEP_MS;
        if (getenv("FALLOUT_ALLMAPS") != nullptr) {
            unsigned int now = SDL_GetTicks();
            if (started == 0 && now > 34000) {
                started = 1;
                mapStart = 0;
            }
            if (started == 1) {
                // Worldmap watchdog: the iso view down for 8s means a
                // transition escaped to the world map - report and stop.
                if (isoIsDisabled()) {
                    if (isoDownSince == 0) isoDownSince = now;
                    else if (now - isoDownSince > 8000) {
                        fprintf(stderr, "[wd] iso down 8s at target=%d actual=%d - aborting\\n", kMaps[mapIdx < kMapCount ? mapIdx : kMapCount - 1], gMapHeader.index);
                        exit(2);
                    }
                } else {
                    isoDownSince = 0;
                }

                if (mapStart == 0) {
                    if (mapIdx >= kMapCount) {
                        gProfAxis = -1; gProfMap = -1;
                        fprintf(stderr, "[done] all maps\\n");
                        exit(0);
                    }
                    MapTransition tr;
                    memset(&tr, 0, sizeof(tr));
                    tr.map = static_cast<Map>(kMaps[mapIdx]);
                    tr.elevation = 0;
                    tr.tile = 20100;
                    tr.rotation = ROTATION_NE;
                    mapSetTransition(&tr);
                    gProfAxis = -1;
                    mapStart = now;
                    last = now;
                    carry = 0.0;
                    dir = 1;
                } else {
                    unsigned int el = now - mapStart;
                    int target = kMaps[mapIdx];
                    if (el >= (unsigned)MAP_MS) {
                        if (gMapHeader.index != target) {
                            fprintf(stderr, "[skip] target=%d actual=%d (combat=%d)\\n", target, gMapHeader.index, (int)isInCombat());
                        }
                        mapIdx++;
                        mapStart = 0;
                    } else if (el < (unsigned)LOAD_MS || gMapHeader.index != target || isoIsDisabled() || isInCombat()) {
                        gProfAxis = -1;
                        last = now;
                    } else {
                        gProfMap = gMapHeader.index;
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
'''
assert t.count(old) == 1
t = t.replace(old, new)
if '#include <stdlib.h>' not in t:
    t = t.replace('#include "game_sound.h"\n', '#include "game_sound.h"\n\n#include <stdlib.h>\n', 1)
for inc in ['#include "map.h"', '#include "map_defs.h"', '#include "obj_types.h"', '#include "tile.h"', '#include "combat.h"']:
    if inc not in t:
        t = t.replace('#include <stdlib.h>\n', '#include <stdlib.h>\n' + inc + '\n', 1)
io.open(p, 'w', encoding='utf-8', newline='').write(t)
print("allmaps v2 driver patched")
