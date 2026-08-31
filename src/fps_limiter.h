#ifndef FPS_LIMITER_H
#define FPS_LIMITER_H

namespace fallout {

class FpsLimiter {
public:
    FpsLimiter(unsigned int fps = 60);

    void mark();
    void throttle();

    // Called whenever something is drawn or the user interacts, so the limiter
    // knows the game is not idling.
    void notifyActivity();

    // Called on every actually-presented frame. Long stretches with neither
    // presents nor input let the limiter sink to a deeper idle level.
    void notifyPresent();

    // Frame rate the last throttle() call aimed for (for the stats overlay).
    unsigned int lastTargetFps() const { return _lastTargetFps; }

    // Milliseconds per second the main thread spent working (not sleeping)
    // over the last completed one-second window.
    unsigned int busyMsPerSec() const { return _lastBusyMsPerSec; }

    void setIdleFps(unsigned int fps) { _idleFps = fps; }
    // Upper bound for the ACTIVE frame rate (battery lever: 30 halves the
    // rendering work during play; idle tiers stay below it anyway).
    void setFpsCap(unsigned int fps) { _fpsCap = fps != 0 ? fps : 60; }
    void setIdleGrace(unsigned int ms) { _idleGraceMs = ms; }

private:
    const unsigned int _fps;
    unsigned int _fpsCap = 60;
    unsigned int _idleFps;
    unsigned int _idleGraceMs;
    unsigned int _ticks;
    unsigned int _lastActivityTicks;
    unsigned int _lastPresentTicks;
    unsigned int _lastTargetFps;
    unsigned int _busyAccumMs;
    unsigned int _busyWindowStart;
    unsigned int _lastBusyMsPerSec;
};

} // namespace fallout

#endif /* FPS_LIMITER_H */
