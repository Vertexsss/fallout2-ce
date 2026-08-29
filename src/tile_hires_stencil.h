#ifndef TILE_HIRES_STENCIL_H
#define TILE_HIRES_STENCIL_H

#include "geometry.h"

namespace fallout {

void tile_hires_stencil_init();

void tile_hires_stencil_on_map_load();

void tile_hires_stencil_on_center_tile_or_elevation_change();

void tile_hires_stencil_draw(Rect* rect, unsigned char* buffer, int windowWidth, int windowHeight);

// Live toggle from the in-game settings screen: rebuilds or clears the
// stencil caches and refreshes the view.
void tile_hires_stencil_set_enabled(bool enabled);

// Even when scrollblockers allow it, disallow scrolling on big resolutions
// if there is nothing but void to show there.
bool tile_hires_stencil_allows_scrolling_to_tile(int newCenterTile, int currentCenterTile, int elevation, int windowWidth, int windowHeight);

// After a forced centering (map load, teleport) auto-scrolls a little until
// the black overlay is out of view. Returns the adjusted center tile.
int tile_hires_stencil_get_tweaked_center_tile(int initialCenterTile, int elevation, int windowWidth, int windowHeight);

} // namespace fallout

#endif