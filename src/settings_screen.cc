#include "settings_screen.h"

#include <stdio.h>

#include "eco_cores.h"

#include <string.h>

#include "color.h"
#include "cycle.h"
#include "input.h"
#include "kb.h"
#include "platform/ios/quick_toolbar.h"
#include "settings.h"
#include "svga.h"
#include "text_font.h"
#include "tile_hires_stencil.h"
#include "touch.h"
#include "window_manager.h"

namespace fallout {

static bool gSettingsPrevTouchMode = false;

namespace {

constexpr int kWindowWidth = 380;
constexpr int kTitleHeight = 32;
// Shrinks when the logical screen is short (RENDER SCALE 2X and small
// windows): 17 rows at 30px no longer fit 410-480 lines, and a window
// taller than the screen cannot be created at all - the CFG button then
// silently did nothing.
int gRowHeight = 30;
constexpr int kFooterHeight = 56;
constexpr int kRowPadding = 12;
constexpr int kValueBoxWidth = 110;

constexpr int kKeyRowBase = 500;
constexpr int kKeyClose = 599;

struct Row {
    const char* label;
    // Returns the display text for the current value.
    const char* (*text)();
    // Advances to the next value (cycling) and applies it live if possible.
    void (*next)();
};

// The engine refuses to render below 640x480, silently resetting scale to 1
// - on screens where half resolution is below that, the toggle is a no-op.
bool scaleApplicable()
{
    return settings.screen.resolution_x / 2 >= 640 && settings.screen.resolution_y / 2 >= 480;
}

const char* scaleText()
{
    if (!scaleApplicable()) {
        return "N/A";
    }
    return settings.screen.scale >= 2 ? "2X *" : "1X *";
}

void scaleNext()
{
    if (!scaleApplicable()) {
        return;
    }
    settings.screen.scale = settings.screen.scale >= 2 ? 1 : 2;
}

const char* barModeText()
{
    return settings.ui.iface_bar_mode ? "OVERLAP *" : "CLASSIC *";
}

void barModeNext()
{
    settings.ui.iface_bar_mode = !settings.ui.iface_bar_mode;
}

const char* idleFpsText()
{
    switch (settings.screen.idle_fps) {
    case 60: return "60";
    case 30: return "30";
    default: return "15";
    }
}

void idleFpsNext()
{
    switch (settings.screen.idle_fps) {
    case 15: settings.screen.idle_fps = 30; break;
    case 30: settings.screen.idle_fps = 60; break;
    default: settings.screen.idle_fps = 15; break;
    }
    sharedFpsLimiter.setIdleFps(settings.screen.idle_fps);
}

const char* cycleText()
{
    switch (settings.system.cycle_speed_factor) {
    case 2: return "1/2";
    case 4: return "1/4";
    default: return "1X";
    }
}

void cycleNext()
{
    int next;
    switch (settings.system.cycle_speed_factor) {
    case 1: next = 2; break;
    case 2: next = 4; break;
    default: next = 1; break;
    }
    cycleSetSpeedFactor(next);
}

// Renders the game at 2/3 of the native resolution and lets SDL stretch it
// to the full screen - everything becomes 1.5x bigger. On a 2x retina panel
// this lands exactly on 3 physical pixels per game pixel, so it stays crisp.
bool uiScaleReduced()
{
    return settings.screen.native_resolution_x != 0
        && settings.screen.resolution_x != settings.screen.native_resolution_x;
}

bool uiScaleAvailable()
{
    int nx = settings.screen.native_resolution_x != 0 ? settings.screen.native_resolution_x : settings.screen.resolution_x;
    int ny = settings.screen.native_resolution_y != 0 ? settings.screen.native_resolution_y : settings.screen.resolution_y;
    return nx * 2 / 3 >= 640 && ny * 2 / 3 >= 480;
}

const char* uiScaleText()
{
    if (!uiScaleAvailable()) {
        return "N/A";
    }
    return uiScaleReduced() ? "1.5X *" : "1X *";
}

void uiScaleNext()
{
    if (!uiScaleAvailable()) {
        return;
    }

    if (settings.screen.native_resolution_x == 0) {
        settings.screen.native_resolution_x = settings.screen.resolution_x;
        settings.screen.native_resolution_y = settings.screen.resolution_y;
    }

    if (uiScaleReduced()) {
        settings.screen.resolution_x = settings.screen.native_resolution_x;
        settings.screen.resolution_y = settings.screen.native_resolution_y;
        settings.ui.iface_bar_width = settings.screen.resolution_x >= 800 ? 800 : 640;
    } else {
        settings.screen.resolution_x = settings.screen.native_resolution_x * 2 / 3;
        settings.screen.resolution_y = settings.screen.native_resolution_y * 2 / 3;
        settings.ui.iface_bar_width = settings.screen.resolution_x >= 800 ? 800 : 640;
    }
}

const char* dudePointerText()
{
    return settings.ui.dude_pointer ? "ON" : "OFF";
}

void dudePointerNext()
{
    settings.ui.dude_pointer = !settings.ui.dude_pointer;
}

const char* followHeroText()
{
    return settings.ui.follow_hero ? "ON" : "OFF";
}

void followHeroNext()
{
    settings.ui.follow_hero = !settings.ui.follow_hero;
}

const char* edgeScrollText()
{
    return settings.ui.edge_scroll ? "ON" : "OFF";
}

void edgeScrollNext()
{
    // Read per refresh - applies instantly.
    settings.ui.edge_scroll = !settings.ui.edge_scroll;
}

const char* freeScrollText()
{
    return settings.ui.free_scroll ? "ON" : "OFF";
}

void freeScrollNext()
{
    // Read per scroll attempt - applies instantly, no restart needed.
    settings.ui.free_scroll = !settings.ui.free_scroll;
}

const char* stencilText()
{
    return settings.ui.enable_high_resolution_stencil ? "ON" : "OFF";
}

void stencilNext()
{
    settings.ui.enable_high_resolution_stencil = !settings.ui.enable_high_resolution_stencil;
    tile_hires_stencil_set_enabled(settings.ui.enable_high_resolution_stencil);
}

const char* toolbarText()
{
    return settings.ui.quick_toolbar_visible ? "ON" : "OFF";
}

void toolbarNext()
{
    settings.ui.quick_toolbar_visible = !settings.ui.quick_toolbar_visible;
    quickToolbarSetEnabled(settings.ui.quick_toolbar_visible);
    if (settings.ui.quick_toolbar_visible) {
        quickToolbarShow();
    }
}

const char* fpsCounterText()
{
    return settings.debug.show_fps ? "ON" : "OFF";
}

void fpsCounterNext()
{
    settings.debug.show_fps = !settings.debug.show_fps;
}

const char* fpsCapText()
{
    // The cfg accepts any 30..60 - show what actually applies.
    static char buf[8];
    snprintf(buf, sizeof(buf), "%d", settings.screen.fps_cap);
    return buf;
}

void fpsCapNext()
{
    settings.screen.fps_cap = settings.screen.fps_cap >= 60 ? 30 : 60;
    sharedFpsLimiter.setFpsCap(settings.screen.fps_cap);
}

const char* ecoCoresText()
{
    return settings.ui.eco_cores ? "ON" : "OFF";
}

void ecoCoresNext()
{
    settings.ui.eco_cores = !settings.ui.eco_cores;
    ecoCoresSetEnabled(settings.ui.eco_cores);
}

const char* gpuPanText()
{
    return settings.screen.gpu_iso ? "ON" : "OFF";
}

void gpuPanNext()
{
    settings.screen.gpu_iso = !settings.screen.gpu_iso;
}

const char* autoPowerText()
{
    return settings.ui.auto_power ? "ON" : "OFF";
}

void autoPowerNext()
{
    settings.ui.auto_power = !settings.ui.auto_power;
    sharedFpsLimiter.setAutoPower(settings.ui.auto_power);
}

const char* menuArtText()
{
    return settings.ui.main_menu_classic_art ? "CLASSIC" : "HRP";
}

void menuArtNext()
{
    settings.ui.main_menu_classic_art = !settings.ui.main_menu_classic_art;
}

constexpr Row kRows[] = {
    { "RENDER SCALE", scaleText, scaleNext },
    { "IFACE BAR MODE", barModeText, barModeNext },
    { "IDLE FPS", idleFpsText, idleFpsNext },
    { "FPS CAP", fpsCapText, fpsCapNext },
    { "ECO CORES", ecoCoresText, ecoCoresNext },
    { "AUTO POWER", autoPowerText, autoPowerNext },
    { "GPU PAN", gpuPanText, gpuPanNext },
    { "COLOR CYCLE SPEED", cycleText, cycleNext },
    { "FPS COUNTER", fpsCounterText, fpsCounterNext },
    { "TOUCH TOOLBAR", toolbarText, toolbarNext },
    { "FREE CAMERA", freeScrollText, freeScrollNext },
    { "EDGE SCROLL", edgeScrollText, edgeScrollNext },
    { "FOLLOW HERO", followHeroText, followHeroNext },
    { "MAP STENCIL", stencilText, stencilNext },
    { "PLAYER ARROW", dudePointerText, dudePointerNext },
    { "UI SCALE", uiScaleText, uiScaleNext },
    { "MENU ART", menuArtText, menuArtNext },
};
constexpr int kRowCount = static_cast<int>(sizeof(kRows) / sizeof(kRows[0]));
int gWindowHeight = kTitleHeight + kRowCount * 30 + kFooterHeight;

void fillRect(unsigned char* buffer, int pitch, int x, int y, int w, int h, Color color)
{
    for (int row = 0; row < h; row++) {
        memset(buffer + (y + row) * pitch + x, color, static_cast<size_t>(w));
    }
}

void drawTextAt(unsigned char* buffer, int pitch, int x, int y, const char* text, Color color)
{
    fontDrawText(buffer + y * pitch + x, text, kWindowWidth - x, pitch, color);
}

void paint(int win, int selected)
{
    unsigned char* buffer = windowGetBuffer(win);
    if (buffer == nullptr) {
        return;
    }

    const Color panel = intensityColorTable[COLOR_WHITE][8];
    const Color panelSelected = intensityColorTable[COLOR_WHITE][16];
    const Color border = intensityColorTable[COLOR_WHITE][28];
    const Color textColor = intensityColorTable[COLOR_LIGHT_YELLOW][48];
    const Color labelColor = intensityColorTable[COLOR_WHITE][44];
    const Color dimColor = intensityColorTable[COLOR_WHITE][28];

    fillRect(buffer, kWindowWidth, 0, 0, kWindowWidth, gWindowHeight, panel);
    // outer border
    fillRect(buffer, kWindowWidth, 0, 0, kWindowWidth, 1, border);
    fillRect(buffer, kWindowWidth, 0, gWindowHeight - 1, kWindowWidth, 1, border);
    fillRect(buffer, kWindowWidth, 0, 0, 1, gWindowHeight, border);
    fillRect(buffer, kWindowWidth, kWindowWidth - 1, 0, 1, gWindowHeight, border);

    int lineHeight = fontGetLineHeight();

    const char* title = "SCREEN SETTINGS";
    int titleX = (kWindowWidth - fontGetStringWidth(title)) / 2;
    drawTextAt(buffer, kWindowWidth, titleX, (kTitleHeight - lineHeight) / 2 + 2, title, textColor);
    fillRect(buffer, kWindowWidth, kRowPadding, kTitleHeight - 2, kWindowWidth - 2 * kRowPadding, 1, border);

    for (int i = 0; i < kRowCount; i++) {
        int rowY = kTitleHeight + i * gRowHeight;
        if (i == selected) {
            fillRect(buffer, kWindowWidth, 2, rowY + 1, kWindowWidth - 4, gRowHeight - 2, panelSelected);
        }
        int textY = rowY + (gRowHeight - lineHeight) / 2 + 2;
        drawTextAt(buffer, kWindowWidth, kRowPadding, textY, kRows[i].label, labelColor);

        // value box, right-aligned
        int boxX = kWindowWidth - kRowPadding - kValueBoxWidth;
        fillRect(buffer, kWindowWidth, boxX, rowY + 3, kValueBoxWidth, gRowHeight - 6, panel);
        fillRect(buffer, kWindowWidth, boxX, rowY + 3, kValueBoxWidth, 1, border);
        fillRect(buffer, kWindowWidth, boxX, rowY + gRowHeight - 4, kValueBoxWidth, 1, border);
        fillRect(buffer, kWindowWidth, boxX, rowY + 3, 1, gRowHeight - 6, border);
        fillRect(buffer, kWindowWidth, boxX + kValueBoxWidth - 1, rowY + 3, 1, gRowHeight - 6, border);
        const char* value = kRows[i].text();
        int valueX = boxX + (kValueBoxWidth - fontGetStringWidth(value)) / 2;
        drawTextAt(buffer, kWindowWidth, valueX, textY, value, textColor);
    }

    int footerY = kTitleHeight + kRowCount * gRowHeight;
    drawTextAt(buffer, kWindowWidth, kRowPadding, footerY + 6, "* TAKES EFFECT AFTER RESTART", dimColor);

    // close button
    const char* closeLabel = "CLOSE";
    int closeW = 90;
    int closeX = (kWindowWidth - closeW) / 2;
    int closeY = footerY + kFooterHeight - 32;
    fillRect(buffer, kWindowWidth, closeX, closeY, closeW, 24, intensityColorTable[COLOR_WHITE][12]);
    fillRect(buffer, kWindowWidth, closeX, closeY, closeW, 1, border);
    fillRect(buffer, kWindowWidth, closeX, closeY + 23, closeW, 1, border);
    fillRect(buffer, kWindowWidth, closeX, closeY, 1, 24, border);
    fillRect(buffer, kWindowWidth, closeX + closeW - 1, closeY, 1, 24, border);
    drawTextAt(buffer, kWindowWidth, closeX + (closeW - fontGetStringWidth(closeLabel)) / 2, closeY + (24 - lineHeight) / 2 + 2, closeLabel, textColor);

    windowRefresh(win);
}

} // namespace

void settingsScreenShow()
{
    int oldFont = fontGetCurrent();
    fontSetCurrent(101);

    gRowHeight = (screenGetHeight() - kTitleHeight - kFooterHeight) / kRowCount;
    if (gRowHeight > 30) {
        gRowHeight = 30;
    }
    // Never below the row's own text (a replacement font can be taller).
    int minRowHeight = fontGetLineHeight() + 4;
    if (minRowHeight < 14) {
        minRowHeight = 14;
    }
    if (gRowHeight < minRowHeight) {
        gRowHeight = minRowHeight;
    }
    gWindowHeight = kTitleHeight + kRowCount * gRowHeight + kFooterHeight;

    int windowX = (screenGetWidth() - kWindowWidth) / 2;
    int windowY = (screenGetHeight() - gWindowHeight) / 2;
    if (windowY < 0) {
        windowY = 0;
    }
    int win = windowCreate(windowX, windowY, kWindowWidth, gWindowHeight, COLOR_BLACK, WINDOW_MODAL | WINDOW_MOVE_ON_TOP);
    if (win == -1) {
        return;
    }

    // Like the other UI screens: taps land where the finger is, not where
    // the trackpad-style world cursor happens to be.
    gSettingsPrevTouchMode = touch_get_touchscreen_mode();
    touch_set_touchscreen_mode(true);

    // invisible hotspot buttons: whole row cycles the value, footer closes
    for (int i = 0; i < kRowCount; i++) {
        buttonCreate(win, 2, kTitleHeight + i * gRowHeight + 1, kWindowWidth - 4, gRowHeight - 2,
            -1, -1, -1, kKeyRowBase + i);
    }
    int footerY = kTitleHeight + kRowCount * gRowHeight;
    buttonCreate(win, (kWindowWidth - 90) / 2, footerY + kFooterHeight - 32, 90, 24,
        -1, -1, -1, kKeyClose);

    int selected = 0;
    paint(win, selected);

    bool done = false;
    while (!done) {
        sharedFpsLimiter.mark();

        int keyCode = inputGetInput();
        if (keyCode == KEY_ESCAPE || keyCode == kKeyClose) {
            done = true;
        } else if (keyCode >= kKeyRowBase && keyCode < kKeyRowBase + kRowCount) {
            selected = keyCode - kKeyRowBase;
            kRows[selected].next();
            paint(win, selected);
        } else if (keyCode == KEY_ARROW_UP) {
            selected = (selected + kRowCount - 1) % kRowCount;
            paint(win, selected);
        } else if (keyCode == KEY_ARROW_DOWN) {
            selected = (selected + 1) % kRowCount;
            paint(win, selected);
        } else if (keyCode == KEY_ARROW_LEFT || keyCode == KEY_ARROW_RIGHT || keyCode == KEY_RETURN) {
            kRows[selected].next();
            paint(win, selected);
        }

        renderPresent();
        sharedFpsLimiter.throttle();
    }

    settingsSave();

    touch_set_touchscreen_mode(gSettingsPrevTouchMode);
    fontSetCurrent(oldFont);
    windowDestroy(win);
}

} // namespace fallout
