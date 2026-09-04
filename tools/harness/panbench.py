import harness_common
import io, os
os.chdir(harness_common.REPO)

p = 'src/game_sound.cc'
t = io.open(p, encoding='utf-8', newline='').read()
old = "void _gsound_bkg_proc()\n{"
new = """void _gsound_bkg_proc()
{
    // TEMP TEST HOOK (not committed): FALLOUT_PANBENCH teleports to map 9,
    // toggles the auto-pan benchmark on, and logs the view bias over time
    // to confirm the camera sweeps and bounces off both edges.
    {
        static int pbPhase = 0;
        static unsigned int pbLastLog = 0;
        if (pbPhase >= 0 && getenv("FALLOUT_PANBENCH") != nullptr) {
            unsigned int pt = SDL_GetTicks();
            if (pbPhase == 0 && pt > 34000) {
                MapTransition transition;
                memset(&transition, 0, sizeof(transition));
                transition.map = static_cast<Map>(9);
                transition.elevation = 0;
                transition.tile = -1;
                transition.rotation = ROTATION_NE;
                mapSetTransition(&transition);
                pbPhase = 1;
            } else if (pbPhase == 1 && pt > 39000) {
                touchOverlayToggleAutoPan();
                fprintf(stderr, "[pb] auto-pan toggled ON at %u\\n", pt);
                pbPhase = 2;
            } else if (pbPhase == 2) {
                if (pt > 55000) {
                    exit(0);
                }
                if (pt - pbLastLog >= 500) {
                    pbLastLog = pt;
                    int bx, by;
                    tileGetViewPixelBias(&bx, &by);
                    fprintf(stderr, "[pb] t=%u center=%d bias=%d,%d\\n", pt, gCenterTile, bx, by);
                }
            }
        }
    }
"""
assert t.count(old) == 1
t = t.replace(old, new)
if '#include <stdlib.h>' not in t:
    t = t.replace('#include "game_sound.h"\n', '#include "game_sound.h"\n\n#include <stdlib.h>\n', 1)
for inc in ['#include "map.h"', '#include "map_defs.h"', '#include "obj_types.h"', '#include "tile.h"', '#include "touch_overlay.h"']:
    if inc not in t:
        t = t.replace('#include <stdlib.h>\n', '#include <stdlib.h>\n' + inc + '\n', 1)
io.open(p, 'w', encoding='utf-8', newline='').write(t)
print("panbench hook patched")
