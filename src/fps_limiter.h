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

    void setIdleFps(unsigned int fps) { _idleFps = fps; }
    void setIdleGrace(unsigned int ms) { _idleGraceMs = ms; }

private:
    const unsigned int _fps;
    unsigned int _idleFps;
    unsigned int _idleGraceMs;
    unsigned int _ticks;
    unsigned int _lastActivityTicks;
    unsigned int _lastPresentTicks;
    unsigned int _lastTargetFps;
};

} // namespace fallout

#endif /* FPS_LIMITER_H */
