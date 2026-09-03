#include "mouse.h"

#include "delay.h"

#include "camera_follow.h"
#include "display_monitor.h"
#include "worldmap.h"

#include <math.h>

#if __APPLE__
#include <TargetConditionals.h>
#endif

#include "color.h"
#include "dinput.h"
#include "input.h"
#include "interface.h"
#include "kb.h"
#include "map.h"
#include "memory.h"
#include "platform/ios/quick_toolbar.h"
#include "svga.h"
#include "touch.h"
#include "touch_overlay.h"
#include "window_manager.h"

namespace fallout {

static void mousePrepareDefaultCursor();
static void _mouse_anim();
static void _mouse_clip();

// The default mouse cursor buffer.
//
// Initially it contains color codes, which will be replaced at startup
// according to loaded palette.
//
// Available color codes:
// - 0: transparent
// - 1: white
// - 15: black
//
// 0x51E250 or_mask
static unsigned char gMouseDefaultCursor[MOUSE_DEFAULT_CURSOR_SIZE] = {
    // clang-format off
    1,  1,  1,  1,  1,  1,  1, 0,
    1, 15, 15, 15, 15, 15,  1, 0,
    1, 15, 15, 15, 15,  1,  1, 0,
    1, 15, 15, 15, 15,  1,  1, 0,
    1, 15, 15, 15, 15, 15,  1, 1,
    1, 15,  1,  1, 15, 15, 15, 1,
    1,  1,  1,  1,  1, 15, 15, 1,
    0,  0,  0,  0,  1,  1,  1, 1,
    // clang-format on
};

// 0x51E290 mouse_idling
static int _mouse_idling = 0;

// 0x51E294 mouse_buf
static unsigned char* gMouseCursorData = nullptr;

// 0x51E298 mouse_shape
static unsigned char* _mouse_shape = nullptr;

// 0x51E29C mouse_fptr
static unsigned char* _mouse_fptr = nullptr;

// 0x51E2A0 mouse_sensitivity
static double gMouseSensitivity = 1.0;

// 0x51E2AC last_buttons
static int last_buttons = 0;

// 0x6AC790 mouse_is_hidden
static bool gCursorIsHidden;

// 0x6AC794 raw_x
static int _raw_x;

// 0x6AC798 mouse_length
static int gMouseCursorHeight;

// 0x6AC79C raw_y
static int _raw_y;

// 0x6AC7A0 raw_buttons
static int _raw_buttons;

// 0x6AC7A4 mouse_y
static int gMouseCursorY;

// 0x6AC7A8 mouse_x
static int gMouseCursorX;

// 0x6AC7AC mouse_disabled
static int _mouse_disabled;

// 0x6AC7B0 mouse_buttons
static int gMouseEvent;

// 0x6AC7B4 mouse_speed
static unsigned int _mouse_speed;

// 0x6AC7B8 mouse_curr_frame
static int _mouse_curr_frame;

// 0x6AC7BC have_mouse
static bool gMouseInitialized;

// 0x6AC7C0 mouse_pitch
static int gMouseCursorPitch;

// 0x6AC7C4 mouse_width
static int gMouseCursorWidth;

// 0x6AC7C8 mouse_num_frames
static int _mouse_num_frames;

// 0x6AC7CC mouse_hoty
static int _mouse_hoty;

// 0x6AC7D0 mouse_hotx
static int _mouse_hotx;

// 0x6AC7D4 mouse_idle_start_time
static unsigned int _mouse_idle_start_time;

// 0x6AC7D8 mouse_blit_trans
WindowDrawingProc2* _mouse_blit_trans;

// 0x6AC7DC mouse_blit
WINDOWDRAWINGPROC _mouse_blit;

// 0x6AC7E0 mouse_trans
static char _mouse_trans;

static int gMouseWheelX = 0;
static int gMouseWheelY = 0;

// 0x4C9F40
int mouseInit()
{
    gMouseInitialized = false;
    _mouse_disabled = 0;

    gCursorIsHidden = true;

    mousePrepareDefaultCursor();

    if (mouseSetFrame(nullptr, 0, 0, 0, 0, 0, 0) == -1) {
        return -1;
    }

    if (!mouseDeviceAcquire()) {
        return -1;
    }

    gMouseInitialized = true;
    gMouseCursorX = _scr_size.right / 2;
    gMouseCursorY = _scr_size.bottom / 2;
    _raw_x = _scr_size.right / 2;
    _raw_y = _scr_size.bottom / 2;
    _mouse_idle_start_time = getTicks();

    return 0;
}

// 0x4C9FD8
void mouseFree()
{
    mouseDeviceUnacquire();

    if (gMouseCursorData != nullptr) {
        internal_free(gMouseCursorData);
        gMouseCursorData = nullptr;
    }

    if (_mouse_fptr != nullptr) {
        tickersRemove(_mouse_anim);
        _mouse_fptr = nullptr;
    }
}

// 0x4CA01C
static void mousePrepareDefaultCursor()
{
    for (int index = 0; index < 64; index++) {
        switch (gMouseDefaultCursor[index]) {
        case 0:
            gMouseDefaultCursor[index] = COLOR_BLACK;
            break;
        case 1:
            gMouseDefaultCursor[index] = COLOR_DARK_GREY;
            break;
        case 15:
            gMouseDefaultCursor[index] = COLOR_WHITE;
            break;
        }
    }
}

// 0x4CA0AC
int mouseSetFrame(unsigned char* frame, int width, int height, int pitch, int hotX, int hotY, char transparentColor)
{
    Rect rect;
    unsigned char* cursorFrame;
    int hotXDelta;
    int hotYDelta;

    cursorFrame = frame;

    if (frame == nullptr) {
        // NOTE: Original code looks tail recursion optimization.
        return mouseSetFrame(gMouseDefaultCursor, MOUSE_DEFAULT_CURSOR_WIDTH, MOUSE_DEFAULT_CURSOR_HEIGHT, MOUSE_DEFAULT_CURSOR_WIDTH, 1, 1, COLOR_BLACK);
    }

    bool cursorWasHidden = gCursorIsHidden;
    if (!gCursorIsHidden && gMouseInitialized) {
        gCursorIsHidden = true;
        mouseGetRect(&rect);
        windowRefreshAll(&rect);
    }

    if (width != gMouseCursorWidth || height != gMouseCursorHeight) {
        unsigned char* buf = (unsigned char*)internal_malloc(width * height);
        if (buf == nullptr) {
            if (!cursorWasHidden) {
                mouseShowCursor();
            }
            return -1;
        }

        if (gMouseCursorData != nullptr) {
            internal_free(gMouseCursorData);
        }

        gMouseCursorData = buf;
    }

    gMouseCursorWidth = width;
    gMouseCursorHeight = height;
    gMouseCursorPitch = pitch;
    _mouse_shape = cursorFrame;
    _mouse_trans = transparentColor;

    if (_mouse_fptr) {
        tickersRemove(_mouse_anim);
        _mouse_fptr = nullptr;
    }

    hotXDelta = _mouse_hotx - hotX;
    _mouse_hotx = hotX;

    gMouseCursorX += hotXDelta;

    hotYDelta = _mouse_hoty - hotY;
    _mouse_hoty = hotY;

    gMouseCursorY += hotYDelta;

    _mouse_clip();

    if (!cursorWasHidden) {
        mouseShowCursor();
    }

    _raw_x = gMouseCursorX;
    _raw_y = gMouseCursorY;

    return 0;
}

// NOTE: Looks like this code is not reachable.
//
// 0x4CA2D0
static void _mouse_anim()
{
    // 0x51E2A8
    static unsigned int ticker = 0;

    if (getTicksSince(ticker) >= _mouse_speed) {
        ticker = getTicks();

        if (++_mouse_curr_frame == _mouse_num_frames) {
            _mouse_curr_frame = 0;
        }

        _mouse_shape = gMouseCursorWidth * _mouse_curr_frame * gMouseCursorHeight + _mouse_fptr;

        if (!gCursorIsHidden) {
            mouseShowCursor();
        }
    }
}

// 0x4CA34C
void mouseShowCursor()
{
    unsigned char* cursorData;
    int clipX;
    int clipWidth;
    int clipY;
    int clipHeight;
    int cursorDataIndex;

    cursorData = gMouseCursorData;
    if (gMouseInitialized) {
        if (!_mouse_blit_trans || !gCursorIsHidden) {
            _win_get_mouse_buf(gMouseCursorData);
            cursorData = gMouseCursorData;
            cursorDataIndex = 0;

            for (int y = 0; y < gMouseCursorHeight; y++) {
                for (int x = 0; x < gMouseCursorWidth; x++) {
                    unsigned char pixel = _mouse_shape[y * gMouseCursorPitch + x];
                    if (pixel != _mouse_trans) {
                        cursorData[cursorDataIndex] = pixel;
                    }
                    cursorDataIndex++;
                }
            }
        }

        if (gMouseCursorX >= _scr_size.left) {
            if (gMouseCursorWidth + gMouseCursorX - 1 <= _scr_size.right) {
                clipWidth = gMouseCursorWidth;
                clipX = 0;
            } else {
                clipX = 0;
                clipWidth = _scr_size.right - gMouseCursorX + 1;
            }
        } else {
            clipX = _scr_size.left - gMouseCursorX;
            clipWidth = gMouseCursorWidth - (_scr_size.left - gMouseCursorX);
        }

        if (gMouseCursorY >= _scr_size.top) {
            if (gMouseCursorHeight + gMouseCursorY - 1 <= _scr_size.bottom) {
                clipY = 0;
                clipHeight = gMouseCursorHeight;
            } else {
                clipY = 0;
                clipHeight = _scr_size.bottom - gMouseCursorY + 1;
            }
        } else {
            clipY = _scr_size.top - gMouseCursorY;
            clipHeight = gMouseCursorHeight - (_scr_size.top - gMouseCursorY);
        }

        gMouseCursorData = cursorData;
        if (_mouse_blit_trans && gCursorIsHidden) {
            _mouse_blit_trans(_mouse_shape, gMouseCursorPitch, gMouseCursorHeight, clipX, clipY, clipWidth, clipHeight, clipX + gMouseCursorX, clipY + gMouseCursorY, _mouse_trans);
        } else {
            _mouse_blit(gMouseCursorData, gMouseCursorWidth, gMouseCursorHeight, clipX, clipY, clipWidth, clipHeight, clipX + gMouseCursorX, clipY + gMouseCursorY);
        }

        cursorData = gMouseCursorData;
        gCursorIsHidden = false;
    }
    gMouseCursorData = cursorData;
}

// 0x4CA534
void mouseHideCursor()
{
    Rect rect;

    if (gMouseInitialized) {
        if (!gCursorIsHidden) {
            rect.left = gMouseCursorX;
            rect.top = gMouseCursorY;
            rect.right = gMouseCursorX + gMouseCursorWidth - 1;
            rect.bottom = gMouseCursorY + gMouseCursorHeight - 1;

            gCursorIsHidden = true;
            windowRefreshAll(&rect);
        }
    }
}

// Original buttons visibly depress when clicked; a synthetic tap is
// instantaneous, so its pressed art would flash for a single frame. Emulate
// a short hold instead: the pressed state lasts kSyntheticPressMs before
// the release (and the button's action) goes through.
constexpr unsigned int kSyntheticPressMs = 90;

// Pipeline taps: keep the simulated mouse button held for a few frames.
static int gSyntheticHoldButtons = 0;
static unsigned int gSyntheticHoldUntil = 0;

// HUD tap-through: the pressed button waiting for its visual release.
static int gHudTapPressedBtn = -1;
static int gHudTapPressedKeyCode = -1;
static unsigned int gHudTapReleaseTicks = 0;

#if __APPLE__ && TARGET_OS_IOS
// Checks whether a tap lands on an interface-bar button and, if so,
// injects the corresponding keyCode so the cursor never moves.
// Returns true if the tap was consumed (button injected or bare chrome hit).
static bool handleHudTapThrough(const Gesture& gesture)
{
    if (!mouseDeviceUsesRelativeMode() || touch_get_touchscreen_mode() || gInterfaceBarWindow == -1) {
        return false;
    }

    Window* hudWindow = windowGetWindow(gInterfaceBarWindow);
    if (hudWindow == nullptr || (hudWindow->flags & WINDOW_HIDDEN) != 0) {
        return false;
    }

    Rect hudRect;
    if (windowGetRect(gInterfaceBarWindow, &hudRect) != 0
        || gesture.x < hudRect.left || gesture.x > hudRect.right
        || gesture.y < hudRect.top || gesture.y > hudRect.bottom) {
        return false;
    }

    if (gesture.numberOfTouches == 1 || gesture.numberOfTouches == 2) {
        for (Button* button = hudWindow->buttonListHead; button != nullptr; button = button->next) {
            if ((button->flags & BUTTON_FLAG_DISABLED) != 0) {
                continue;
            }
            int left = hudWindow->rect.left + button->rect.left;
            int top = hudWindow->rect.top + button->rect.top;
            int right = hudWindow->rect.left + button->rect.right;
            int bottom = hudWindow->rect.top + button->rect.bottom;
            if (gesture.x < left || gesture.x > right || gesture.y < top || gesture.y > bottom) {
                continue;
            }
            int keyCode = gesture.numberOfTouches == 1
                ? button->leftMouseUpEventCode
                : button->rightMouseUpEventCode;
            if (keyCode == -1) {
                break;
            }
            // A quick second tap flushes the previous press immediately.
            if (gHudTapPressedBtn != -1) {
                _win_button_set_visual_pressed(gHudTapPressedBtn, false);
                if (gHudTapPressedKeyCode != -1) {
                    enqueueInputEvent(gHudTapPressedKeyCode);
                }
            }
            _win_button_set_visual_pressed(button->id, true);
            gHudTapPressedBtn = button->id;
            gHudTapPressedKeyCode = keyCode;
            gHudTapReleaseTicks = SDL_GetTicks() + kSyntheticPressMs;
            return true;
        }
    }

    // Tap landed on belt chrome (no button under it). Consume silently
    // rather than teleporting the cursor to an inert region.
    return true;
}
#endif

// Two-finger pan accumulators and fling (inertia) state. The accumulators
// convert finger distance into 32x24px map steps; a released pan above the
// speed threshold keeps scrolling with exponential decay until it slows
// down or any new touch cancels it.
// True previous gesture position for delta-based pan/wheel handling. The
// prevx/prevy statics inside _mouse_info are zeroed in absolute mode to feed
// absolute coordinates to _mouse_simulate_input, which makes them unusable
// as a delta base there.
static int gGesturePrevX = 0;
static int gGesturePrevY = 0;

// Pans captured by a scrollable list (see the kPan handler).
enum TouchScrollTarget {
    kTouchScrollNone,
    kTouchScrollTabs,
    kTouchScrollMonitor,
};
static int gTouchScrollTarget = kTouchScrollNone;
static double gTouchScrollVy = 0.0;
static unsigned int gTouchScrollLastTicks = 0;

// A tap in touchscreen mode whose finger-down and -up arrived in the same
// event pump never got its cursor warp: the press is placed and deferred by
// one frame so the window manager hovers the button before the click
// (it ignores a button-down on a button it is not already hovering).
static int gDeferredTapButtons = 0;

// Set when a touch long press starts delivering its held button: the
// engine's hold-to-open menus (inventory item menu, world action menu) wait
// another BUTTON_REPEAT_TIME for the first repeat on top of the 300ms the
// recognizer already took. Backdating the press timestamp makes that repeat
// fire on the next frame - the long press itself is the intent signal.
static bool gBackdateLeftPress = false;

bool mouseTouchTapHoldActive()
{
    return gSyntheticHoldButtons != 0 && SDL_GetTicks() < gSyntheticHoldUntil;
}

// Ends a pan that was routed to a scrollable list without waiting for its
// kEnded (the gesture backlog was discarded).
// Wait for the mouse event state to settle before a modal transition -
// bounded (a latched flag from a held finger or a focus race must not hang
// the game), and whatever cannot settle is cleared: a leaked button event
// reads as input and would instantly skip the movie or slide that follows.
void mouseSettleEvents(unsigned int maxMs)
{
    unsigned int start = SDL_GetTicks();
    while (mouseGetEvent() != 0 && SDL_GetTicks() - start < maxMs) {
        _mouse_info();
        delay_ms(1);
    }
    gMouseEvent = 0;
    _raw_buttons = 0;
}

void mouseTouchScrollCancel()
{
    if (gTouchScrollTarget == kTouchScrollTabs) {
        wmTouchTabsRelease(0.0);
    } else if (gTouchScrollTarget == kTouchScrollMonitor) {
        displayMonitorTouchRelease(0.0);
    }
    gTouchScrollTarget = kTouchScrollNone;
}

// Finger travel per one list-scroll wheel tick in touchscreen contexts.
constexpr int kWheelStepPx = 32;
static int gWheelAccumX = 0;
static int gWheelAccumY = 0;

// Posted for taps that arrive while the cursor is hidden (movies, loads):
// "press any key" consumers see it, everything else ignores the code.
constexpr int kHiddenCursorTapKeyCode = 2003;

// When the cursor was last deliberately moved. Edge scrolling on touch only
// engages near this moment: a parked trackpad-style cursor at a screen edge
// must not drag the camera forever.
static unsigned int gLastCursorMotionTicks = 0;

bool mouseCursorMovedRecently(unsigned int windowMs)
{
    return SDL_GetTicks() - gLastCursorMotionTicks <= windowMs;
}

static double gFlingCarryX = 0.0;
static double gFlingCarryY = 0.0;
static double gFlingVX = 0.0;
static double gFlingVY = 0.0;
static bool gFlingActive = false;
static unsigned int gFlingLastTicks = 0;
static unsigned int gPanLastTicks = 0;

// 0x4CA59C
void _mouse_info()
{
    if (!gMouseInitialized) {
        return;
    }

    // Pending synthetic HUD button release (see handleHudTapThrough).
    if (gHudTapPressedBtn != -1 && SDL_GetTicks() >= gHudTapReleaseTicks) {
        _win_button_set_visual_pressed(gHudTapPressedBtn, false);
        if (gHudTapPressedKeyCode != -1) {
            enqueueInputEvent(gHudTapPressedKeyCode);
        }
        gHudTapPressedBtn = -1;
        gHudTapPressedKeyCode = -1;
    }

    if (gCursorIsHidden || _mouse_disabled) {
        // The gesture queue keeps filling from the event pump while the
        // cursor is hidden (movies, map loads, transitions). Discard the
        // backlog instead of replaying it as phantom clicks and pans once
        // the cursor comes back. A completed tap still pings the input
        // queue so cutscenes can be skipped by tapping, and the pan
        // trackers stay anchored so a pan spanning the gap does not jump.
        Gesture pending;
        while (touch_get_gesture(&pending)) {
            if (pending.type == kTap && pending.state == kEnded) {
                enqueueInputEvent(kHiddenCursorTapKeyCode);
            }
            gGesturePrevX = pending.x;
            gGesturePrevY = pending.y;
        }

        // Inertia must not carry over across a load or cutscene either.
        gFlingActive = false;
        // Nor a list scroll whose finger-up was in the discarded backlog:
        // the log would think it is being dragged forever.
        mouseTouchScrollCancel();
        gDeferredTapButtons = 0;

        return;
    }

    if (gDeferredTapButtons != 0) {
        int buttons = gDeferredTapButtons;
        gDeferredTapButtons = 0;
        _mouse_simulate_input(0, 0, buttons);
        gSyntheticHoldButtons = buttons;
        gSyntheticHoldUntil = SDL_GetTicks() + kSyntheticPressMs;
    }

    Gesture gesture;
    if (touch_get_gesture(&gesture)) {
        // Any new touch cancels scroll inertia.
        if (gesture.state == kBegan) {
            gFlingActive = false;
        }
        static int prevx;
        static int prevy;

        // Multi-finger gestures for keyboard-less touch play:
        //   3-finger swipe down → ESC (options menu)
        //   3-finger long press → hold Left Shift (highlights interactables)
        //   4-finger long press → F6  (quicksave)
        if (gesture.type == kPan && gesture.numberOfTouches == 3) {
            static int swipeStartY;
            if (gesture.state == kBegan) {
                swipeStartY = gesture.y;
            } else if (gesture.state == kEnded) {
                int dy = gesture.y - swipeStartY;
                if (dy > screenGetHeight() / 4) {
                    enqueueInputEvent(KEY_ESCAPE);
                }
            }
            return;
        }

        // Four-finger long press → F6 (quicksave). Long-press is more
        // reliable than a tap since all 4 fingers rarely land and lift
        // within the 75ms tap window; and more reliable than a swipe
        // since iPadOS intercepts multi-finger vertical swipes.
        if (gesture.type == kLongPress && gesture.numberOfTouches == 4) {
            if (gesture.state == kBegan) {
                enqueueInputEvent(KEY_F6);
            }
            return;
        }

        // Three-finger long press toggles the native item highlight on and
        // off, switch-style. It stays on while playing: the outlines do not
        // touch movement (no simulated Shift is held).
        if (gesture.type == kLongPress && gesture.numberOfTouches == 3) {
            if (gesture.state == kBegan) {
                touchOverlayToggleHighlight();
            }
            return;
        }

        switch (gesture.type) {
        case kTap: {
            // Toolbar taps bypass the mouse pipeline entirely: the handler
            // invokes the action in place, so the cursor never moves.
            // Skip when touchscreen mode is active (dialog, inventory, etc.)
            // so toolbar doesn't intercept taps meant for overlapping UI.
            if (!touch_get_touchscreen_mode()
                && gesture.numberOfTouches == 1
                && quickToolbarContainsPoint(gesture.x, gesture.y)) {
                if (quickToolbarHandleTap(gesture.x, gesture.y)) {
                    break;
                }
            }

            if (!touch_get_touchscreen_mode()
                && gesture.numberOfTouches == 1
                && touchOverlayContainsPoint(gesture.x, gesture.y)) {
                if (touchOverlayHandleTap(gesture.x, gesture.y)) {
                    break;
                }
            }


#if __APPLE__ && TARGET_OS_IOS
            if (handleHudTapThrough(gesture)) {
                goto tap_done;
            }
#endif

            if (mouseDeviceUsesRelativeMode()) {
                if (touch_get_touchscreen_mode() && (gMouseCursorX != gesture.x || gMouseCursorY != gesture.y)) {
                    // Finger-down and -up landed in one pump, so the
                    // touchscreen warp never happened: place the cursor
                    // now and press next frame (see gDeferredTapButtons).
                    _mouse_set_position(gesture.x, gesture.y);
                    if (gesture.numberOfTouches == 1) {
                        gDeferredTapButtons = MOUSE_STATE_LEFT_BUTTON_DOWN;
                    } else if (gesture.numberOfTouches == 2) {
                        gDeferredTapButtons = MOUSE_STATE_RIGHT_BUTTON_DOWN;
                    }
                    goto tap_done;
                }
                if (gesture.numberOfTouches == 1) {
                    _mouse_simulate_input(0, 0, MOUSE_STATE_LEFT_BUTTON_DOWN);
                } else if (gesture.numberOfTouches == 2) {
                    _mouse_simulate_input(0, 0, MOUSE_STATE_RIGHT_BUTTON_DOWN);
                }
            } else {
                _mouse_set_position(gesture.x, gesture.y);
                if (gesture.numberOfTouches == 1) {
                    _mouse_simulate_input(gesture.x, gesture.y, MOUSE_STATE_LEFT_BUTTON_DOWN);
                } else if (gesture.numberOfTouches == 2) {
                    _mouse_simulate_input(gesture.x, gesture.y, MOUSE_STATE_RIGHT_BUTTON_DOWN);
                }
            }

            // Stretch the tap into a short hold so engine buttons show
            // their pressed art like they do for a real quick click.
            if (gesture.numberOfTouches == 1) {
                gSyntheticHoldButtons = MOUSE_STATE_LEFT_BUTTON_DOWN;
                gSyntheticHoldUntil = SDL_GetTicks() + kSyntheticPressMs;
            } else if (gesture.numberOfTouches == 2) {
                gSyntheticHoldButtons = MOUSE_STATE_RIGHT_BUTTON_DOWN;
                gSyntheticHoldUntil = SDL_GetTicks() + kSyntheticPressMs;
            }
        tap_done:
            break;
        }
        case kLongPress:
        case kPan:
            if (gesture.state == kBegan) {
                prevx = gesture.x;
                prevy = gesture.y;
                gGesturePrevX = gesture.x;
                gGesturePrevY = gesture.y;
            }

            // Pans that start over a scrollable list (world map town list,
            // the message log) scroll that list pixel by pixel with inertia
            // instead of moving the cursor or the map.
            if (gesture.type == kPan) {
                if (gesture.state == kBegan) {
                    gTouchScrollTarget = kTouchScrollNone;
                    if (gesture.numberOfTouches != 1) {
                        // Two fingers stay the camera / map wheel.
                    } else if (wmTouchTabsHitTest(gesture.x, gesture.y)) {
                        gTouchScrollTarget = kTouchScrollTabs;
                    } else if (displayMonitorTouchHitTest(gesture.x, gesture.y)) {
                        gTouchScrollTarget = kTouchScrollMonitor;
                    }
                    gTouchScrollVy = 0.0;
                    gTouchScrollLastTicks = SDL_GetTicks();
                }
                if (gTouchScrollTarget != kTouchScrollNone) {
                    unsigned int nowTicks = SDL_GetTicks();
                    int dy = gesture.y - gGesturePrevY;
                    unsigned int dt = nowTicks - gTouchScrollLastTicks;
                    if (dt > 0) {
                        gTouchScrollVy = 0.7 * gTouchScrollVy + 0.3 * (1000.0 * dy / dt);
                        gTouchScrollLastTicks = nowTicks;
                    }
                    if (gTouchScrollTarget == kTouchScrollTabs) {
                        wmTouchTabsPan(dy);
                    } else {
                        displayMonitorTouchPan(dy);
                    }
                    gGesturePrevX = gesture.x;
                    gGesturePrevY = gesture.y;
                    prevx = gesture.x;
                    prevy = gesture.y;
                    if (gesture.state == kEnded) {
                        if (gTouchScrollTarget == kTouchScrollTabs) {
                            wmTouchTabsRelease(gTouchScrollVy);
                        } else {
                            displayMonitorTouchRelease(gTouchScrollVy);
                        }
                        gTouchScrollTarget = kTouchScrollNone;
                    }
                    break;
                }
            }
            if (!mouseDeviceUsesRelativeMode()) {
                prevx = 0;
                prevy = 0;
            }

            if (gesture.type == kLongPress) {
                if (gesture.numberOfTouches == 1) {
                    if (gesture.state == kBegan) {
                        gBackdateLeftPress = true;
                    }
                    _mouse_simulate_input(gesture.x - prevx, gesture.y - prevy, MOUSE_STATE_LEFT_BUTTON_DOWN);
                } else if (gesture.numberOfTouches == 2) {
                    _mouse_simulate_input(gesture.x - prevx, gesture.y - prevy, MOUSE_STATE_RIGHT_BUTTON_DOWN);
                }
            } else if (gesture.type == kPan) {
                if (!touch_get_pan_mode() && gesture.numberOfTouches == 1) {
                    // Zoomed out the screen still maps 1:1 to the cursor -
                    // the cursor is a screen-space entity and every screen
                    // point lies inside the zoom crop.
                    _mouse_simulate_input(gesture.x - prevx, gesture.y - prevy, 0);
                } else if (touch_get_pan_mode() || gesture.numberOfTouches == 2) {
                    // Windowed screens (inventory, dialogs) interpret the
                    // wheel as list scrolling - keep that behavior there.
                    // Screens that replace the iso view entirely (the world
                    // map) also consume the wheel: the world map scrolls by
                    // it, and the pixel pan below would only move the hidden
                    // iso map underneath.
                    if (touch_get_touchscreen_mode() || isoIsDisabled()) {
                        // List consumers only look at the wheel's sign, one
                        // row per event - so emitting a wheel tick on every
                        // motion event scrolled by elapsed time, not finger
                        // travel (a slow one-second drag flew through ~60
                        // rows). Accumulate travel and tick once per
                        // kWheelStepPx instead.
                        if (gesture.state == kBegan) {
                            gWheelAccumX = 0;
                            gWheelAccumY = 0;
                        }
                        gWheelAccumX += gGesturePrevX - gesture.x;
                        gWheelAccumY += gesture.y - gGesturePrevY;
                        gGesturePrevX = gesture.x;
                        gGesturePrevY = gesture.y;

                        int ticksX = gWheelAccumX / kWheelStepPx;
                        int ticksY = gWheelAccumY / kWheelStepPx;
                        gWheelAccumX -= ticksX * kWheelStepPx;
                        gWheelAccumY -= ticksY * kWheelStepPx;
                        gMouseWheelX = ticksX;
                        gMouseWheelY = ticksY;
                        if (gMouseWheelX != 0 || gMouseWheelY != 0) {
                            gMouseEvent |= MOUSE_EVENT_WHEEL;
                            _raw_buttons |= MOUSE_EVENT_WHEEL;
                        }
                        break;
                    }

                    // Pixel-granular pan: the map follows the fingers 1:1 and
                    // smoothly (sub-tile viewport bias), tracking velocity
                    // for the release fling.
                    unsigned int nowTicks = SDL_GetTicks();

                    if (gesture.state == kBegan) {
                        gFlingVX = 0.0;
                        gFlingVY = 0.0;
                        gPanLastTicks = nowTicks;
                        // The player took the camera - stop following.
                        cameraFollowCancel();
                    }

                    int dxPix = gGesturePrevX - gesture.x;
                    int dyPix = gGesturePrevY - gesture.y;

                    unsigned int panDt = nowTicks - gPanLastTicks;
                    if (panDt > 0) {
                        gFlingVX = 0.7 * gFlingVX + 0.3 * (1000.0 * dxPix / panDt);
                        gFlingVY = 0.7 * gFlingVY + 0.3 * (1000.0 * dyPix / panDt);
                        gPanLastTicks = nowTicks;
                    }

                    static int sPinchPrevSpread = 0;
                    static double sPanCarryX = 0.0;
                    static double sPanCarryY = 0.0;
                    static double sZoomAnchorCarryX = 0.0;
                    static double sZoomAnchorCarryY = 0.0;
                    if (gesture.state == kBegan) {
                        sPinchPrevSpread = touch_active_finger_spread();
                        sPanCarryX = 0.0;
                        sPanCarryY = 0.0;
                        sZoomAnchorCarryX = 0.0;
                        sZoomAnchorCarryY = 0.0;
                    }

                    // The standard two-finger gesture: pan and zoom run
                    // SIMULTANEOUSLY, no modes and no thresholds (the way
                    // UIPinchGestureRecognizer / map apps do it). Spread
                    // jitter integrates to zero, and the zoom is anchored
                    // at the finger centroid, so unintended scale noise is
                    // imperceptible while a deliberate pinch responds
                    // instantly. Fingers together = zoom out.
                    if (gesture.numberOfTouches == 2 && gesture.state != kEnded) {
                        int spread = touch_active_finger_spread();
                        if (spread > 0 && sPinchPrevSpread > 0 && spread != sPinchPrevSpread) {
                            double oldZoom = renderIsoGetZoom();
                            renderIsoSetZoom(oldZoom * sPinchPrevSpread / spread);
                            double newZoom = renderIsoGetZoom();
                            if (newZoom != oldZoom) {
                                // Keep the world point under the pinch
                                // centroid stationary: world(S) = const
                                // requires a camera shift of
                                // (oldZoom - newZoom) * (S - center).
                                sZoomAnchorCarryX += (oldZoom - newZoom) * (gesture.x - screenGetWidth() / 2.0);
                                sZoomAnchorCarryY += (oldZoom - newZoom) * (gesture.y - screenGetVisibleHeight() / 2.0);
                                int anchorX = static_cast<int>(sZoomAnchorCarryX);
                                int anchorY = static_cast<int>(sZoomAnchorCarryY);
                                sZoomAnchorCarryX -= anchorX;
                                sZoomAnchorCarryY -= anchorY;
                                if (anchorX != 0 || anchorY != 0) {
                                    mapScrollPixels(anchorX, anchorY);
                                }
                            }
                        }
                        if (spread > 0) {
                            sPinchPrevSpread = spread;
                        }
                    }

                    // Release settles the zoom to the nearest end when it
                    // is already close - one gentle adjustment instead of
                    // mid-gesture snapping.
                    if (gesture.state == kEnded) {
                        double endZoom = renderIsoGetZoom();
                        if (endZoom < 1.06) {
                            renderIsoSetZoom(1.0);
                        } else if (endZoom > 1.44) {
                            renderIsoSetZoom(1.5);
                        }
                    }

                    if (dxPix != 0 || dyPix != 0) {
                        // Finger travel is screen-space; zoomed out, one
                        // screen pixel spans zoom world pixels - scale so
                        // the map keeps tracking the fingers 1:1 on screen.
                        double panZoom = renderIsoGetZoom();
                        sPanCarryX += dxPix * panZoom;
                        sPanCarryY += dyPix * panZoom;
                        int panX = static_cast<int>(sPanCarryX);
                        int panY = static_cast<int>(sPanCarryY);
                        sPanCarryX -= panX;
                        sPanCarryY -= panY;
                        if (panX != 0 || panY != 0) {
                            mapScrollPixels(panX, panY);
                        }
                    }

                    if (gesture.state == kEnded
                        && gFlingVX * gFlingVX + gFlingVY * gFlingVY > 200.0 * 200.0) {
                        // Cap the launch speed - flick spikes overshoot.
                        double speed = sqrt(gFlingVX * gFlingVX + gFlingVY * gFlingVY);
                        if (speed > 1400.0) {
                            gFlingVX *= 1400.0 / speed;
                            gFlingVY *= 1400.0 / speed;
                        }
                        gFlingActive = true;
                        gFlingLastTicks = nowTicks;
                    }
                }
            }

            prevx = gesture.x;
            prevy = gesture.y;
            gGesturePrevX = gesture.x;
            gGesturePrevY = gesture.y;
            break;
        case kUnrecognized:
            break;
        }

        return;
    }

    // A modal touchscreen context (dialog, inventory) can open without a
    // touch, e.g. an NPC starting a conversation - inertia must not keep
    // dragging the world behind it.
    if (gFlingActive && touch_get_touchscreen_mode()) {
        gFlingActive = false;
    }

    // Scroll inertia: keep panning after the fingers lift, decaying
    // exponentially, until it slows down, hits a boundary or a new touch
    // cancels it.
    if (gFlingActive) {
        unsigned int nowTicks = SDL_GetTicks();
        unsigned int flingDt = nowTicks - gFlingLastTicks;
        if (flingDt > 100) {
            // A suspension mid-coast must not become one giant step.
            flingDt = 100;
        }
        if (flingDt > 0) {
            gFlingLastTicks = nowTicks;

            double flingZoom = renderIsoGetZoom();
            gFlingCarryX += gFlingVX * flingDt / 1000.0 * flingZoom;
            gFlingCarryY += gFlingVY * flingDt / 1000.0 * flingZoom;
            int px = static_cast<int>(gFlingCarryX);
            int py = static_cast<int>(gFlingCarryY);
            gFlingCarryX -= px;
            gFlingCarryY -= py;

            if ((px != 0 || py != 0) && mapScrollPixels(px, py) == -1) {
                gFlingActive = false;
            }

            double decay = exp(-static_cast<double>(flingDt) / 220.0);
            gFlingVX *= decay;
            gFlingVY *= decay;
            if (gFlingVX * gFlingVX + gFlingVY * gFlingVY < 60.0 * 60.0) {
                gFlingActive = false;
            }

            // The fling is user-initiated motion - keep the loop at full
            // rate so it stays smooth.
            sharedFpsLimiter.notifyActivity();
            return;
        }
    }

    int x;
    int y;
    int buttons = 0;

    MouseData mouseData;
    if (mouseDeviceGetData(&mouseData)) {
        x = mouseData.x;
        y = mouseData.y;

        if (mouseData.buttons[0] == 1) {
            buttons |= MOUSE_STATE_LEFT_BUTTON_DOWN;
        }

        if (mouseData.buttons[1] == 1) {
            buttons |= MOUSE_STATE_RIGHT_BUTTON_DOWN;
        }
    } else {
        x = 0;
        y = 0;
    }

    // Synthetic tap hold: keep reporting the button as pressed until the
    // deadline, then let the natural release (button-up) go through.
    if (gSyntheticHoldButtons != 0) {
        if (SDL_GetTicks() < gSyntheticHoldUntil) {
            buttons |= gSyntheticHoldButtons;
        } else {
            gSyntheticHoldButtons = 0;
        }
    }

    // Mouse sensitivity only applies to relative movement. In windowed mode
    // SDL provides absolute coordinates that should not be scaled.
    if (mouseDeviceUsesRelativeMode()) {
        x = (int)(x * gMouseSensitivity);
        y = (int)(y * gMouseSensitivity);
    }

    _mouse_simulate_input(x, y, buttons);

    // TODO: Move to `_mouse_simulate_input`.
    gMouseWheelX = mouseData.wheelX;
    gMouseWheelY = mouseData.wheelY;

    if (gMouseWheelX != 0 || gMouseWheelY != 0) {
        gMouseEvent |= MOUSE_EVENT_WHEEL;
        _raw_buttons |= MOUSE_EVENT_WHEEL;
    }
}

// 0x4CA698
void _mouse_simulate_input(int delta_x, int delta_y, int buttons)
{
    // 0x6AC7E4
    static unsigned int previousRightButtonTimestamp;

    // 0x6AC7E8
    static unsigned int previousLeftButtonTimestamp;

    // 0x6AC7EC
    static int previousEvent;

    if (!gMouseInitialized || gCursorIsHidden) {
        return;
    }

    if (delta_x == 0 && delta_y == 0 && buttons == last_buttons) {
        if (last_buttons == 0) {
            if (!_mouse_idling) {
                _mouse_idle_start_time = getTicks();
                _mouse_idling = 1;
            }

            last_buttons = 0;
            _raw_buttons = 0;
            gMouseEvent = 0;

            return;
        }
    }

    _mouse_idling = 0;
    last_buttons = buttons;
    previousEvent = gMouseEvent;
    gMouseEvent = 0;

    if ((previousEvent & MOUSE_EVENT_LEFT_BUTTON_DOWN_REPEAT) != 0) {
        if ((buttons & 0x01) != 0) {
            gMouseEvent |= MOUSE_EVENT_LEFT_BUTTON_REPEAT;

            if (getTicksSince(previousLeftButtonTimestamp) > BUTTON_REPEAT_TIME) {
                gMouseEvent |= MOUSE_EVENT_LEFT_BUTTON_DOWN;
                previousLeftButtonTimestamp = getTicks();
            }
        } else {
            gMouseEvent |= MOUSE_EVENT_LEFT_BUTTON_UP;
        }
    } else {
        if ((buttons & 0x01) != 0) {
            gMouseEvent |= MOUSE_EVENT_LEFT_BUTTON_DOWN;
            previousLeftButtonTimestamp = getTicks();
            if (gBackdateLeftPress) {
                // Touch long press: the hold has already lasted long enough.
                previousLeftButtonTimestamp -= BUTTON_REPEAT_TIME + 1;
            }
        }
        gBackdateLeftPress = false;
    }

    if ((previousEvent & MOUSE_EVENT_RIGHT_BUTTON_DOWN_REPEAT) != 0) {
        if ((buttons & 0x02) != 0) {
            gMouseEvent |= MOUSE_EVENT_RIGHT_BUTTON_REPEAT;
            if (getTicksSince(previousRightButtonTimestamp) > BUTTON_REPEAT_TIME) {
                gMouseEvent |= MOUSE_EVENT_RIGHT_BUTTON_DOWN;
                previousRightButtonTimestamp = getTicks();
            }
        } else {
            gMouseEvent |= MOUSE_EVENT_RIGHT_BUTTON_UP;
        }
    } else {
        if (buttons & 0x02) {
            gMouseEvent |= MOUSE_EVENT_RIGHT_BUTTON_DOWN;
            previousRightButtonTimestamp = getTicks();
        }
    }

    _raw_buttons = gMouseEvent;

    if (delta_x != 0 || delta_y != 0) {
        gLastCursorMotionTicks = SDL_GetTicks();

        Rect mouseRect;
        mouseRect.left = gMouseCursorX;
        mouseRect.top = gMouseCursorY;
        mouseRect.right = gMouseCursorWidth + gMouseCursorX - 1;
        mouseRect.bottom = gMouseCursorHeight + gMouseCursorY - 1;
        if (mouseDeviceUsesRelativeMode()) {
            gMouseCursorX += delta_x;
            gMouseCursorY += delta_y;
        } else {
            _mouse_set_position(delta_x, delta_y);
        }
        _mouse_clip();

        windowRefreshAll(&mouseRect);

        mouseShowCursor();

        if (mouseDeviceUsesRelativeMode()) {
            _raw_x = gMouseCursorX;
            _raw_y = gMouseCursorY;
        } else {
            _raw_x = delta_x;
            _raw_y = delta_y;
        }
    }
}

// 0x4CA8C8
bool _mouse_in(int left, int top, int right, int bottom)
{
    if (!gMouseInitialized) {
        return false;
    }

    return gMouseCursorHeight + gMouseCursorY > top
        && right >= gMouseCursorX
        && gMouseCursorWidth + gMouseCursorX > left
        && bottom >= gMouseCursorY;
}

// 0x4CA934
bool _mouse_click_in(int left, int top, int right, int bottom)
{
    if (!gMouseInitialized) {
        return false;
    }

    return _mouse_hoty + gMouseCursorY >= top
        && _mouse_hotx + gMouseCursorX <= right
        && _mouse_hotx + gMouseCursorX >= left
        && _mouse_hoty + gMouseCursorY <= bottom;
}

// 0x4CA9A0
void mouseGetRect(Rect* rect)
{
    rect->left = gMouseCursorX;
    rect->top = gMouseCursorY;
    rect->right = gMouseCursorWidth + gMouseCursorX - 1;
    rect->bottom = gMouseCursorHeight + gMouseCursorY - 1;
}

// 0x4CA9DC
void mouseGetPosition(int* xPtr, int* yPtr)
{
    *xPtr = _mouse_hotx + gMouseCursorX;
    *yPtr = _mouse_hoty + gMouseCursorY;
}

// 0x4CAA04
void _mouse_set_position(int x, int y)
{
    // Teleporting a visible cursor must erase it at the old position first,
    // or its image stays baked into the screen buffer (touch taps warp the
    // cursor constantly; during idle nothing repaints over the leftovers).
    // Both refreshes land within one frame, so nothing flickers.
    bool wasVisible = gMouseInitialized && !gCursorIsHidden;
    if (wasVisible) {
        mouseHideCursor();
    }

    gMouseCursorX = x - _mouse_hotx;
    gMouseCursorY = y - _mouse_hoty;
    _raw_y = y - _mouse_hoty;
    _raw_x = x - _mouse_hotx;
    _mouse_clip();

    if (wasVisible) {
        mouseShowCursor();
    }
}

// 0x4CAA38
static void _mouse_clip()
{
    if (_mouse_hotx + gMouseCursorX < _scr_size.left) {
        gMouseCursorX = _scr_size.left - _mouse_hotx;
    } else if (_mouse_hotx + gMouseCursorX > _scr_size.right) {
        gMouseCursorX = _scr_size.right - _mouse_hotx;
    }

    if (_mouse_hoty + gMouseCursorY < _scr_size.top) {
        gMouseCursorY = _scr_size.top - _mouse_hoty;
    } else if (_mouse_hoty + gMouseCursorY > _scr_size.bottom) {
        gMouseCursorY = _scr_size.bottom - _mouse_hoty;
    }
}

// 0x4CAAA0
int mouseGetEvent()
{
    return gMouseEvent;
}

// 0x4CAAA8
bool cursorIsHidden()
{
    return gCursorIsHidden;
}

// 0x4CAB5C
void _mouse_get_raw_state(int* out_x, int* out_y, int* out_buttons)
{
    MouseData mouseData;
    if (!mouseDeviceGetData(&mouseData)) {
        mouseData.x = 0;
        mouseData.y = 0;
        mouseData.buttons[0] = (gMouseEvent & MOUSE_EVENT_LEFT_BUTTON_DOWN) != 0;
        mouseData.buttons[1] = (gMouseEvent & MOUSE_EVENT_RIGHT_BUTTON_DOWN) != 0;
    }

    _raw_buttons = 0;
    if (mouseDeviceUsesRelativeMode()) {
        _raw_x += mouseData.x;
        _raw_y += mouseData.y;
    } else {
        _raw_x = mouseData.x;
        _raw_y = mouseData.y;
    }

    if (mouseData.buttons[0] != 0) {
        _raw_buttons |= MOUSE_EVENT_LEFT_BUTTON_DOWN;
    }

    if (mouseData.buttons[1] != 0) {
        _raw_buttons |= MOUSE_EVENT_RIGHT_BUTTON_DOWN;
    }

    *out_x = _raw_x;
    *out_y = _raw_y;
    *out_buttons = _raw_buttons;
}

// 0x4CAC3C
void mouseSetSensitivity(double value)
{
    if (value >= MOUSE_SENSITIVITY_MIN && value <= MOUSE_SENSITIVITY_MAX) {
        gMouseSensitivity = value;
    }
}

void mouseGetPositionInWindow(int win, int* x, int* y)
{
    mouseGetPosition(x, y);

    Window* window = windowGetWindow(win);
    if (window != nullptr) {
        *x -= window->rect.left;
        *y -= window->rect.top;
    }
}

bool mouseHitTestInWindow(int win, int left, int top, int right, int bottom)
{
    Window* window = windowGetWindow(win);
    if (window != nullptr) {
        left += window->rect.left;
        top += window->rect.top;
        right += window->rect.left;
        bottom += window->rect.top;
    }

    return _mouse_click_in(left, top, right, bottom);
}

void mouseGetWheel(int* x, int* y)
{
    *x = gMouseWheelX;
    *y = gMouseWheelY;
}

void convertMouseWheelToArrowKey(int* keyCodePtr)
{
    if (*keyCodePtr == -1) {
        if ((mouseGetEvent() & MOUSE_EVENT_WHEEL) != 0) {
            int wheelX;
            int wheelY;
            mouseGetWheel(&wheelX, &wheelY);

            if (wheelY > 0) {
                *keyCodePtr = KEY_ARROW_UP;
            } else if (wheelY < 0) {
                *keyCodePtr = KEY_ARROW_DOWN;
            }
        }
    }
}

int mouse_get_last_buttons()
{
    return last_buttons;
}

} // namespace fallout
