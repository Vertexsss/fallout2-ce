#ifndef FALLOUT_CAMERA_FOLLOW_H_
#define FALLOUT_CAMERA_FOLLOW_H_

#include "obj_types.h"

namespace fallout {

// Smart camera follow. When the player starts a walk while standing inside
// the central zone of the view, the camera glides along (pixel-smooth) until
// the walk ends. A walk that starts with the player off-center - the view
// was deliberately panned elsewhere - leaves the camera alone. Any manual
// pan cancels the glide. Toggled by settings.ui.follow_hero.

// Called when a walk/run animation is registered for an object.
void cameraFollowOnWalkRegistered(Object* owner);

// Stops an active glide (manual pan took over).
void cameraFollowCancel();

// Per-frame ticker: keeps the walking player centered.
void cameraFollowTick();

} // namespace fallout

#endif // FALLOUT_CAMERA_FOLLOW_H_
