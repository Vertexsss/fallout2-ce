#include "display_monitor.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include <math.h>
#include <algorithm>
#include <vector>
#include "fps_limiter.h"
#include <SDL.h>

#include <string.h>

#include <fstream>
#include <string>

#include "art.h"
#include "color.h"
#include "combat.h"
#include "draw.h"
#include "game_mouse.h"
#include "game_sound.h"
#include "geometry.h"
#include "input.h"
#include "interface.h"
#include "memory.h"
#include "settings.h"
#include "svga.h"
#include "text_font.h"
#include "window_manager.h"

namespace fallout {

// The maximum number of lines display monitor can hold. Once this value
// is reached earlier messages are thrown away.
#define DISPLAY_MONITOR_LINES_CAPACITY (100)

// The maximum length of a string in display monitor (in characters).
#define DISPLAY_MONITOR_LINE_LENGTH (80)

#define DISPLAY_MONITOR_X (23)
#define DISPLAY_MONITOR_Y (24)
#define DISPLAY_MONITOR_WIDTH (167 + gInterfaceBarContentOffset)
#define DISPLAY_MONITOR_HEIGHT (60)

#define DISPLAY_MONITOR_HALF_HEIGHT (DISPLAY_MONITOR_HEIGHT / 2)

#define DISPLAY_MONITOR_FONT (101)

#define DISPLAY_MONITOR_BEEP_DELAY (500U)

static void display_clear();
static void displayMonitorRefresh();
static void displayMonitorSetScrollPixels(int scrollPx);
static void displayMonitorSnapToLine();
static void displayMonitorTouchUpdate();
static int displayMonitorMaxScrollPixels();
static void displayMonitorScrollUpOnMouseDown(int btn, int keyCode);
static void displayMonitorScrollDownOnMouseDown(int btn, int keyCode);
static void displayMonitorScrollUpOnMouseEnter(int btn, int keyCode);
static void displayMonitorScrollDownOnMouseEnter(int btn, int keyCode);
static void displayMonitorOnMouseExit(int btn, int keyCode);

static void consoleFileInit();
static void consoleFileReset();
static void consoleFileExit();
static void consoleFileAddMessage(const char* message);
static void consoleFileFlush();

// 0x51850C disp_init
static bool gDisplayMonitorInitialized = false;

// The rectangle that display monitor occupies in the main interface window.
//
// 0x518510 disp_rect
static Rect gDisplayMonitorRect;

// 0x518520 dn_bid
static int gDisplayMonitorScrollDownButton = -1;

// 0x518524 up_bid
static int gDisplayMonitorScrollUpButton = -1;

// 0x56DBFC display_string_buf
static char gDisplayMonitorLines[DISPLAY_MONITOR_LINES_CAPACITY][DISPLAY_MONITOR_LINE_LENGTH];

// 0x56FB3C disp_buf
static unsigned char* gDisplayMonitorBackgroundFrmData;

// 0x56FB40 max_disp
static int _max_disp;

// Touch scrolling: position in pixels back into history (0 = newest lines
// at the bottom), plus inertia state. The vanilla up/down arrows move it a
// whole line at a time; a finger moves it per pixel.
static int gDisplayMonitorScrollPx = 0;
static double gDisplayMonitorFlingVy = 0.0;
static double gDisplayMonitorFlingCarry = 0.0;
static unsigned int gDisplayMonitorFlingTicks = 0;
static bool gDisplayMonitorTouchDragging = false;

// 0x56FB44 display_enabled
static bool gDisplayMonitorEnabled;

// 0x56FB48 disp_curr
static int _disp_curr;

// 0x56FB4C intface_full_width
static int _intface_full_width;

// 0x56FB50 max
static int gDisplayMonitorLinesCapacity;

// 0x56FB54 disp_start
static int _disp_start;

// 0x56FB58 lastTime
static unsigned int gDisplayMonitorLastBeepTimestamp;

static std::ofstream gConsoleFileStream;
static int gConsoleFilePrintCount = 0;

// 0x431610 display_init
int displayMonitorInit()
{
    if (!gDisplayMonitorInitialized) {
        gDisplayMonitorRect = {
            DISPLAY_MONITOR_X,
            DISPLAY_MONITOR_Y,
            DISPLAY_MONITOR_X + DISPLAY_MONITOR_WIDTH - 1,
            DISPLAY_MONITOR_Y + DISPLAY_MONITOR_HEIGHT - 1,
        };

        int oldFont = fontGetCurrent();
        fontSetCurrent(DISPLAY_MONITOR_FONT);

        gDisplayMonitorLinesCapacity = DISPLAY_MONITOR_LINES_CAPACITY;
        gDisplayMonitorScrollPx = 0;
        gDisplayMonitorFlingVy = 0.0;
        gDisplayMonitorTouchDragging = false;
        tickersAdd(displayMonitorTouchUpdate);
        _max_disp = DISPLAY_MONITOR_HEIGHT / fontGetLineHeight();
        _disp_start = 0;
        _disp_curr = 0;
        fontSetCurrent(oldFont);

        gDisplayMonitorBackgroundFrmData = (unsigned char*)internal_malloc(DISPLAY_MONITOR_WIDTH * DISPLAY_MONITOR_HEIGHT);
        if (gDisplayMonitorBackgroundFrmData == nullptr) {
            return -1;
        }

        if (gInterfaceBarIsCustom) {
            _intface_full_width = gInterfaceBarWidth;
            blitBufferToBuffer(customInterfaceBarGetBackgroundImageData() + gInterfaceBarWidth * DISPLAY_MONITOR_Y + DISPLAY_MONITOR_X,
                DISPLAY_MONITOR_WIDTH,
                DISPLAY_MONITOR_HEIGHT,
                gInterfaceBarWidth,
                gDisplayMonitorBackgroundFrmData,
                DISPLAY_MONITOR_WIDTH);
        } else {
            FrmImage backgroundFrmImage;
            int backgroundFid = buildFid(OBJ_TYPE_INTERFACE, 16);
            if (!backgroundFrmImage.lock(backgroundFid)) {
                internal_free(gDisplayMonitorBackgroundFrmData);
                return -1;
            }

            unsigned char* backgroundFrmData = backgroundFrmImage.getData();
            _intface_full_width = backgroundFrmImage.getWidth();

            blitBufferToBuffer(backgroundFrmData + _intface_full_width * DISPLAY_MONITOR_Y + DISPLAY_MONITOR_X,
                DISPLAY_MONITOR_WIDTH,
                DISPLAY_MONITOR_HEIGHT,
                _intface_full_width,
                gDisplayMonitorBackgroundFrmData,
                DISPLAY_MONITOR_WIDTH);
        }

        gDisplayMonitorScrollUpButton = buttonCreate(gInterfaceBarWindow,
            DISPLAY_MONITOR_X,
            DISPLAY_MONITOR_Y,
            DISPLAY_MONITOR_WIDTH,
            DISPLAY_MONITOR_HALF_HEIGHT,
            -1,
            -1,
            -1,
            -1,
            nullptr,
            nullptr,
            nullptr,
            0);
        if (gDisplayMonitorScrollUpButton != -1) {
            buttonSetMouseCallbacks(gDisplayMonitorScrollUpButton,
                displayMonitorScrollUpOnMouseEnter,
                displayMonitorOnMouseExit,
                displayMonitorScrollUpOnMouseDown,
                nullptr);
        }

        gDisplayMonitorScrollDownButton = buttonCreate(gInterfaceBarWindow,
            DISPLAY_MONITOR_X,
            DISPLAY_MONITOR_Y + DISPLAY_MONITOR_HALF_HEIGHT,
            DISPLAY_MONITOR_WIDTH,
            DISPLAY_MONITOR_HEIGHT - DISPLAY_MONITOR_HALF_HEIGHT,
            -1,
            -1,
            -1,
            -1,
            nullptr,
            nullptr,
            nullptr,
            0);
        if (gDisplayMonitorScrollDownButton != -1) {
            buttonSetMouseCallbacks(gDisplayMonitorScrollDownButton,
                displayMonitorScrollDownOnMouseEnter,
                displayMonitorOnMouseExit,
                displayMonitorScrollDownOnMouseDown,
                nullptr);
        }

        gDisplayMonitorEnabled = true;
        gDisplayMonitorInitialized = true;

        // NOTE: Uninline.
        display_clear();

        // SFALL
        consoleFileInit();
    }

    return 0;
}

// 0x431800 display_reset
int displayMonitorReset()
{
    // NOTE: Uninline.
    display_clear();

    // SFALL
    consoleFileReset();

    return 0;
}

// 0x43184C display_exit
void displayMonitorExit()
{
    tickersRemove(displayMonitorTouchUpdate);
    if (gDisplayMonitorInitialized) {
        // SFALL
        consoleFileExit();

        internal_free(gDisplayMonitorBackgroundFrmData);
        gDisplayMonitorInitialized = false;
    }
}

// 0x43186C display_print
void displayMonitorAddMessage(const char* str)
{
    if (!gDisplayMonitorInitialized || str == nullptr) {
        return;
    }

    // SFALL
    consoleFileAddMessage(str);

    int oldFont = fontGetCurrent();
    fontSetCurrent(DISPLAY_MONITOR_FONT);

    char knob = '\x95';

    char knobString[2];
    knobString[0] = knob;
    knobString[1] = '\0';
    int knobWidth = fontGetStringWidth(knobString);

    if (!isInCombat()) {
        unsigned int now = _get_bk_time();
        if (getTicksBetween(now, gDisplayMonitorLastBeepTimestamp) >= DISPLAY_MONITOR_BEEP_DELAY) {
            gDisplayMonitorLastBeepTimestamp = now;
            soundPlayFile("monitor");
        }
    }

    std::string mutableMessage(str);
    char* mutableStr = mutableMessage.data();

    // TODO: Refactor these two loops.
    char* splitPos = nullptr;
    while (true) {
        while (fontGetStringWidth(mutableStr) <= DISPLAY_MONITOR_WIDTH - _max_disp - knobWidth) {
            char* temp = gDisplayMonitorLines[_disp_start];
            int length;
            if (knob != '\0') {
                *temp++ = knob;
                length = DISPLAY_MONITOR_LINE_LENGTH - 2;
                knob = '\0';
                knobWidth = 0;
            } else {
                length = DISPLAY_MONITOR_LINE_LENGTH - 1;
            }
            strncpy(temp, mutableStr, length);
            gDisplayMonitorLines[_disp_start][DISPLAY_MONITOR_LINE_LENGTH - 1] = '\0';
            _disp_start = (_disp_start + 1) % gDisplayMonitorLinesCapacity;

            if (splitPos == nullptr) {
                fontSetCurrent(oldFont);
                if (gDisplayMonitorTouchDragging || gDisplayMonitorFlingVy != 0.0) {
                    gDisplayMonitorScrollPx = std::min(gDisplayMonitorScrollPx + fontGetLineHeight(), displayMonitorMaxScrollPixels());
                } else {
                    gDisplayMonitorScrollPx = 0;
                }
                _disp_curr = _disp_start;
                displayMonitorRefresh();
                return;
            }

            mutableStr = splitPos + 1;
            *splitPos = ' ';
            splitPos = nullptr;
        }

        char* space = strrchr(mutableStr, ' ');
        if (space == nullptr) {
            break;
        }

        if (splitPos != nullptr) {
            *splitPos = ' ';
        }

        splitPos = space;
        if (space != nullptr) {
            *space = '\0';
        }
    }

    char* temp = gDisplayMonitorLines[_disp_start];
    int length;
    if (knob != '\0') {
        temp++;
        gDisplayMonitorLines[_disp_start][0] = knob;
        length = DISPLAY_MONITOR_LINE_LENGTH - 2;
        knob = '\0';
    } else {
        length = DISPLAY_MONITOR_LINE_LENGTH - 1;
    }
    strncpy(temp, mutableStr, length);

    gDisplayMonitorLines[_disp_start][DISPLAY_MONITOR_LINE_LENGTH - 1] = '\0';
    _disp_start = (_disp_start + 1) % gDisplayMonitorLinesCapacity;

    fontSetCurrent(oldFont);
    if (gDisplayMonitorTouchDragging || gDisplayMonitorFlingVy != 0.0) {
        // The player is scrolling the log right now - keep what they look at
        // (the ring shifted by one line under them).
        gDisplayMonitorScrollPx = std::min(gDisplayMonitorScrollPx + fontGetLineHeight(), displayMonitorMaxScrollPixels());
    } else {
        gDisplayMonitorScrollPx = 0;
    }
    _disp_curr = _disp_start;
    displayMonitorRefresh();
}

// NOTE: Inlined.
//
// 0x431A2C display_clear
static void display_clear()
{
    int index;

    if (gDisplayMonitorInitialized) {
        for (index = 0; index < gDisplayMonitorLinesCapacity; index++) {
            gDisplayMonitorLines[index][0] = '\0';
        }

        _disp_start = 0;
        _disp_curr = 0;
        gDisplayMonitorScrollPx = 0;
        gDisplayMonitorFlingVy = 0.0;
        displayMonitorRefresh();
    }
}

// 0x431A78 display_redraw
static void displayMonitorRefresh()
{
    if (!gDisplayMonitorInitialized) {
        return;
    }

    unsigned char* buf = windowGetBuffer(gInterfaceBarWindow);
    if (buf == nullptr) {
        return;
    }

    buf += _intface_full_width * DISPLAY_MONITOR_Y + DISPLAY_MONITOR_X;
    blitBufferToBuffer(gDisplayMonitorBackgroundFrmData,
        DISPLAY_MONITOR_WIDTH,
        DISPLAY_MONITOR_HEIGHT,
        DISPLAY_MONITOR_WIDTH,
        buf,
        _intface_full_width);

    int oldFont = fontGetCurrent();
    fontSetCurrent(DISPLAY_MONITOR_FONT);

    int lineHeight = fontGetLineHeight();
    int monitorWidth = DISPLAY_MONITOR_WIDTH;
    int monitorHeight = DISPLAY_MONITOR_HEIGHT;
    int line = gDisplayMonitorScrollPx / lineHeight;
    int pixel = gDisplayMonitorScrollPx % lineHeight;

    // Rows are drawn through a row buffer so a partially visible row (the
    // pixel offset) can be clipped to the monitor area. Row k of
    // 0.._max_disp shows the line `line + (_max_disp - k)` back in history;
    // its top is at pixel + (k - 1) * lineHeight.
    std::vector<unsigned char> row(static_cast<size_t>(monitorWidth) * lineHeight);
    for (int k = 0; k <= _max_disp; k++) {
        int back = line + (_max_disp - k);
        if (back >= gDisplayMonitorLinesCapacity) {
            continue;
        }
        int stringIndex = (_disp_start + gDisplayMonitorLinesCapacity - 1 - back) % gDisplayMonitorLinesCapacity;
        int y = pixel + (k - 1) * lineHeight;
        int visibleTop = std::max(y, 0);
        int visibleBottom = std::min(y + lineHeight, monitorHeight);
        if (visibleTop >= visibleBottom) {
            continue;
        }

        // Even though the display monitor is rectangular, its graphic is
        // not: earlier messages sit one pixel further right per row to give
        // a feel of depth (the original incremented the destination by 1).
        int depthX = std::max(k - 1, 0);

        // Background under this row (visible part), text drawn over it,
        // visible part copied back.
        for (int ry = visibleTop; ry < visibleBottom; ry++) {
            memcpy(&row[static_cast<size_t>(ry - y) * monitorWidth], buf + ry * _intface_full_width, monitorWidth);
        }
        fontDrawText(&row[static_cast<size_t>(0) + depthX], gDisplayMonitorLines[stringIndex], monitorWidth - depthX, monitorWidth, COLOR_GREEN);
        for (int ry = visibleTop; ry < visibleBottom; ry++) {
            memcpy(buf + ry * _intface_full_width, &row[static_cast<size_t>(ry - y) * monitorWidth], monitorWidth);
        }
    }

    windowRefreshRect(gInterfaceBarWindow, &gDisplayMonitorRect);
    fontSetCurrent(oldFont);
}

static int displayMonitorMaxScrollPixels()
{
    return std::max(0, (gDisplayMonitorLinesCapacity - _max_disp) * fontGetLineHeight());
}

static void displayMonitorSetScrollPixels(int scrollPx)
{
    scrollPx = std::clamp(scrollPx, 0, displayMonitorMaxScrollPixels());
    if (scrollPx == gDisplayMonitorScrollPx) {
        return;
    }
    gDisplayMonitorScrollPx = scrollPx;
    _disp_curr = (_disp_start + gDisplayMonitorLinesCapacity - scrollPx / fontGetLineHeight()) % gDisplayMonitorLinesCapacity;
    displayMonitorRefresh();
}

static void displayMonitorSnapToLine()
{
    int lineHeight = fontGetLineHeight();
    int rem = gDisplayMonitorScrollPx % lineHeight;
    if (rem != 0) {
        displayMonitorSetScrollPixels(rem < lineHeight / 2 ? gDisplayMonitorScrollPx - rem : gDisplayMonitorScrollPx + lineHeight - rem);
    }
}

bool displayMonitorTouchHitTest(int x, int y)
{
    // Deliberately not gated on gDisplayMonitorEnabled: reading back the
    // log while the enemy moves is harmless and wanted.
    if (!gDisplayMonitorInitialized || gInterfaceBarWindow == -1) {
        return false;
    }
    Window* window = windowGetWindow(gInterfaceBarWindow);
    if (window == nullptr || (window->flags & WINDOW_HIDDEN) != 0) {
        return false;
    }
    Rect r;
    if (windowGetRect(gInterfaceBarWindow, &r) != 0) {
        return false;
    }
    return x >= r.left + gDisplayMonitorRect.left && x <= r.left + gDisplayMonitorRect.right
        && y >= r.top + gDisplayMonitorRect.top && y <= r.top + gDisplayMonitorRect.bottom;
}

void displayMonitorTouchPan(int dyPixels)
{
    if (!gDisplayMonitorInitialized) {
        return;
    }
    gDisplayMonitorTouchDragging = true;
    gDisplayMonitorFlingVy = 0.0;
    // Content follows the finger: dragging down reveals earlier lines.
    displayMonitorSetScrollPixels(gDisplayMonitorScrollPx + dyPixels);
}

void displayMonitorTouchRelease(double fingerVelocityPxPerSec)
{
    if (!gDisplayMonitorInitialized) {
        return;
    }
    gDisplayMonitorTouchDragging = false;
    gDisplayMonitorFlingVy = fingerVelocityPxPerSec;
    gDisplayMonitorFlingCarry = 0.0;
    gDisplayMonitorFlingTicks = SDL_GetTicks();
    if (fabs(gDisplayMonitorFlingVy) < 60.0) {
        gDisplayMonitorFlingVy = 0.0;
        displayMonitorSnapToLine();
    }
}

static void displayMonitorTouchUpdate()
{
    if (gDisplayMonitorFlingVy == 0.0) {
        return;
    }
    unsigned int now = SDL_GetTicks();
    unsigned int dt = now - gDisplayMonitorFlingTicks;
    gDisplayMonitorFlingTicks = now;
    if (dt == 0) {
        return;
    }
    if (dt > 100) {
        dt = 100;
    }
    gDisplayMonitorFlingCarry += gDisplayMonitorFlingVy * dt / 1000.0;
    int step = static_cast<int>(gDisplayMonitorFlingCarry);
    gDisplayMonitorFlingCarry -= step;
    if (step != 0) {
        int before = gDisplayMonitorScrollPx;
        displayMonitorSetScrollPixels(before + step);
        if (gDisplayMonitorScrollPx != before + step) {
            // Hit the newest or the oldest line.
            gDisplayMonitorFlingVy = 0.0;
            displayMonitorSnapToLine();
            return;
        }
    }
    gDisplayMonitorFlingVy *= exp(-static_cast<double>(dt) / 220.0);
    if (fabs(gDisplayMonitorFlingVy) < 40.0) {
        gDisplayMonitorFlingVy = 0.0;
        displayMonitorSnapToLine();
        return;
    }
    sharedFpsLimiter.notifyActivity();
}

// 0x431B70 display_scroll_up
static void displayMonitorScrollUpOnMouseDown(int btn, int keyCode)
{
    gDisplayMonitorFlingVy = 0.0;
    displayMonitorSetScrollPixels(gDisplayMonitorScrollPx + fontGetLineHeight());
}

// 0x431B9C display_scroll_down
static void displayMonitorScrollDownOnMouseDown(int btn, int keyCode)
{
    gDisplayMonitorFlingVy = 0.0;
    displayMonitorSetScrollPixels(gDisplayMonitorScrollPx - fontGetLineHeight());
}

// 0x431BC8 display_arrow_up
static void displayMonitorScrollUpOnMouseEnter(int btn, int keyCode)
{
    gameMouseSetCursor(MOUSE_CURSOR_SMALL_ARROW_UP);
}

// 0x431BD4 display_arrow_down
static void displayMonitorScrollDownOnMouseEnter(int btn, int keyCode)
{
    gameMouseSetCursor(MOUSE_CURSOR_SMALL_ARROW_DOWN);
}

// 0x431BE0 display_arrow_restore
static void displayMonitorOnMouseExit(int btn, int keyCode)
{
    gameMouseSetCursor(MOUSE_CURSOR_ARROW);
}

// 0x431BEC display_disable
void displayMonitorDisable()
{
    if (gDisplayMonitorEnabled) {
#if !(__APPLE__ && TARGET_OS_IOS)
        // Touch build keeps the log scrollable during the enemy's turn.
        buttonDisable(gDisplayMonitorScrollDownButton);
        buttonDisable(gDisplayMonitorScrollUpButton);
#endif
        gDisplayMonitorEnabled = false;
    }
}

// 0x431C14 display_enable
void displayMonitorEnable()
{
    if (!gDisplayMonitorEnabled) {
#if !(__APPLE__ && TARGET_OS_IOS)
        buttonEnable(gDisplayMonitorScrollDownButton);
        buttonEnable(gDisplayMonitorScrollUpButton);
#endif
        gDisplayMonitorEnabled = true;
    }
}

static void consoleFileInit()
{
    const std::string& consolePath = settings.debug.console_output_path;
    if (!consolePath.empty()) {
        gConsoleFileStream.open(consolePath);
    }
}

static void consoleFileReset()
{
    if (gConsoleFileStream.is_open()) {
        gConsoleFilePrintCount = 0;
        gConsoleFileStream.flush();
    }
}

static void consoleFileExit()
{
    if (gConsoleFileStream.is_open()) {
        gConsoleFileStream.close();
    }
}

static void consoleFileAddMessage(const char* message)
{
    if (gConsoleFileStream.is_open()) {
        gConsoleFileStream << message << '\n';

        gConsoleFilePrintCount++;
        if (gConsoleFilePrintCount >= 20) {
            consoleFileFlush();
        }
    }
}

static void consoleFileFlush()
{
    if (gConsoleFileStream.is_open()) {
        gConsoleFilePrintCount = 0;
        gConsoleFileStream.flush();
    }
}

} // namespace fallout
