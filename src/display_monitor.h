#ifndef DISPLAY_MONITOR_H
#define DISPLAY_MONITOR_H

namespace fallout {

int displayMonitorInit();
int displayMonitorReset();
void displayMonitorExit();
void displayMonitorAddMessage(const char* string);
void displayMonitorDisable();

// Touch scrolling (see mouse.cc): hit test in screen coordinates, pan by
// the finger's delta, release with its velocity (px/s, positive = down).
bool displayMonitorTouchHitTest(int x, int y);
void displayMonitorTouchPan(int dyPixels);
void displayMonitorTouchRelease(double fingerVelocityPxPerSec);
void displayMonitorEnable();

} // namespace fallout

#endif /* DISPLAY_MONITOR_H */
