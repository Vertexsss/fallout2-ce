#include "touch_overlay.h"

#include <string.h>

#include <SDL.h>

#include "color.h"
#include "input.h"
#include "interface.h"
#include "kb.h"
#include "svga.h"
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
OverlayButton gHltButton;
bool gShown = false;
bool gHighlightActive = false;

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

    // Bottom-left, above the interface bar.
    createButton(gHltButton, kMargin, screenH - INTERFACE_BAR_HEIGHT - kButtonHeight - 10,
        "HLT", kTouchOverlayHighlightKeyCode);
}

void touchOverlayFree()
{
    if (gHighlightActive) {
        pushShiftEvent(false);
        gHighlightActive = false;
    }
    if (gCfgButton.window != -1) {
        windowDestroy(gCfgButton.window);
        gCfgButton.window = -1;
    }
    if (gHltButton.window != -1) {
        windowDestroy(gHltButton.window);
        gHltButton.window = -1;
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
    if (gHltButton.window != -1) {
        windowShow(gHltButton.window);
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
    if (gHltButton.window != -1) {
        windowHide(gHltButton.window);
    }
    gShown = false;
}

bool touchOverlayContainsPoint(int x, int y)
{
    if (!gShown) {
        return false;
    }
    return pointInButton(gCfgButton, x, y) || pointInButton(gHltButton, x, y);
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

    if (pointInButton(gHltButton, x, y)) {
        touchOverlayToggleHighlight();
        return true;
    }

    return false;
}

void touchOverlayToggleHighlight()
{
    gHighlightActive = !gHighlightActive;
    pushShiftEvent(gHighlightActive);
    paintButton(gHltButton, gHighlightActive);
}

void touchOverlayReleaseHighlight()
{
    if (gHighlightActive) {
        touchOverlayToggleHighlight();
    }
}

} // namespace fallout
