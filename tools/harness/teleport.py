import harness_common
import io, os
os.chdir(harness_common.REPO)

p = 'src/game_sound.cc'
t = io.open(p, encoding='utf-8', newline='').read()
old = "void _gsound_bkg_proc()\n{"
new = """void _gsound_bkg_proc()
{
    // TEMP TEST HOOK (not committed): FALLOUT_TELEPORT=<mapid> teleports
    // once after the new game settles, then leaves the window running.
    {
        static int tpPhase = 0;
        const char* tpEnv = getenv("FALLOUT_TELEPORT");
        if (tpPhase == 0 && tpEnv != nullptr && SDL_GetTicks() > 34000) {
            int mapId = atoi(tpEnv);
            if (mapId > 0) {
                MapTransition transition;
                memset(&transition, 0, sizeof(transition));
                transition.map = static_cast<Map>(mapId);
                transition.elevation = 0;
                transition.tile = -1;
                transition.rotation = ROTATION_NE;
                mapSetTransition(&transition);
            }
            tpPhase = 1;
        }
    }
"""
assert t.count(old) == 1
t = t.replace(old, new)
if '#include <stdlib.h>' not in t:
    t = t.replace('#include "game_sound.h"\n', '#include "game_sound.h"\n\n#include <stdlib.h>\n', 1)
for inc in ['#include "map.h"', '#include "map_defs.h"', '#include "obj_types.h"']:
    if inc not in t:
        t = t.replace('#include <stdlib.h>\n', '#include <stdlib.h>\n' + inc + '\n', 1)
io.open(p, 'w', encoding='utf-8', newline='').write(t)
print("teleport hook patched")
