#ifndef FALLOUT_POWER_STATE_H_
#define FALLOUT_POWER_STATE_H_

namespace fallout {

// True while the system asks apps to save power: Low Power Mode is on, or
// the thermal state is serious/critical (Apple's guidance for both is to
// lower frame rates). Cached; the OS is queried at most once a second.
bool powerStateSaverRequested();

} // namespace fallout

#endif // FALLOUT_POWER_STATE_H_
