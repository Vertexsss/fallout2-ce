#ifndef FALLOUT_TOUCH_OVERLAY_H_
#define FALLOUT_TOUCH_OVERLAY_H_

namespace fallout {

// Two small always-in-gameplay overlay buttons:
// - CFG in the top-right corner opens the settings screen;
// - HLT in the bottom-left corner (above the interface bar) toggles item
//   highlighting - the same simulated Shift hold as the three-finger
//   long-press gesture, but as a sticky on/off button.
// Cross-platform (engine-drawn windows), so it is testable on desktop where
// clicks go through regular window-manager buttons; on iOS taps are routed
// by the touch dispatcher without moving the cursor.

// Key code posted by the HLT button through the input queue on desktop.
constexpr int kTouchOverlayHighlightKeyCode = 2001;

void touchOverlayInit();
void touchOverlayFree();
void touchOverlayShow();
void touchOverlayHide();

// True when the screen point falls within one of the overlay buttons.
bool touchOverlayContainsPoint(int x, int y);

// Invokes the action under the screen point without moving the cursor.
bool touchOverlayHandleTap(int x, int y);

// Toggles the simulated-Shift item highlighting and repaints the button.
void touchOverlayToggleHighlight();

// Releases the highlight if it is on. Called before a world tap is simulated:
// Shift also modifies movement (walk instead of run), so a held simulated
// Shift would make every movement tap crawl.
void touchOverlayReleaseHighlight();

} // namespace fallout

#endif // FALLOUT_TOUCH_OVERLAY_H_
