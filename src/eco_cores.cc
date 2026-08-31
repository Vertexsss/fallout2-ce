#include "eco_cores.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#include <pthread/qos.h>
#endif
#endif

namespace fallout {

void applyEcoCores(bool enabled)
{
#if defined(__APPLE__) && TARGET_OS_IOS
    // UTILITY prefers the E-cores; USER_INTERACTIVE is the UIKit main
    // thread default. The game needs a few ms per frame - the E-cores
    // handle that without dropping frames.
    pthread_set_qos_class_self_np(enabled ? QOS_CLASS_UTILITY : QOS_CLASS_USER_INTERACTIVE, 0);
#else
    (void)enabled;
#endif
}

} // namespace fallout
