#include "touch_overlay.h"

#include <math.h>
#include <string.h>

#include <SDL.h>

#include <algorithm>
#include "art.h"
#include "draw.h"
#include "game_mouse.h"
#include "color.h"
#include "input.h"
#include "interface.h"
#include "kb.h"
#include "map.h"
#include "map_defs.h"
#include "object.h"
#include "settings.h"
#include "svga.h"
#include "tile.h"
#include "text_font.h"
#include "window_manager.h"

namespace fallout {

namespace {

constexpr int kButtonWidth = 44;
constexpr int kButtonHeight = 26;
constexpr int kMargin = 8;

struct OverlayButton {
    int window = -1;
    int x = 0;
    int y = 0;
    const char* label = nullptr;
    int keyCode = 0;
};

OverlayButton gCfgButton;
bool gShown = false;
bool gHighlightActive = false;

// Edge-of-screen arrow pointing toward the off-screen player.
int gPointerSize = 30;
constexpr int kPointerMargin = 44;
int gPointerWindow = -1;
int gPointerX = 0;
int gPointerY = 0;
int gPointerDir = -1;
bool gPointerShown = false;

void fillRect(unsigned char* buffer, int pitch, int x, int y, int w, int h, Color color)
{
    for (int row = 0; row < h; row++) {
        memset(buffer + (y + row) * pitch + x, color, static_cast<size_t>(w));
    }
}

void paintButton(const OverlayButton& button, bool active)
{
    if (button.window == -1) {
        return;
    }

    unsigned char* buffer = windowGetBuffer(button.window);
    if (buffer == nullptr) {
        return;
    }

    const Color panel = intensityColorTable[COLOR_WHITE][active ? 20 : 8];
    const Color border = intensityColorTable[COLOR_WHITE][28];
    const Color text = active
        ? intensityColorTable[COLOR_LIGHT_GREEN][48]
        : intensityColorTable[COLOR_LIGHT_YELLOW][48];

    fillRect(buffer, kButtonWidth, 0, 0, kButtonWidth, kButtonHeight, panel);
    fillRect(buffer, kButtonWidth, 0, 0, kButtonWidth, 1, border);
    fillRect(buffer, kButtonWidth, 0, kButtonHeight - 1, kButtonWidth, 1, border);
    fillRect(buffer, kButtonWidth, 0, 0, 1, kButtonHeight, border);
    fillRect(buffer, kButtonWidth, kButtonWidth - 1, 0, 1, kButtonHeight, border);

    int oldFont = fontGetCurrent();
    fontSetCurrent(101);
    int tx = (kButtonWidth - fontGetStringWidth(button.label)) / 2;
    int ty = (kButtonHeight - fontGetLineHeight()) / 2 + 2;
    fontDrawText(buffer + ty * kButtonWidth + tx, button.label, kButtonWidth - tx, kButtonWidth, text);
    fontSetCurrent(oldFont);

    windowRefresh(button.window);
}

void createButton(OverlayButton& button, int x, int y, const char* label, int keyCode)
{
    button.x = x;
    button.y = y;
    button.label = label;
    button.keyCode = keyCode;
    button.window = windowCreate(x, y, kButtonWidth, kButtonHeight, COLOR_BLACK, WINDOW_HIDDEN | WINDOW_TRANSPARENT);
    if (button.window == -1) {
        return;
    }

    // Regular engine button so desktop clicks work through the window
    // manager; on iOS taps are routed via touchOverlayHandleTap instead.
    buttonCreate(button.window, 0, 0, kButtonWidth, kButtonHeight, -1, -1, -1, keyCode);
    paintButton(button, false);
}

bool pointInButton(const OverlayButton& button, int x, int y)
{
    return button.window != -1
        && x >= button.x && x < button.x + kButtonWidth
        && y >= button.y && y < button.y + kButtonHeight;
}

// Direction index (0 = east, clockwise on screen) -> scroll cursor.
int pointerCursorForDir(int dir)
{
    static const int kCursors[8] = {
        MOUSE_CURSOR_SCROLL_E,
        MOUSE_CURSOR_SCROLL_SE,
        MOUSE_CURSOR_SCROLL_S,
        MOUSE_CURSOR_SCROLL_SW,
        MOUSE_CURSOR_SCROLL_W,
        MOUSE_CURSOR_SCROLL_NW,
        MOUSE_CURSOR_SCROLL_N,
        MOUSE_CURSOR_SCROLL_NE,
    };
    return kCursors[dir & 7];
}

void paintPointer(int dir)
{
    if (gPointerWindow == -1) {
        return;
    }

    unsigned char* buffer = windowGetBuffer(gPointerWindow);
    if (buffer == nullptr) {
        return;
    }

    memset(buffer, 0, static_cast<size_t>(gPointerSize) * gPointerSize);

    // The engine's own directional scroll cursors (the arrows the world
    // map and the map edges show) - the same arrow art, drawn by the
    // original artists for all 8 directions - centered in the window.
    int cursorFid = gameMouseGetCursorFid(pointerCursorForDir(dir));
    if (cursorFid != -1) {
        CacheEntry* handle;
        Art* art = artLock(cursorFid, &handle);
        if (art != nullptr) {
            int w = artGetWidth(art, 0, ROTATION_NE);
            int h = artGetHeight(art, 0, ROTATION_NE);
            unsigned char* data = artGetFrameData(art, 0, ROTATION_NE);
            if (data != nullptr && w > 0 && h > 0 && w <= gPointerSize && h <= gPointerSize) {
                int x0 = (gPointerSize - w) / 2;
                int y0 = (gPointerSize - h) / 2;
                blitBufferToBufferTrans(data, w, h, w, buffer + y0 * gPointerSize + x0, gPointerSize);
                artUnlock(handle);
                windowRefresh(gPointerWindow);
                return;
            }
            artUnlock(handle);
        }
    }

    // Fallback: filled triangle pointing along one of 8 directions.
    static const int kDirX[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
    static const int kDirY[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };

    double len = (kDirX[dir] != 0 && kDirY[dir] != 0) ? 0.7071 : 1.0;
    double dx = kDirX[dir] * len;
    double dy = kDirY[dir] * len;
    double cx = gPointerSize / 2.0;
    double cy = gPointerSize / 2.0;

    double tipX = cx + dx * 12.0;
    double tipY = cy + dy * 12.0;
    double baseX = cx - dx * 6.0;
    double baseY = cy - dy * 6.0;
    double perpX = -dy;
    double perpY = dx;
    double aX = baseX + perpX * 8.0;
    double aY = baseY + perpY * 8.0;
    double bX = baseX - perpX * 8.0;
    double bY = baseY - perpY * 8.0;

    const Color fill = intensityColorTable[COLOR_LIGHT_YELLOW][48];

    for (int y = 0; y < gPointerSize; y++) {
        for (int x = 0; x < gPointerSize; x++) {
            double d1 = (x - tipX) * (aY - tipY) - (y - tipY) * (aX - tipX);
            double d2 = (x - aX) * (bY - aY) - (y - aY) * (bX - aX);
            double d3 = (x - bX) * (tipY - bY) - (y - bY) * (tipX - bX);
            bool neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
            bool pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
            if (!(neg && pos)) {
                buffer[y * gPointerSize + x] = fill;
            }
        }
    }

    windowRefresh(gPointerWindow);
}

void pointerHide()
{
    if (gPointerShown && gPointerWindow != -1) {
        windowHide(gPointerWindow);
        gPointerShown = false;
    }
}

// Runs every frame as a ticker: shows the arrow at the screen edge in the
// player's direction while the player is scrolled out of view.
void pointerTick()
{
    if (!gShown || gPointerWindow == -1 || !settings.ui.dude_pointer
        || gDude == nullptr || gDude->tile == -1) {
        pointerHide();
        return;
    }

    int dudeX;
    int dudeY;
    tileToScreenXY(gDude->tile, &dudeX, &dudeY);
    dudeX += 16;
    dudeY += 8;

    int viewW = screenGetWidth();
    int viewH = screenGetVisibleHeight();

    if (dudeX >= 0 && dudeX < viewW && dudeY >= 0 && dudeY < viewH) {
        pointerHide();
        return;
    }

    int px = dudeX;
    int py = dudeY;
    if (px < kPointerMargin) px = kPointerMargin;
    if (px > viewW - kPointerMargin - gPointerSize) px = viewW - kPointerMargin - gPointerSize;
    if (py < kPointerMargin) py = kPointerMargin;
    if (py > viewH - kPointerMargin - gPointerSize) py = viewH - kPointerMargin - gPointerSize;

    double angle = atan2(static_cast<double>(dudeY - (viewH / 2)), static_cast<double>(dudeX - (viewW / 2)));
    int dir = static_cast<int>(floor(angle / (3.14159265 / 4.0) + 0.5)) & 7;

    if (!gPointerShown) {
        windowShow(gPointerWindow);
        gPointerShown = true;
        gPointerDir = -1;
    }

    if (px != gPointerX || py != gPointerY) {
        gPointerX = px;
        gPointerY = py;
        windowSetPosition(gPointerWindow, px, py);
    }

    if (dir != gPointerDir) {
        gPointerDir = dir;
        paintPointer(dir);
    }
}

// Outlines every item lying on the ground on the current elevation using
// the engine's native OUTLINE_TYPE_ITEM (the same machinery combat uses for
// targets). The simulated Shift alone only works when a mod like FO2Tweaks
// provides highlighting - a vanilla install has nothing listening to it.
void applyItemOutlinesOnElevation(bool enable, int elevation)
{
    Object** objects = nullptr;
    int count = objectListCreate(-1, elevation, OBJ_TYPE_ITEM, &objects);

    for (int i = 0; i < count; i++) {
        Object* object = objects[i];
        if (object->tile == -1) {
            // In somebody's inventory, not on the map.
            continue;
        }
        if ((object->flags & OBJECT_HIDDEN) != 0 || (object->flags & OBJECT_NO_HIGHLIGHT) != 0) {
            continue;
        }

        Rect rect;
        if (enable) {
            if (!objectHasOutline(object)) {
                objectSetOutline(object, OUTLINE_TYPE_ITEM, &rect);
            }
        } else {
            if ((object->outline & OUTLINE_TYPE_MAX) == OUTLINE_TYPE_ITEM) {
                objectClearOutline(object, &rect);
            }
        }
    }

    if (objects != nullptr) {
        objectListFree(objects);
    }
}

void applyItemOutlines(bool enable)
{
    if (enable) {
        applyItemOutlinesOnElevation(true, gElevation);
    } else {
        // Clear everywhere: the player may have changed elevation while the
        // highlight was on, and outlines must not linger on other levels.
        for (int elevation = 0; elevation < ELEVATION_COUNT; elevation++) {
            applyItemOutlinesOnElevation(false, elevation);
        }
    }

    tileWindowRefresh();
}

void pushShiftEvent(bool down)
{
    // FO2tweaks' highlighting reads key state via sfall's key_pressed(),
    // which tracks real SDL key events - same mechanism as the
    // three-finger long-press gesture.
    SDL_Event ev;
    SDL_zero(ev);
    ev.key.keysym.scancode = SDL_SCANCODE_LSHIFT;
    ev.key.keysym.sym = SDLK_LSHIFT;
    ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    SDL_PushEvent(&ev);
}

} // namespace

void touchOverlayInit()
{
    int screenW = screenGetWidth();
    int screenH = screenGetHeight();

    // Top-right corner, below the sfall difficulty indicator line.
    createButton(gCfgButton, screenW - kButtonWidth - kMargin, 36, "CFG", KEY_F11);


    // Size the pointer window for the largest of the 8 scroll cursors.
    gPointerSize = 30;
    for (int dir = 0; dir < 8; dir++) {
        int fid = gameMouseGetCursorFid(pointerCursorForDir(dir));
        CacheEntry* handle;
        Art* art = fid != -1 ? artLock(fid, &handle) : nullptr;
        if (art != nullptr) {
            gPointerSize = std::max(gPointerSize, std::max(artGetWidth(art, 0, ROTATION_NE), artGetHeight(art, 0, ROTATION_NE)) + 2);
            artUnlock(handle);
        }
    }

    gPointerWindow = windowCreate(0, 0, gPointerSize, gPointerSize, COLOR_BLACK,
        WINDOW_HIDDEN | WINDOW_TRANSPARENT | WINDOW_MOVE_ON_TOP);
    if (gPointerWindow != -1) {
        buttonCreate(gPointerWindow, 0, 0, gPointerSize, gPointerSize, -1, -1, -1, kTouchOverlayCenterKeyCode);
    }
    gPointerShown = false;

    tickersAdd(pointerTick);
}

void touchOverlayFree()
{
    tickersRemove(pointerTick);

    if (gPointerWindow != -1) {
        windowDestroy(gPointerWindow);
        gPointerWindow = -1;
    }
    gPointerShown = false;

    if (gHighlightActive) {
        pushShiftEvent(false);
        gHighlightActive = false;
    }
    if (gCfgButton.window != -1) {
        windowDestroy(gCfgButton.window);
        gCfgButton.window = -1;
    }
    gShown = false;
}

void touchOverlayShow()
{
    if (gShown) {
        return;
    }
    if (gCfgButton.window != -1) {
        windowShow(gCfgButton.window);
    }
    gShown = true;
}

void touchOverlayHide()
{
    if (!gShown) {
        return;
    }

    // Do not leave a simulated Shift stuck while the buttons are gone.
    if (gHighlightActive) {
        touchOverlayToggleHighlight();
    }

    if (gCfgButton.window != -1) {
        windowHide(gCfgButton.window);
    }
    pointerHide();
    gShown = false;
}

bool touchOverlayContainsPoint(int x, int y)
{
    if (!gShown) {
        return false;
    }
    if (pointInButton(gCfgButton, x, y)) {
        return true;
    }
    return gPointerShown
        && x >= gPointerX && x < gPointerX + gPointerSize
        && y >= gPointerY && y < gPointerY + gPointerSize;
}

bool touchOverlayHandleTap(int x, int y)
{
    if (!gShown) {
        return false;
    }

    if (pointInButton(gCfgButton, x, y)) {
        // Route through the regular key path so the modal settings screen
        // opens from the main loop, not from touch handling.
        enqueueInputEvent(KEY_F11);
        return true;
    }


    if (gPointerShown
        && x >= gPointerX && x < gPointerX + gPointerSize
        && y >= gPointerY && y < gPointerY + gPointerSize) {
        touchOverlayCenterOnDude();
        return true;
    }

    return false;
}

void touchOverlayToggleHighlight()
{
    // Switch-style: outlines only. No simulated Shift is held, so the
    // highlight can stay on indefinitely without slowing movement to a
    // walk, and no on-screen button is needed.
    gHighlightActive = !gHighlightActive;
    applyItemOutlines(gHighlightActive);
}


void touchOverlayCenterOnDude()
{
    if (gDude != nullptr && gDude->tile != -1) {
        tileSetCenter(gDude->tile, TILE_SET_CENTER_REFRESH_WINDOW | TILE_SET_CENTER_FLAG_IGNORE_SCROLL_RESTRICTIONS | TILE_SET_CENTER_FLAG_ALLOW_HIRES_TWEAK);
    }
}

void touchOverlayReleaseHighlight()
{
    if (gHighlightActive) {
        touchOverlayToggleHighlight();
    }
}

} // namespace fallout
