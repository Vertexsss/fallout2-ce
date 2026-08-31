#include "power_state.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#include <objc/message.h>
#include <objc/runtime.h>
#endif
#endif

#include <SDL.h>

namespace fallout {

bool powerStateSaverRequested()
{
#if defined(__APPLE__) && TARGET_OS_IOS
    // NSProcessInfo through the ObjC runtime - keeps this file plain C++.
    static unsigned int lastTicks = 0;
    static bool cached = false;

    unsigned int now = SDL_GetTicks();
    if (lastTicks == 0 || now - lastTicks >= 1000) {
        lastTicks = now != 0 ? now : 1;

        id cls = reinterpret_cast<id>(objc_getClass("NSProcessInfo"));
        id info = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(cls, sel_registerName("processInfo"));
        if (info != nullptr) {
            bool lowPower = reinterpret_cast<bool (*)(id, SEL)>(objc_msgSend)(info, sel_registerName("isLowPowerModeEnabled"));
            // NSProcessInfoThermalState: 0 nominal, 1 fair, 2 serious, 3 critical.
            long thermal = reinterpret_cast<long (*)(id, SEL)>(objc_msgSend)(info, sel_registerName("thermalState"));
            cached = lowPower || thermal >= 2;
        }
    }
    return cached;
#else
    return false;
#endif
}

} // namespace fallout
