#ifndef FALLOUT_ECO_CORES_H_
#define FALLOUT_ECO_CORES_H_

namespace fallout {

// Eco scheduling per Apple's energy guide: the main thread's QoS is
// lowered to "utility" ONLY while the game is idling (the guide's "run at
// utility or lower ... when user activity is not occurring"). Active play
// always runs at the user-interactive default - "utility" also relaxes
// timer latency (coalescing), which made a permanently-lowered main
// thread feel sluggish. All calls must come from the main thread.

// The CFG switch: when disabled, the thread stays user-interactive.
void ecoCoresSetEnabled(bool enabled);

// Driven by the fps limiter: true once the idle tier engages, false the
// moment activity is noticed.
void ecoCoresSetIdle(bool idle);

} // namespace fallout

#endif // FALLOUT_ECO_CORES_H_
