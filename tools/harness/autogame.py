import harness_common
import io, os
os.chdir(harness_common.REPO)
p = 'src/game_sound.cc'
t = io.open(p, encoding='utf-8', newline='').read()
old = "void _gsound_bkg_proc()\n{"
new = """void _gsound_bkg_proc()
{
    // TEMP TEST HOOK (not committed): FALLOUT_AUTOGAME=1 walks from the
    // main menu into a fresh game; FALLOUT_AUTOSCROLL=1 then parks the
    // cursor at alternating screen edges (35s..75s) so active map
    // scrolling can be measured.
    {
        static int autoPhase = 0;
        if (autoPhase >= 0 && getenv("FALLOUT_AUTOGAME") != nullptr) {
            unsigned int t = SDL_GetTicks();
            if (autoPhase == 0 && t > 6000) {
                enqueueInputEvent('n');
                autoPhase = 1;
            } else if (autoPhase == 1 && t > 10000) {
                enqueueInputEvent('t');
                autoPhase = 2;
            } else if (autoPhase >= 2 && autoPhase < 30 && t > 12000 + (autoPhase - 2) * 800) {
                // Spam SPACE every 800ms through 33s: skips the char screen
                // AND the elder.mve intro whenever it appears (its poll
                // loop ticks this hook and picks up the queued key). A
                // minimized/bench run shifts the movie later, so a fixed
                // burst that ends early would miss it.
                enqueueInputEvent(32);
                autoPhase++;
            } else if (autoPhase == 30) {
                if (getenv("FALLOUT_AUTOSCROLL") == nullptr) {
                    autoPhase = -1;
                } else if (t > 35000) {
                    autoPhase = 31;
                }
            } else if (autoPhase == 31) {
                if (t > 75000) {
                    autoPhase = -1;
                } else {
                    bool right = (t / 3000) % 2 == 0;
                    mapScrollPixels(right ? 4 : -4, 0);
                    sharedFpsLimiter.notifyActivity();
                }
            }
        }
    }
"""
assert t.count(old) == 1
t = t.replace(old, new)
if '#include <stdlib.h>' not in t:
    t = t.replace('#include "game_sound.h"\n', '#include "game_sound.h"\n\n#include <stdlib.h>\n', 1)
for inc in ['#include "mouse.h"', '#include "svga.h"', '#include "map.h"', '#include "fps_limiter.h"']:
    if inc not in t:
        t = t.replace('#include <stdlib.h>\n', '#include <stdlib.h>\n' + inc + '\n', 1)
io.open(p, 'w', encoding='utf-8', newline='').write(t)
print("patched")
