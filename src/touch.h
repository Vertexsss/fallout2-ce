#ifndef FALLOUT_TOUCH_H_
#define FALLOUT_TOUCH_H_

#include <SDL.h>

namespace fallout {

enum GestureType {
    kUnrecognized,
    kTap,
    kLongPress,
    kPan,
};

enum GestureState {
    kPossible,
    kBegan,
    kChanged,
    kEnded,
};

struct Gesture {
    GestureType type;
    GestureState state;
    int numberOfTouches;
    int x;
    int y;
};

void touch_handle_start(SDL_TouchFingerEvent* event);
void touch_handle_move(SDL_TouchFingerEvent* event);
void touch_handle_end(SDL_TouchFingerEvent* event);
void touch_process_gesture();
// Drops all finger state and queued gestures (app activation changes).
void touch_reset();
bool touch_get_gesture(Gesture* gesture);
void touch_set_touchscreen_mode(const bool value);
bool touch_get_touchscreen_mode();

// Modal screens that pop up over the world: the touch build runs the world
// cursor in trackpad mode, so taps there click wherever the cursor stands.
// Hold touchscreen mode (taps land under the finger) for the screen's
// lifetime and restore the caller's mode on every exit path.
struct TouchscreenModeScope {
    bool previous;
    TouchscreenModeScope()
        : previous(touch_get_touchscreen_mode())
    {
        touch_set_touchscreen_mode(true);
    }
    ~TouchscreenModeScope()
    {
        touch_set_touchscreen_mode(previous);
    }
};
void touch_set_pan_mode(const bool value);
bool touch_get_pan_mode();

} // namespace fallout

#endif /* FALLOUT_TOUCH_H_ */
