#include "touch.h"

#include <algorithm>
#include <queue>

#include "mouse.h"
#include "svga.h"

namespace fallout {

#define TOUCH_PHASE_BEGAN 0
#define TOUCH_PHASE_MOVED 1
#define TOUCH_PHASE_ENDED 2

#define MAX_TOUCHES 10

#define TAP_MAXIMUM_DURATION 75
// A tap by an extra finger while a one-finger gesture is held (secondary
// tap = right click / cursor mode switch).
#define SECONDARY_TAP_MAXIMUM_DURATION 250
#define PAN_MINIMUM_MOVEMENT 4
#define LONG_PRESS_MINIMUM_DURATION 500

struct TouchLocation {
    int x;
    int y;
};

struct Touch {
    bool used;
    SDL_FingerID fingerId;
    TouchLocation startLocation;
    Uint32 startTimestamp;
    TouchLocation currentLocation;
    Uint32 currentTimestamp;
    int phase;
};

static Touch touches[MAX_TOUCHES];
static Gesture currentGesture;
// FIFO: with a stack, frames that push more than one gesture (a synthetic
// secondary tap plus the continuous-gesture update) bury the tap under an
// ever-refreshed top - it either never surfaces or fires in a burst after
// the fingers lift.
static std::queue<Gesture> gestureEventsQueue;

static bool gUseTouchscreenMode = false;
// A secondary tap fired in this sequence - swallow the primary finger's own
// tap when it finally lifts, so a sloppy two-finger tap does not also click.
static bool gSuppressPrimaryTap = false;
static bool gUsePanMode = false;

static int find_touch(SDL_FingerID fingerId)
{
    for (int index = 0; index < MAX_TOUCHES; index++) {
        if (touches[index].fingerId == fingerId) {
            return index;
        }
    }
    return -1;
}

static int find_unused_touch_index()
{
    for (int index = 0; index < MAX_TOUCHES; index++) {
        if (!touches[index].used) {
            return index;
        }
    }
    return -1;
}

static TouchLocation touch_get_start_location_centroid(int* indexes, int length)
{
    TouchLocation centroid;
    centroid.x = 0;
    centroid.y = 0;
    for (int index = 0; index < length; index++) {
        centroid.x += touches[indexes[index]].startLocation.x;
        centroid.y += touches[indexes[index]].startLocation.y;
    }
    centroid.x /= length;
    centroid.y /= length;
    return centroid;
}

static TouchLocation touch_get_current_location_centroid(int* indexes, int length)
{
    TouchLocation centroid;
    centroid.x = 0;
    centroid.y = 0;
    for (int index = 0; index < length; index++) {
        centroid.x += touches[indexes[index]].currentLocation.x;
        centroid.y += touches[indexes[index]].currentLocation.y;
    }
    centroid.x /= length;
    centroid.y /= length;
    return centroid;
}

void touch_handle_start(SDL_TouchFingerEvent* event)
{
    // On iOS `fingerId` is an address of underlying `UITouch` object. When
    // `touchesBegan` is called this object might be reused, but with
    // incresed `tapCount` (which is ignored in this implementation).
    int index = find_touch(event->fingerId);
    if (index == -1) {
        index = find_unused_touch_index();
    }

    if (index != -1) {
        Touch* touch = &(touches[index]);
        touch->used = true;
        touch->fingerId = event->fingerId;
        touch->startTimestamp = event->timestamp;
        touch->startLocation.x = static_cast<int>(event->x * screenGetWidth());
        touch->startLocation.y = static_cast<int>(event->y * screenGetHeight());
        touch->currentTimestamp = touch->startTimestamp;
        touch->currentLocation = touch->startLocation;
        touch->phase = TOUCH_PHASE_BEGAN;
    }
}

void touch_handle_move(SDL_TouchFingerEvent* event)
{
    int index = find_touch(event->fingerId);
    if (index != -1) {
        Touch* touch = &(touches[index]);
        touch->currentTimestamp = event->timestamp;
        touch->currentLocation.x = static_cast<int>(event->x * screenGetWidth());
        touch->currentLocation.y = static_cast<int>(event->y * screenGetHeight());
        touch->phase = TOUCH_PHASE_MOVED;
    }
}

void touch_handle_end(SDL_TouchFingerEvent* event)
{
    int index = find_touch(event->fingerId);
    if (index != -1) {
        Touch* touch = &(touches[index]);
        touch->currentTimestamp = event->timestamp;
        touch->currentLocation.x = static_cast<int>(event->x * screenGetWidth());
        touch->currentLocation.y = static_cast<int>(event->y * screenGetHeight());
        touch->phase = TOUCH_PHASE_ENDED;
    }
}

void touch_process_gesture()
{
    Uint32 sequenceStartTimestamp = -1;
    int sequenceStartIndex = -1;

    // Find start of sequence (earliest touch).
    for (int index = 0; index < MAX_TOUCHES; index++) {
        if (touches[index].used) {
            if (sequenceStartTimestamp > touches[index].startTimestamp) {
                sequenceStartTimestamp = touches[index].startTimestamp;
                sequenceStartIndex = index;
            }
        }
    }

    if (sequenceStartIndex == -1) {
        return;
    }

    Uint32 sequenceEndTimestamp = -1;
    if (touches[sequenceStartIndex].phase == TOUCH_PHASE_ENDED) {
        sequenceEndTimestamp = touches[sequenceStartIndex].currentTimestamp;

        // Find end timestamp of sequence.
        for (int index = 0; index < MAX_TOUCHES; index++) {
            if (touches[index].used
                && touches[index].startTimestamp >= sequenceStartTimestamp
                && touches[index].startTimestamp <= sequenceEndTimestamp) {
                if (touches[index].phase == TOUCH_PHASE_ENDED) {
                    if (sequenceEndTimestamp < touches[index].currentTimestamp) {
                        sequenceEndTimestamp = touches[index].currentTimestamp;

                        // Start over since we can have fingers missed.
                        index = -1;
                    }
                } else {
                    // Sequence is current.
                    sequenceEndTimestamp = -1;
                    break;
                }
            }
        }
    }

    int active[MAX_TOUCHES];
    int activeCount = 0;

    int ended[MAX_TOUCHES];
    int endedCount = 0;

    // Split participating fingers into two buckets - active fingers (currently
    // on screen) and ended (lifted up).
    for (int index = 0; index < MAX_TOUCHES; index++) {
        if (touches[index].used
            && touches[index].currentTimestamp >= sequenceStartTimestamp
            && touches[index].currentTimestamp <= sequenceEndTimestamp) {
            if (touches[index].phase == TOUCH_PHASE_ENDED) {
                ended[endedCount++] = index;
            } else {
                active[activeCount++] = index;
            }

            // If this sequence is over, unmark participating finger as used.
            if (sequenceEndTimestamp != -1) {
                touches[index].used = false;
            }
        }
    }

    const bool sequenceOver = sequenceEndTimestamp != static_cast<Uint32>(-1);

    if (currentGesture.type == kPan || currentGesture.type == kLongPress) {
        if (currentGesture.state != kEnded) {
            // For continuous gestures we want number of fingers to remain the
            // same as it was when gesture was recognized.
            if (activeCount == currentGesture.numberOfTouches && endedCount == 0) {
                TouchLocation centroid = touch_get_current_location_centroid(active, activeCount);
                currentGesture.state = kChanged;
                currentGesture.x = centroid.x;
                currentGesture.y = centroid.y;
                gestureEventsQueue.push(currentGesture);
            } else if (currentGesture.numberOfTouches == 1 && activeCount == 2 && endedCount == 0) {
                // A second finger landed while a one-finger gesture is held.
                // Keep the gesture alive following the primary (earliest)
                // finger: a quick lift makes a secondary tap (right click),
                // a longer hold converts into two-finger recognition (pan).
                int primary = active[0];
                int secondary = active[1];
                if (touches[primary].startTimestamp > touches[secondary].startTimestamp) {
                    std::swap(primary, secondary);
                }

                if (SDL_GetTicks() - touches[secondary].startTimestamp > SECONDARY_TAP_MAXIMUM_DURATION) {
                    currentGesture.state = kEnded;
                    gestureEventsQueue.push(currentGesture);
                    currentGesture.type = kUnrecognized;

                    // Restart both fingers so two-finger pan recognition
                    // starts fresh from the current positions.
                    Uint32 now = SDL_GetTicks();
                    touches[primary].startLocation = touches[primary].currentLocation;
                    touches[primary].startTimestamp = now;
                    touches[secondary].startLocation = touches[secondary].currentLocation;
                    touches[secondary].startTimestamp = now;
                } else {
                    currentGesture.state = kChanged;
                    currentGesture.x = touches[primary].currentLocation.x;
                    currentGesture.y = touches[primary].currentLocation.y;
                    gestureEventsQueue.push(currentGesture);
                }
            } else if (currentGesture.numberOfTouches == 1 && activeCount == 1 && endedCount == 1) {
                // The extra finger lifted while the primary stayed down.
                int secondary = ended[0];
                bool quickTap = touches[secondary].currentTimestamp - touches[secondary].startTimestamp <= SECONDARY_TAP_MAXIMUM_DURATION
                    && abs(touches[secondary].currentLocation.x - touches[secondary].startLocation.x) <= 2 * PAN_MINIMUM_MOVEMENT
                    && abs(touches[secondary].currentLocation.y - touches[secondary].startLocation.y) <= 2 * PAN_MINIMUM_MOVEMENT;

                // Consume the lifted finger so it does not linger in the
                // ended bucket for the rest of the sequence.
                touches[secondary].used = false;

                if (quickTap) {
                    Gesture tap;
                    tap.type = kTap;
                    tap.state = kEnded;
                    tap.numberOfTouches = 2;
                    tap.x = touches[active[0]].currentLocation.x;
                    tap.y = touches[active[0]].currentLocation.y;
                    gestureEventsQueue.push(tap);
                    gSuppressPrimaryTap = true;
                }

                currentGesture.state = kChanged;
                currentGesture.x = touches[active[0]].currentLocation.x;
                currentGesture.y = touches[active[0]].currentLocation.y;
                gestureEventsQueue.push(currentGesture);
            } else {
                currentGesture.state = kEnded;
                gestureEventsQueue.push(currentGesture);
            }
        }

        // Reset continuous gesture if when current sequence is over.
        if (currentGesture.state == kEnded && sequenceEndTimestamp != -1) {
            currentGesture.type = kUnrecognized;
        }
    } else {
        if (activeCount == 1 && endedCount == 1
            && touches[ended[0]].startTimestamp > touches[active[0]].startTimestamp) {
            // A second finger tapped while the first is held but no gesture
            // is recognized yet - same secondary tap as during pan.
            int secondary = ended[0];
            bool quickTap = touches[secondary].currentTimestamp - touches[secondary].startTimestamp <= SECONDARY_TAP_MAXIMUM_DURATION
                && abs(touches[secondary].currentLocation.x - touches[secondary].startLocation.x) <= 2 * PAN_MINIMUM_MOVEMENT
                && abs(touches[secondary].currentLocation.y - touches[secondary].startLocation.y) <= 2 * PAN_MINIMUM_MOVEMENT;

            touches[secondary].used = false;

            if (quickTap) {
                Gesture tap;
                tap.type = kTap;
                tap.state = kEnded;
                tap.numberOfTouches = 2;
                tap.x = touches[active[0]].currentLocation.x;
                tap.y = touches[active[0]].currentLocation.y;
                gestureEventsQueue.push(tap);
                gSuppressPrimaryTap = true;
            }
        } else if (activeCount == 0 && endedCount != 0) {
            // For taps we need all participating fingers to be both started
            // and ended simultaneously (within predefined threshold).
            Uint32 startEarliestTimestamp = -1;
            Uint32 startLatestTimestamp = 0;
            Uint32 endEarliestTimestamp = -1;
            Uint32 endLatestTimestamp = 0;

            for (int index = 0; index < endedCount; index++) {
                startEarliestTimestamp = std::min(startEarliestTimestamp, touches[ended[index]].startTimestamp);
                startLatestTimestamp = std::max(startLatestTimestamp, touches[ended[index]].startTimestamp);
                endEarliestTimestamp = std::min(endEarliestTimestamp, touches[ended[index]].currentTimestamp);
                endLatestTimestamp = std::max(endLatestTimestamp, touches[ended[index]].currentTimestamp);
            }

            if (endedCount == 1 && gSuppressPrimaryTap) {
                // The mode switch already fired off this finger's companion.
                gSuppressPrimaryTap = false;
            } else if (startLatestTimestamp - startEarliestTimestamp <= TAP_MAXIMUM_DURATION
                && endLatestTimestamp - endEarliestTimestamp <= TAP_MAXIMUM_DURATION) {
                TouchLocation currentCentroid = touch_get_current_location_centroid(ended, endedCount);

                currentGesture.type = kTap;
                currentGesture.state = kEnded;
                currentGesture.numberOfTouches = endedCount;
                currentGesture.x = currentCentroid.x;
                currentGesture.y = currentCentroid.y;
                gestureEventsQueue.push(currentGesture);

                // Reset tap gesture immediately.
                currentGesture.type = kUnrecognized;
            }
        } else if (activeCount != 0 && endedCount == 0) {
            TouchLocation startCentroid = touch_get_start_location_centroid(active, activeCount);
            TouchLocation currentCentroid = touch_get_current_location_centroid(active, activeCount);

            // Disambiguate between pan and long press.
            if (abs(currentCentroid.x - startCentroid.x) >= PAN_MINIMUM_MOVEMENT
                || abs(currentCentroid.y - startCentroid.y) >= PAN_MINIMUM_MOVEMENT) {
                currentGesture.type = kPan;
                currentGesture.state = kBegan;
                currentGesture.numberOfTouches = activeCount;
                currentGesture.x = currentCentroid.x;
                currentGesture.y = currentCentroid.y;
                gestureEventsQueue.push(currentGesture);
            } else if (SDL_GetTicks() - touches[active[0]].startTimestamp >= LONG_PRESS_MINIMUM_DURATION) {
                currentGesture.type = kLongPress;
                currentGesture.state = kBegan;
                currentGesture.numberOfTouches = activeCount;
                currentGesture.x = currentCentroid.x;
                currentGesture.y = currentCentroid.y;
                gestureEventsQueue.push(currentGesture);
            }

            if (gUseTouchscreenMode) {
                mouseHideCursor();
                _mouse_set_position(currentCentroid.x, currentCentroid.y);
                mouseShowCursor();
            }
        }
    }

    if (sequenceOver) {
        gSuppressPrimaryTap = false;
    }
}

bool touch_get_gesture(Gesture* gesture)
{
    if (gestureEventsQueue.empty()) {
        return false;
    }

    *gesture = gestureEventsQueue.front();
    gestureEventsQueue.pop();

    return true;
}

void touch_set_touchscreen_mode(const bool value)
{
    gUseTouchscreenMode = value;
}

bool touch_get_touchscreen_mode()
{
    return gUseTouchscreenMode;
}

void touch_set_pan_mode(const bool value)
{
    gUsePanMode = value;
}

bool touch_get_pan_mode()
{
    return gUsePanMode;
}
} // namespace fallout
