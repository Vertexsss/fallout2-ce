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

void FpsLimiter::throttle()
{
    unsigned int now = SDL_GetTicks();

    unsigned int targetFps = _fps;
    if (!gProgramIsActive) {
        // Window is not focused (backgrounded, Stage Manager, Slide Over).
        targetFps = 5;
    } else if (now - _lastActivityTicks > _idleGraceMs) {
        // Nothing has been drawn for a while - the screen is static.
        targetFps = _idleFps;
    }

    if (targetFps == 0) {
        targetFps = 1;
    }

    const unsigned int minFrameTime = 1000 / _fps;
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

        // Unfocused windows tolerate slower reaction - nap in bigger chunks.
        const unsigned int chunkMs = gProgramIsActive ? 16 : 50;
        const unsigned int remaining = budget - elapsed;
        SDL_Delay(remaining < chunkMs ? remaining : chunkMs);
    }
}

} // namespace fallout
