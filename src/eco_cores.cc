#include "eco_cores.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#include <pthread/qos.h>
#endif
#endif

namespace fallout {

static bool gEcoEnabled = true;
static bool gEcoIdle = false;
static int gEcoApplied = -1;

static void ecoCoresApply()
{
    int want = (gEcoEnabled && gEcoIdle) ? 1 : 0;
    if (want == gEcoApplied) {
        return;
    }
    gEcoApplied = want;
#if defined(__APPLE__) && TARGET_OS_IOS
    pthread_set_qos_class_self_np(want != 0 ? QOS_CLASS_UTILITY : QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

void ecoCoresSetEnabled(bool enabled)
{
    gEcoEnabled = enabled;
    ecoCoresApply();
}

void ecoCoresSetIdle(bool idle)
{
    gEcoIdle = idle;
    ecoCoresApply();
}

} // namespace fallout
