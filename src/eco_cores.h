#ifndef FALLOUT_ECO_CORES_H_
#define FALLOUT_ECO_CORES_H_

namespace fallout {

// On Apple Silicon, a thread at "utility" QoS is scheduled on the
// efficiency cores - several times less power for the same (small) load.
// Applies to the calling thread; call from the main thread.
void applyEcoCores(bool enabled);

} // namespace fallout

#endif // FALLOUT_ECO_CORES_H_
