#include "camera_follow.h"

#include <math.h>
#include <stdlib.h>

#include <SDL.h>

#include "animation.h"
#include "critter.h"
#include "fps_limiter.h"
#include "map.h"
#include "object.h"
#include "settings.h"
#include "svga.h"
#include "tile.h"
#include "touch.h"

namespace fallout {

namespace {

// A walk only engages the follow when the player stands within this
// fraction of the view's half-size from its center ("medium radius").
constexpr double kStartZone = 0.5;

// Time constant of the glide toward the player.
constexpr double kGlideTauMs = 160.0;

bool gActive = false;
unsigned int gLastTicks = 0;
double gCarryX = 0.0;
double gCarryY = 0.0;

// Player's offset from the center of the visible world area, in pixels.
void dudeOffsetFromCenter(int* dx, int* dy)
{
    int x;
    int y;
    tileToScreenXY(gDude->tile, &x, &y);
    // Tile art origin -> hex center.
    x += 16;
    y += 8;
    *dx = x - screenGetWidth() / 2;
    *dy = y - screenGetVisibleHeight() / 2;
}

} // namespace

void cameraFollowOnWalkRegistered(Object* owner)
{
    if (owner == nullptr || owner != gDude || !settings.ui.follow_hero || gDude->tile == -1) {
        return;
    }

    int dx;
    int dy;
    dudeOffsetFromCenter(&dx, &dy);

    int zoneX = static_cast<int>(screenGetWidth() / 2 * kStartZone);
    int zoneY = static_cast<int>(screenGetVisibleHeight() / 2 * kStartZone);
    bool inZone = abs(dx) <= zoneX && abs(dy) <= zoneY;

    // A walk that starts off-center (or off-screen) means the player is
    // looking elsewhere on purpose - do not yank the view back.
    gActive = inZone;
    gLastTicks = SDL_GetTicks();
    gCarryX = 0.0;
    gCarryY = 0.0;
}

void cameraFollowCancel()
{
    gActive = false;
}

void cameraFollowTick()
{
    if (!gActive) {
        return;
    }

    if (!settings.ui.follow_hero || gDude == nullptr || gDude->tile == -1 || !animationIsBusy(gDude)) {
        // Walk finished (or the feature was switched off mid-walk).
        gActive = false;
        return;
    }

    // A UI screen over the world (inventory, dialog, pipboy) freezes the
    // walk; do not drag the map behind it - resume when it closes.
    if (isoIsDisabled() || touch_get_touchscreen_mode()) {
        gLastTicks = SDL_GetTicks();
        return;
    }

    unsigned int now = SDL_GetTicks();
    unsigned int dt = now - gLastTicks;
    gLastTicks = now;
    if (dt == 0) {
        return;
    }
    if (dt > 100) {
        dt = 100;
    }

    int dx;
    int dy;
    dudeOffsetFromCenter(&dx, &dy);
    if (abs(dx) <= 1 && abs(dy) <= 1) {
        return;
    }

    // Exponential glide: cover a fixed fraction of the remaining offset per
    // time constant, with sub-pixel carry so slow drifts still add up.
    double k = 1.0 - exp(-static_cast<double>(dt) / kGlideTauMs);
    gCarryX += dx * k;
    gCarryY += dy * k;
    int stepX = static_cast<int>(gCarryX);
    int stepY = static_cast<int>(gCarryY);
    gCarryX -= stepX;
    gCarryY -= stepY;
    if (stepX == 0 && stepY == 0) {
        return;
    }

    if (mapScrollPixels(stepX, stepY) == -1) {
        // Map edge or scroll restriction: let the player walk on alone.
        gActive = false;
        return;
    }

    // The glide is watched motion - keep the frame rate up while it runs.
    sharedFpsLimiter.notifyActivity();
}

} // namespace fallout
