#ifndef FALLOUT_SETTINGS_SCREEN_H_
#define FALLOUT_SETTINGS_SCREEN_H_

namespace fallout {

// Modal in-game screen for graphics/power settings that have no place in the
// classic Preferences panel. Rows marked with * are written to fallout2.cfg
// and take effect on the next launch; everything else applies immediately.
void settingsScreenShow();

} // namespace fallout

#endif // FALLOUT_SETTINGS_SCREEN_H_
