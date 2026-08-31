#include "fps_limiter.h"

#include <SDL.h>

#include "win32.h"

namespace fallout {

FpsLimiter::FpsLimiter(unsigned int fps)
    : _fps(fps != 0 ? fps : 60)
    , _idleFps(15)
    , _idleGraceMs(400)
    , _ticks(0)
    , _lastActivityTicks(0)
    , _lastPresentTicks(0)
    , _lastTargetFps(fps != 0 ? fps : 60)
    , _busyAccumMs(0)
    , _busyWindowStart(0)
    , _lastBusyMsPerSec(0)
{
}

void FpsLimiter::mark()
{
    _ticks = SDL_GetTicks();
}

void FpsLimiter::notifyActivity()
{
    _lastActivityTicks = SDL_GetTicks();
}

void FpsLimiter::notifyPresent()
{
    _lastPresentTicks = SDL_GetTicks();
}

void FpsLimiter::throttle()
{
    unsigned int now = SDL_GetTicks();

    // Time since mark() is the work portion of this iteration.
    _busyAccumMs += now - _ticks;
    if (now - _busyWindowStart >= 1000) {
        _lastBusyMsPerSec = _busyAccumMs;
        _busyAccumMs = 0;
        _busyWindowStart = now;
    }

    // After a long stretch with neither input nor a single presented frame
    // (a genuinely static screen - ambient animation like water still counts
    // as presents and keeps the regular idle level), sink deeper.
    constexpr unsigned int kDeepGraceMs = 20000;
    constexpr unsigned int kDeepIdleFps = 5;
    bool deepIdle = false;

    const unsigned int baseFps = _fps < _fpsCap ? _fps : _fpsCap;
    unsigned int targetFps = baseFps;
    if (!gProgramIsActive) {
        // Window is not focused (backgrounded, Stage Manager, Slide Over).
        targetFps = 5;
    } else if (now - _lastActivityTicks > _idleGraceMs) {
        // Nothing has been drawn for a while - the screen is static.
        targetFps = _idleFps;

        if (now - _lastActivityTicks > kDeepGraceMs
            && _lastPresentTicks != 0 && now - _lastPresentTicks > kDeepGraceMs) {
            targetFps = kDeepIdleFps;
            deepIdle = true;
        }
    }

    if (targetFps == 0) {
        targetFps = 1;
    }
    _lastTargetFps = targetFps;

    const unsigned int minFrameTime = 1000 / baseFps;
    const unsigned int budget = 1000 / targetFps;

    // Hard part of the budget: never exceed the nominal frame rate.
    unsigned int elapsed = SDL_GetTicks() - _ticks;
    if (minFrameTime > elapsed) {
        SDL_Delay(minFrameTime - elapsed);
    }

    if (budget <= minFrameTime) {
        return;
    }

    // Soft part: sleep out the rest of the idle budget in short naps,
    // checking for fresh events between them so a touch wakes us up almost
    // immediately. SDL_PollEvent(nullptr) reports pending events without
    // dequeuing them, so the regular pump still sees everything.
    //
    // SDL_WaitEventTimeout would be the natural tool here, but SDL's iOS
    // video driver has no native event wait, so it falls back to polling
    // with SDL_Delay(1) - hundreds of wakeups per second, the opposite of
    // what this limiter is trying to achieve on battery.
    while (true) {
        elapsed = SDL_GetTicks() - _ticks;
        if (elapsed >= budget) {
            break;
        }

        if (SDL_PollEvent(nullptr) != 0) {
            // The user did something - stop idling right away.
            break;
        }

        // Unfocused windows, deep idle and long-untouched sessions tolerate
        // slower reaction - nap in bigger chunks to cut CPU wakeups.
        const unsigned int chunkMs = !gProgramIsActive
            ? 50
            : ((deepIdle || now - _lastActivityTicks > 5000) ? 33 : 16);
        const unsigned int remaining = budget - elapsed;
        if (remaining > 2 * chunkMs) {
            // Only the tail of the budget needs fine-grained polling for
            // input latency; the bulk is one nap (halves the wakeups).
            SDL_Delay(remaining - chunkMs);
            continue;
        }
        SDL_Delay(remaining < chunkMs ? remaining : chunkMs);
    }
}

} // namespace fallout
