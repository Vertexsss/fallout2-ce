#include "svga.h"

#include <limits.h>
#include <stdio.h>

#include <vector>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#if __APPLE__
#include <TargetConditionals.h>
#endif

#include "color.h"
#include "config.h"
#include "dinput.h"
#include "draw.h"
#include "game.h"
#include "interface.h"
#include "map.h"
#include "map_edge.h"
#include "memory.h"
#include "mouse.h"
#include "movie.h"
#include "scan_unimplemented.h"
#include "settings.h"
#include "text_font.h"
#include "tile.h"
#include "win32.h"
#include "window_manager.h"
#include "window_manager_private.h"

namespace fallout {

static bool createRenderer(int width, int height);
static void destroyRenderer();

// screen rect
Rect _scr_size;

// 0x6ACA18 scr_blit
void (*_scr_blit)(unsigned char* src, int src_pitch, int unused, int src_x, int src_y, int src_width, int src_height, int dest_x, int dest_y) = _GNW95_ShowRect;

// 0x6ACA1C zero_mem
void (*_zero_mem)() = nullptr;

SDL_Window* gSdlWindow = nullptr;
SDL_Surface* gSdlSurface = nullptr;
SDL_Renderer* gSdlRenderer = nullptr;
SDL_Texture* gSdlTexture = nullptr;
SDL_Surface* gSdlTextureSurface = nullptr;

// TODO: Remove once migration to update-render cycle is completed.
FpsLimiter sharedFpsLimiter;

// Union of everything written into `gSdlTextureSurface` since the last present.
// Distant small updates (the hex cursor near the bottom and an indicator in
// a corner) must not be fused into one near-fullscreen union - each rect is
// converted and uploaded separately. Overflow falls back to merging.
constexpr int kMaxDirtyRects = 16;
// Two rects this close (or overlapping) get merged.
constexpr int kDirtyMergeSlackPx = 32;
static SDL_Rect gDirtyRects[kMaxDirtyRects];
static int gDirtyRectCount = 0;

// World-space (iso window local) dirt for the GPU ring. Screen-space dirt
// covers the visible crop, but world renders in the margins (pan strips,
// off-crop animation) never reach the screen list - without these marks
// stale ring cells slide into view when the camera moves.
static SDL_Rect gWorldDirtyRects[kMaxDirtyRects];
static int gWorldDirtyRectCount = 0;

void renderMarkWorldDirty(const Rect* rect)
{
    SDL_Rect r;
    r.x = rect->left;
    r.y = rect->top;
    r.w = rect->right - rect->left + 1;
    r.h = rect->bottom - rect->top + 1;
    if (r.w <= 0 || r.h <= 0) {
        return;
    }
    bool merged = true;
    while (merged) {
        merged = false;
        for (int i = 0; i < gWorldDirtyRectCount; i++) {
            SDL_Rect inflated = gWorldDirtyRects[i];
            inflated.x -= kDirtyMergeSlackPx;
            inflated.y -= kDirtyMergeSlackPx;
            inflated.w += 2 * kDirtyMergeSlackPx;
            inflated.h += 2 * kDirtyMergeSlackPx;
            if (SDL_HasIntersection(&inflated, &r) == SDL_TRUE) {
                SDL_UnionRect(&gWorldDirtyRects[i], &r, &r);
                gWorldDirtyRects[i] = gWorldDirtyRects[gWorldDirtyRectCount - 1];
                gWorldDirtyRectCount--;
                merged = true;
                break;
            }
        }
    }
    if (gWorldDirtyRectCount < kMaxDirtyRects) {
        gWorldDirtyRects[gWorldDirtyRectCount++] = r;
    } else {
        SDL_UnionRect(&gWorldDirtyRects[0], &r, &gWorldDirtyRects[0]);
    }
}


// Which palette indices currently appear on the game surface. Lets ambient
// palette animation (color cycling) be skipped entirely when the cycled
// colors are not on screen - the "very fast" group ticks 30 times per second
// on every map whether or not its color is visible.
static bool gPaletteIndexPresent[256];
static bool gPalettePresenceValid = false;
static unsigned int gPalettePresenceScanTicks = 0;

// Cell grid of pixels using the cycling index range (229-255), computed
// alongside the presence map. A cycling tick only re-colors those pixels;
// per-cell bounding boxes keep distant fires from fusing into one
// near-fullscreen region.
static constexpr int kCycleRangeStart = 229;
constexpr int kCycleCellSize = 160;
constexpr int kCycleGridMaxCols = 16;
constexpr int kCycleGridMaxRows = 10;
struct CycleCell {
    short minX, minY, maxX, maxY;
};
static CycleCell gCycleCells[kCycleGridMaxCols * kCycleGridMaxRows];
static int gCycleGridCols = 0;
static int gCycleGridRows = 0;
static bool gCycleAnyPresent = false;
// The cell grid has been built at least once; stale cells are still a far
// better approximation than "the whole screen" - fires do not move between
// rescans, and anything new is caught by the next rescan within 250ms.
static bool gCycleCellsEverBuilt = false;

// Rendering statistics for the FPS counter (power tuning aid).
static int gStatPresents = 0;
static long long gStatUploadBytes = 0;
static int gStatRects = 0;
static int gStatMaxRectW = 0;
static int gStatMaxRectH = 0;

// Cycle cells live in WORLD (iso window local) coordinates when the iso
// world is up: fires just outside the 1x crop must keep animating in the
// ring or they visibly freeze at the zoom-out boundary. Falls back to the
// composited surface when the world is not available (menus, movies).
static bool gCycleCellsWorldSpace = false;

static void paletteRebuildPresence()
{
    memset(gPaletteIndexPresent, 0, sizeof(gPaletteIndexPresent));

    const unsigned char* pixels = nullptr;
    int scanW = 0;
    int scanH = 0;
    int scanPitch = 0;
    gCycleCellsWorldSpace = false;
    if (gIsoWindow != -1) {
        Window* isoWin = windowGetWindow(gIsoWindow);
        if (isoWin != nullptr && (isoWin->flags & WINDOW_HIDDEN) == 0 && isoWin->buffer != nullptr) {
            pixels = isoWin->buffer;
            scanW = isoWin->width;
            scanH = isoWin->height;
            scanPitch = isoWin->width;
            gCycleCellsWorldSpace = true;
        }
    }
    if (pixels == nullptr) {
        pixels = static_cast<const unsigned char*>(gSdlSurface->pixels);
        scanW = gSdlSurface->w;
        scanH = gSdlSurface->h;
        scanPitch = gSdlSurface->pitch;
    }

    gCycleGridCols = (scanW + kCycleCellSize - 1) / kCycleCellSize;
    gCycleGridRows = (scanH + kCycleCellSize - 1) / kCycleCellSize;
    if (gCycleGridCols > kCycleGridMaxCols) gCycleGridCols = kCycleGridMaxCols;
    if (gCycleGridRows > kCycleGridMaxRows) gCycleGridRows = kCycleGridMaxRows;

    for (int i = 0; i < gCycleGridCols * gCycleGridRows; i++) {
        gCycleCells[i] = { 0x7FFF, 0x7FFF, -1, -1 };
    }
    gCycleAnyPresent = false;

    const int cellW = (scanW + gCycleGridCols - 1) / gCycleGridCols;
    const int cellH = (scanH + gCycleGridRows - 1) / gCycleGridRows;

    const unsigned char* row = pixels;
    for (int y = 0; y < scanH; y++) {
        for (int x = 0; x < scanW; x++) {
            unsigned char index = row[x];
            gPaletteIndexPresent[index] = true;
            if (index >= kCycleRangeStart) {
                CycleCell& cell = gCycleCells[(y / cellH) * gCycleGridCols + (x / cellW)];
                if (x < cell.minX) cell.minX = static_cast<short>(x);
                if (x > cell.maxX) cell.maxX = static_cast<short>(x);
                if (y < cell.minY) cell.minY = static_cast<short>(y);
                if (y > cell.maxY) cell.maxY = static_cast<short>(y);
                gCycleAnyPresent = true;
            }
        }
        row += scanPitch;
    }

    gPalettePresenceValid = true;
    gCycleCellsEverBuilt = true;
}

// 0x4CAD08 init_mode_320_200
int _init_mode_320_200()
{
    return _GNW95_init_mode_ex(320, 200, 8);
}

// 0x4CAD40 init_mode_320_400
int _init_mode_320_400()
{
    return _GNW95_init_mode_ex(320, 400, 8);
}

// 0x4CAD5C init_mode_640_480_16
int _init_mode_640_480_16()
{
    return -1;
}

// 0x4CAD64 init_mode_640_480
int _init_mode_640_480()
{
    return _init_vesa_mode(640, 480);
}

// 0x4CAD94 init_mode_640_400
int _init_mode_640_400()
{
    return _init_vesa_mode(640, 400);
}

// 0x4CADA8 init_mode_800_600
int _init_mode_800_600()
{
    return _init_vesa_mode(800, 600);
}

// 0x4CADBC init_mode_1024_768
int _init_mode_1024_768()
{
    return _init_vesa_mode(1024, 768);
}

// 0x4CADD0 init_mode_1280_1024
int _init_mode_1280_1024()
{
    return _init_vesa_mode(1280, 1024);
}

// 0x4CADF8
void _get_start_mode_()
{
}

// 0x4CADFC zero_vid_mem
void _zero_vid_mem()
{
    if (_zero_mem) {
        _zero_mem();
    }
}

// 0x4CAE1C GNW95_init_mode_ex
int _GNW95_init_mode_ex(int width, int height, int bpp)
{
    width = settings.screen.resolution_x;
    height = settings.screen.resolution_y;
    int scale = settings.screen.scale;

    sharedFpsLimiter.setIdleFps(settings.screen.idle_fps);
    sharedFpsLimiter.setFpsCap(settings.screen.fps_cap);
    sharedFpsLimiter.setAutoPower(settings.ui.auto_power);
    sharedFpsLimiter.setIdleGrace(settings.screen.idle_grace_ms);

    // Only allow scaling if resulting game resolution is >= 640x480
    if ((width / scale) < 640 || (height / scale) < 480) {
        scale = 1;
    } else {
        width /= scale;
        height /= scale;
    }

    if (_GNW95_init_window(width, height, settings.screen.windowed, scale) == -1) {
        return -1;
    }

    if (directDrawInit(width, height, bpp) == -1) {
        return -1;
    }

    // macOS seems to require dequeuing NSApp events in order for window to
    // become visible. There is no concrete number of calls required to make
    // it happen. Sadly there is no particular event to watch for because SDL
    // marks window as shown immediately after creation (see
    // `SDL_FinishWindowCreation`).
    for (int i = 0; i < 10; i++) {
        SDL_PumpEvents();
    }

    _scr_size.left = 0;
    _scr_size.top = 0;
    _scr_size.right = width - 1;
    _scr_size.bottom = height - 1;

    _mouse_blit_trans = nullptr;
    _scr_blit = _GNW95_ShowRect;
    _zero_mem = _GNW95_zero_vid_mem;
    _mouse_blit = _GNW95_ShowRect;

    return 0;
}

// 0x4CAECC init_vesa_mode
int _init_vesa_mode(int width, int height)
{
    return _GNW95_init_mode_ex(width, height, 8);
}

// 0x4CAEDC GNW95_init_window
int _GNW95_init_window(int width, int height, WindowMode mode, int scale)
{
    if (gSdlWindow == nullptr) {
        // Touches are fully handled by the gesture recognizer (touch.cc).
        // SDL's default synthetic mouse events from touches drove the cursor
        // a SECOND time through mouseDeviceGetData: in relative mode the
        // deltas accumulated during a drag (while the gesture branch skips
        // the device poll) were dumped in one lump when the fingers lifted,
        // flinging the cursor into the screen edges.
        SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
        // Nor should a real mouse/trackpad feed the touch pipeline
        // (defaults to on for mobile SDL builds).
        SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");

#if __APPLE__ && TARGET_OS_IOS
        // OpenGL ES is deprecated on iOS and runs through a translation layer,
        // which costs both CPU and GPU power. Metal is the native path.
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "metal");

        Uint32 windowFlags = SDL_WINDOW_METAL | SDL_WINDOW_ALLOW_HIGHDPI;
#else
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");

        Uint32 windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI;
#endif

        if (mode == WindowMode::Fullscreen) {
            windowFlags |= SDL_WINDOW_FULLSCREEN;
        } else if (mode == WindowMode::WindowedFullscreen) {
            windowFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        }

        gSdlWindow = SDL_CreateWindow(gProgramWindowTitle, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width * scale, height * scale, windowFlags);
        if (gSdlWindow == nullptr) {
            return -1;
        }

        if (!createRenderer(width, height)) {
            destroyRenderer();

            SDL_DestroyWindow(gSdlWindow);
            gSdlWindow = nullptr;

            return -1;
        }
    }

    return 0;
}

// 0x4CAF9C GNW95_init_DirectDraw
int directDrawInit(int width, int height, int bpp)
{
    if (gSdlSurface != nullptr) {
        unsigned char* palette = directDrawGetPalette();
        directDrawFree();

        if (directDrawInit(width, height, bpp) == -1) {
            return -1;
        }

        directDrawSetPalette(palette);

        return 0;
    }

    gSdlSurface = SDL_CreateRGBSurface(0, width, height, bpp, 0, 0, 0, 0);

    SDL_Color colors[256];
    for (int index = 0; index < 256; index++) {
        colors[index].r = index;
        colors[index].g = index;
        colors[index].b = index;
        colors[index].a = 255;
    }

    SDL_SetPaletteColors(gSdlSurface->format->palette, colors, 0, 256);

    return 0;
}

// 0x4CB1B0 GNW95_reset_mode
void directDrawFree()
{
    if (gSdlSurface != nullptr) {
        SDL_FreeSurface(gSdlSurface);
        gSdlSurface = nullptr;
    }
}

// 0x4CB310 GNW95_SetPaletteEntries
void directDrawSetPaletteInRange(unsigned char* palette, int start, int count)
{
    if (gSdlSurface != nullptr && gSdlSurface->format->palette != nullptr) {
        SDL_Color colors[256];

        if (count != 0) {
            for (int index = 0; index < count; index++) {
                colors[index].r = palette[index * 3] << 2;
                colors[index].g = palette[index * 3 + 1] << 2;
                colors[index].b = palette[index * 3 + 2] << 2;
                colors[index].a = 255;
            }
        }

        SDL_Palette* sdlPalette = gSdlSurface->format->palette;

        // Rebuild the presence map when it is stale, at most 4 times per
        // second; while stale, assume every index is visible (safe).
        unsigned int now = SDL_GetTicks();
        if (!gPalettePresenceValid && now - gPalettePresenceScanTicks >= 250) {
            paletteRebuildPresence();
            gPalettePresenceScanTicks = now;
        }

        bool visibleChange = false;
        for (int index = 0; index < count; index++) {
            const SDL_Color& current = sdlPalette->colors[start + index];
            if (current.r != colors[index].r || current.g != colors[index].g || current.b != colors[index].b) {
                if (!gPalettePresenceValid || gPaletteIndexPresent[start + index]) {
                    visibleChange = true;
                    break;
                }
            }
        }

        SDL_SetPaletteColors(sdlPalette, colors, start, count);

        // Ambient palette animation (color cycling). Defer the re-conversion
        // to the next present and do not count it as user activity, so the
        // idle FPS limiter still engages on otherwise static scenes. If the
        // cycled colors are not even on screen, skip the update entirely.
        // When the update stays within the cycling range and we know where
        // those pixels are, restrict the re-conversion to their bounding box.
        if (visibleChange) {
            const bool cycleOnly = start >= kCycleRangeStart && start + count <= 256;
            if (cycleOnly && gCycleCellsEverBuilt && gCycleAnyPresent) {
                int cellMarginX = 0;
                int cellMarginY = 0;
                if (gCycleCellsWorldSpace) {
                    mapGetIsoMargins(&cellMarginX, &cellMarginY);
                }
                for (int i = 0; i < gCycleGridCols * gCycleGridRows; i++) {
                    const CycleCell& cell = gCycleCells[i];
                    if (cell.maxX >= 0) {
                        // World-space cells convert to screen for the mark;
                        // the ambient mirror carries the unclipped world
                        // rect into the ring list, so off-crop fires keep
                        // animating at zoom-out.
                        SDL_Rect r = { cell.minX - cellMarginX, cell.minY - cellMarginY,
                            cell.maxX - cell.minX + 1, cell.maxY - cell.minY + 1 };
                        renderMarkDirtyAmbient(&r);
                    }
                }
            } else {
                renderMarkDirtyAmbient(nullptr);
            }
        }
    }
}

// 0x4CB568 GNW95_SetPalette
void directDrawSetPalette(unsigned char* palette)
{
    if (gSdlSurface != nullptr && gSdlSurface->format->palette != nullptr) {
        SDL_Color colors[256];

        for (int index = 0; index < 256; index++) {
            colors[index].r = palette[index * 3] << 2;
            colors[index].g = palette[index * 3 + 1] << 2;
            colors[index].b = palette[index * 3 + 2] << 2;
            colors[index].a = 255;
        }

        SDL_SetPaletteColors(gSdlSurface->format->palette, colors, 0, 256);

        // Full palette sets come from fades - keep them counted as activity
        // so fades run at full frame rate.
        renderMarkDirty(nullptr);
    }
}

// 0x4CB68C GNW95_GetPalette
unsigned char* directDrawGetPalette()
{
    // 0x6ACA24
    static unsigned char palette[768];

    if (gSdlSurface != nullptr && gSdlSurface->format->palette != nullptr) {
        SDL_Color* colors = gSdlSurface->format->palette->colors;

        for (int index = 0; index < 256; index++) {
            SDL_Color* color = &(colors[index]);
            palette[index * 3] = color->r >> 2;
            palette[index * 3 + 1] = color->g >> 2;
            palette[index * 3 + 2] = color->b >> 2;
        }
    }

    return palette;
}

// 0x4CB850 GNW95_ShowRect
void _GNW95_ShowRect(unsigned char* src, int srcPitch, int unused, int srcX, int srcY, int srcWidth, int srcHeight, int destX, int destY)
{
    (void)unused;

    blitBufferToBuffer(src + srcPitch * srcY + srcX, srcWidth, srcHeight, srcPitch, (unsigned char*)gSdlSurface->pixels + gSdlSurface->pitch * destY + destX, gSdlSurface->pitch);

    // Surface content changed - the palette presence map is stale.
    gPalettePresenceValid = false;

    SDL_Rect srcRect;
    srcRect.x = destX;
    srcRect.y = destY;
    srcRect.w = srcWidth;
    srcRect.h = srcHeight;

    SDL_Rect destRect;
    destRect.x = destX;
    destRect.y = destY;
    // Conversion to RGB happens once per presented frame in renderPresent.
    // Ambient: world animation (critters, fires, the bobbing hex cursor)
    // must not keep the idle limiter at full rate - it still gets presented,
    // just at the idle FPS, which comfortably covers the ~10fps artwork.
    // Real user input marks activity in the event pump instead.
    renderMarkDirtyAmbient(&srcRect);
}

// Clears drawing surface.
//
// 0x4CBBC8 GNW95_zero_vid_mem
void _GNW95_zero_vid_mem()
{
    if (!gProgramIsActive) {
        return;
    }

    unsigned char* surface = (unsigned char*)gSdlSurface->pixels;
    for (int y = 0; y < gSdlSurface->h; y++) {
        memset(surface, 0, gSdlSurface->w);
        surface += gSdlSurface->pitch;
    }

    gPalettePresenceValid = false;

    renderMarkDirtyAmbient(nullptr);
}

int screenGetWidth()
{
    // TODO: Make it on par with _xres;
    return rectGetWidth(&_scr_size);
}

int screenGetHeight()
{
    // TODO: Make it on par with _yres.
    return rectGetHeight(&_scr_size);
}

int screenGetVisibleHeight()
{
    int windowBottomMargin = 0;

    if (!settings.ui.iface_bar_mode) {
        windowBottomMargin = INTERFACE_BAR_HEIGHT;
    }
    return screenGetHeight() - windowBottomMargin;
}

// returns true if the game is running in fullscreen mode, false otherwise (including windowed fullscreen mode)
bool screenIsExclusiveFullscreen()
{
    Uint32 flags = SDL_GetWindowFlags(gSdlWindow);
    return (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN;
}

static bool createRenderer(int width, int height)
{
    gSdlRenderer = SDL_CreateRenderer(gSdlWindow, -1, 0);
    if (gSdlRenderer == nullptr) {
        return false;
    }

    if (SDL_RenderSetLogicalSize(gSdlRenderer, width, height) != 0) {
        return false;
    }

    gSdlTexture = SDL_CreateTexture(gSdlRenderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, width, height);
    if (gSdlTexture == nullptr) {
        return false;
    }

    Uint32 format;
    if (SDL_QueryTexture(gSdlTexture, &format, nullptr, nullptr, nullptr) != 0) {
        return false;
    }

    gSdlTextureSurface = SDL_CreateRGBSurfaceWithFormat(0, width, height, SDL_BITSPERPIXEL(format), format);
    if (gSdlTextureSurface == nullptr) {
        return false;
    }

    return true;
}

static void isoRingDestroy();

static void destroyRenderer()
{
    // The recorded dirty regions are only meaningful for the surface they
    // were recorded against - forget them before that surface goes away.
    gDirtyRectCount = 0;

    isoRingDestroy();

    if (gSdlTextureSurface != nullptr) {
        SDL_FreeSurface(gSdlTextureSurface);
        gSdlTextureSurface = nullptr;
    }

    if (gSdlTexture != nullptr) {
        SDL_DestroyTexture(gSdlTexture);
        gSdlTexture = nullptr;
    }

    if (gSdlRenderer != nullptr) {
        SDL_DestroyRenderer(gSdlRenderer);
        gSdlRenderer = nullptr;
    }
}

void handleWindowSizeChanged()
{
    movieHandleRendererReset();
    destroyRenderer();
    createRenderer(screenGetWidth(), screenGetHeight());

    // The new surface starts out blank - the next present re-converts and
    // uploads the game surface in full.
    renderMarkDirty(nullptr);

    mouseDeviceRefreshWindowMapping();
}

void renderFpsCounter()
{
    if (!settings.debug.show_fps || gSdlSurface == nullptr || gSdlTextureSurface == nullptr) {
        return;
    }

    static unsigned int sampleStartTicks = 0;
    static int sampleFrames = 0;
    static double fps = 0.0;

    unsigned int now = SDL_GetTicks();
    if (sampleStartTicks == 0) {
        sampleStartTicks = now;
    }

    sampleFrames++;

    static double presentsPerSec = 0.0;
    static double uploadMbPerSec = 0.0;
    static double rectsPerSec = 0.0;
    static int maxRectW = 0;
    static int maxRectH = 0;

    unsigned int elapsed = now - sampleStartTicks;
    if (elapsed >= 500) {
        fps = sampleFrames * 1000.0 / elapsed;
        presentsPerSec = gStatPresents * 1000.0 / elapsed;
        uploadMbPerSec = gStatUploadBytes * 1000.0 / elapsed / (1024.0 * 1024.0);
        rectsPerSec = gStatRects * 1000.0 / elapsed;
        maxRectW = gStatMaxRectW;
        maxRectH = gStatMaxRectH;
        sampleFrames = 0;
        gStatPresents = 0;
        gStatUploadBytes = 0;
        gStatRects = 0;
        gStatMaxRectW = 0;
        gStatMaxRectH = 0;
        sampleStartTicks = now;
    }

    char text[112];
    snprintf(text, sizeof(text), "FPS: %.1f  P: %.0f/s  U: %.1fMB/s  T: %u  R: %.0f/s  L: %dx%d  B: %ums",
        fps, presentsPerSec, uploadMbPerSec, sharedFpsLimiter.lastTargetFps(), rectsPerSec, maxRectW, maxRectH,
        sharedFpsLimiter.busyMsPerSec());

    ScopedFont font(101);

    constexpr int kPadding = 2;
    int textWidth = fontGetStringWidth(text);
    int textHeight = fontGetLineHeight();
    int width = textWidth + kPadding * 2;
    int height = textHeight + kPadding * 2;

    if (width > gSdlSurface->w) {
        width = gSdlSurface->w;
    }

    if (height > gSdlSurface->h) {
        height = gSdlSurface->h;
    }

    // Bottom-left, over the interface bar: outside the GPU pan ring, so
    // the per-frame mark can never merge with a pan strip (top-left it
    // fused with the left strip into a 420x380 upload every frame - pans
    // to one side cost 6x the other).
    int overlayY = gSdlSurface->h - height;
    unsigned char* overlayDest = static_cast<unsigned char*>(gSdlSurface->pixels) + static_cast<size_t>(overlayY) * gSdlSurface->pitch;
    bufferFill(overlayDest, width, height, gSdlSurface->pitch, COLOR_BLACK);
    if (width > kPadding * 2 && height > kPadding * 2) {
        fontDrawText(overlayDest + gSdlSurface->pitch * kPadding + kPadding, text, width - kPadding * 2, gSdlSurface->pitch, COLOR_LIGHT_GREY);
    }

    SDL_Rect rect;
    rect.x = 0;
    rect.y = overlayY;
    rect.w = width;
    rect.h = height;
    // Ambient: the counter itself must not keep the idle limiter awake,
    // otherwise it distorts the very numbers it displays.
    renderMarkDirtyAmbient(&rect);
}

// ---- GPU-composited iso view ----
//
// The iso window is the bottom-most game window; during a pan every pixel
// of it changes, which forced a full-viewport convert + upload per frame.
// Instead its content lives in a ring-addressed texture: screen (x, y)
// maps to texture ((x + ox) % W, (y + oy) % H). A pan advances the origin
// and only the exposed strips are uploaded; the GPU composes the view from
// up to four wrapped pieces, then draws the windows above the iso window
// (and the software cursor) from the classic full-screen texture, whose
// pixels for exactly those rects are always current.
static SDL_Texture* gIsoRingTexture = nullptr;
// Keyed RGBA copy of the cursor art for the zoomed compose - pasting the
// cursor from the classic texture carried a rectangle of 1x-scale world
// around the arrow and displaced it toward the center.
static SDL_Texture* gCursorArtTexture = nullptr;
static int gCursorArtTextureW = 0;
static int gCursorArtTextureH = 0;
static int gIsoRingW = 0;
static int gIsoRingH = 0;
static int gIsoRingOx = 0;
static int gIsoRingOy = 0;
static bool gIsoRingContentValid = false;
static bool gIsoModeLastPresent = false;
static int gIsoShiftsSincePresent = 0;

// Pinch zoom: a pure presentation transform of the iso view. The engine
// keeps rendering the world 1:1 into the ring; the compose scales a
// centered crop of it to the window. UI windows above the iso view stay
// unscaled. Range 1.0 .. 1.5, snapped at both ends.
static double gIsoZoom = 1.0;
static bool gIsoZoomDirty = false;

static int isoRingWrap(int value, int size)
{
    value %= size;
    return value < 0 ? value + size : value;
}

static bool gLoadingScreenFrozen = false;

static bool isoGpuModeActive()
{
    if (gSdlRenderer == nullptr || gSdlTextureSurface == nullptr) {
        return false;
    }
    if (!settings.screen.gpu_iso) {
        return false;
    }
    // The loading-screen freeze repairs gSdlTextureSurface - it must be
    // what reaches the screen.
    if (gLoadingScreenFrozen) {
        return false;
    }
    // A movie covers everything and marks the full screen per frame -
    // composing (and re-uploading) the hidden ring under it would double
    // the upload for nothing.
    if (_moviePlaying()) {
        return false;
    }
    if (gIsoWindow == -1) {
        return false;
    }
    Window* window = windowGetWindow(gIsoWindow);
    if (window == nullptr || (window->flags & WINDOW_HIDDEN) != 0) {
        return false;
    }
    // The ring matches the iso world window only while it sits at its
    // margin-offset origin and spans the full oversized width.
    int marginX;
    int marginY;
    mapGetIsoMargins(&marginX, &marginY);
    return window->rect.left == -marginX && window->rect.top == -marginY
        && window->width == gSdlTextureSurface->w + 2 * marginX;
}

static bool isoRingEnsure(int width, int height)
{
    if (gIsoRingTexture != nullptr && (gIsoRingW != width || gIsoRingH != height)) {
        SDL_DestroyTexture(gIsoRingTexture);
        gIsoRingTexture = nullptr;
    }
    if (gIsoRingTexture == nullptr) {
        gIsoRingTexture = SDL_CreateTexture(gSdlRenderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, width, height);
        gIsoRingW = width;
        gIsoRingH = height;
        gIsoRingOx = 0;
        gIsoRingOy = 0;
        gIsoRingContentValid = false;
    }
    return gIsoRingTexture != nullptr;
}

static void isoRingDestroy()
{
    if (gCursorArtTexture != nullptr) {
        SDL_DestroyTexture(gCursorArtTexture);
        gCursorArtTexture = nullptr;
    }
    if (gIsoRingTexture != nullptr) {
        SDL_DestroyTexture(gIsoRingTexture);
        gIsoRingTexture = nullptr;
    }
    gIsoRingContentValid = false;
    gIsoModeLastPresent = false;
}

// Palette lookup for converting the 8-bit iso window buffer directly.
static Uint32 gIsoRingLut[256];

static void isoRingRebuildLut()
{
    SDL_Palette* palette = gSdlSurface->format->palette;
    for (int index = 0; index < 256; index++) {
        const SDL_Color& c = palette->colors[index];
        gIsoRingLut[index] = SDL_MapRGB(gSdlTextureSurface->format, c.r, c.g, c.b);
    }
}

// Upload a screen-space rect into the ring at wrapped positions - up to 4
// pieces. The pixels come from the ISO WINDOW's own buffer (pure world:
// floating text yes, but no windows drawn above and no software cursor) -
// sourcing the composited surface instead baked window and cursor pixels
// into the world, and they smeared across the screen as the origin moved.
static bool isoRingUpload(const SDL_Rect& r)
{
    unsigned char* windowBuffer = windowGetBuffer(gIsoWindow);
    if (windowBuffer == nullptr) {
        return false;
    }

    static std::vector<Uint32> scratch;
    scratch.resize(static_cast<size_t>(r.w) * r.h);
    for (int y = 0; y < r.h; y++) {
        const unsigned char* src = windowBuffer + static_cast<size_t>(r.y + y) * gIsoRingW + r.x;
        Uint32* dst = scratch.data() + static_cast<size_t>(y) * r.w;
        for (int x = 0; x < r.w; x++) {
            dst[x] = gIsoRingLut[src[x]];
        }
    }
    const int scratchPitch = r.w * 4;

    bool ok = true;
    int spansX[2][3]; // dstTexX, srcScreenX, width
    int spansY[2][3];
    int countX = 0;
    int countY = 0;

    int tx = isoRingWrap(r.x + gIsoRingOx, gIsoRingW);
    int firstW = gIsoRingW - tx < r.w ? gIsoRingW - tx : r.w;
    spansX[countX][0] = tx; spansX[countX][1] = r.x; spansX[countX][2] = firstW; countX++;
    if (firstW < r.w) {
        spansX[countX][0] = 0; spansX[countX][1] = r.x + firstW; spansX[countX][2] = r.w - firstW; countX++;
    }

    int ty = isoRingWrap(r.y + gIsoRingOy, gIsoRingH);
    int firstH = gIsoRingH - ty < r.h ? gIsoRingH - ty : r.h;
    spansY[countY][0] = ty; spansY[countY][1] = r.y; spansY[countY][2] = firstH; countY++;
    if (firstH < r.h) {
        spansY[countY][0] = 0; spansY[countY][1] = r.y + firstH; spansY[countY][2] = r.h - firstH; countY++;
    }

    for (int iy = 0; iy < countY; iy++) {
        for (int ix = 0; ix < countX; ix++) {
            SDL_Rect texRect;
            texRect.x = spansX[ix][0];
            texRect.y = spansY[iy][0];
            texRect.w = spansX[ix][2];
            texRect.h = spansY[iy][2];
            const unsigned char* pixels = reinterpret_cast<const unsigned char*>(scratch.data())
                + static_cast<size_t>(spansY[iy][1] - r.y) * scratchPitch
                + static_cast<size_t>(spansX[ix][1] - r.x) * 4;
            if (SDL_UpdateTexture(gIsoRingTexture, &texRect, pixels, scratchPitch) != 0) {
                ok = false;
            }
            gStatUploadBytes += static_cast<long long>(texRect.w) * texRect.h * 4;
        }
    }
    return ok;
}

// Draw the ring region starting at srcOffset onto the screen area
// (0,0,w,h) as up to 4 wrapped pieces.
static void isoRingCompose(int width, int height, int srcOffsetX, int srcOffsetY)
{
    int ox = isoRingWrap(gIsoRingOx + srcOffsetX, gIsoRingW);
    int oy = isoRingWrap(gIsoRingOy + srcOffsetY, gIsoRingH);
    int firstW = gIsoRingW - ox < width ? gIsoRingW - ox : width;
    int firstH = gIsoRingH - oy < height ? gIsoRingH - oy : height;

    for (int part = 0; part < 4; part++) {
        bool rightPart = (part & 1) != 0;
        bool bottomPart = (part & 2) != 0;
        SDL_Rect dst;
        dst.x = rightPart ? firstW : 0;
        dst.w = rightPart ? width - firstW : firstW;
        dst.y = bottomPart ? firstH : 0;
        dst.h = bottomPart ? height - firstH : firstH;
        if (dst.w <= 0 || dst.h <= 0) {
            continue;
        }
        SDL_Rect src;
        src.x = rightPart ? 0 : ox;
        src.y = bottomPart ? 0 : oy;
        src.w = dst.w;
        src.h = dst.h;
        SDL_RenderCopy(gSdlRenderer, gIsoRingTexture, &src, &dst);
    }
}

bool renderIsoPanShift(int screenDx, int screenDy)
{
    if (!isoGpuModeActive() || gSdlRenderer == nullptr) {
        return false;
    }
    Window* window = windowGetWindow(gIsoWindow);
    if (!isoRingEnsure(window->width, window->height)) {
        return false;
    }
    if (!gIsoRingContentValid) {
        // First pan since (re)creation - the classic full refresh that
        // the caller will do fills the ring.
        return false;
    }
    if (gIsoShiftsSincePresent >= 3) {
        // Runaway shifts within one present would balloon the re-marked
        // rects; fall back to a full refresh. Two shifts per present are
        // normal on the iPad (120Hz touch events against 60Hz presents) -
        // the shift-time re-marks below keep every pending rect correct,
        // and falling back on each second shift meant a 2.25x full-world
        // refresh every other pan frame (the "interior updates late"
        // feel).
        return false;
    }
    if (gDirtyRectCount > 4) {
        // A busy frame (many animation rects pending) would need them all
        // re-marked at shifted positions; with the merge slack that
        // degenerates into full-screen uploads WORSE than the classic
        // path. Fall back to the classic full refresh for this frame -
        // never worse than the old behavior, and calm frames (the vast
        // majority of a pan) keep the ring win.
        return false;
    }

    gIsoShiftsSincePresent++;

    // Anything drawn into the iso buffer since the last present was marked
    // at pre-shift coordinates; its ring cells now map one shift away and
    // would ghost (the hex-cursor erase and the walking animation frame
    // land here every pan frame). Re-mark those rects at their shifted
    // positions as well.
    SDL_Rect pending[kMaxDirtyRects];
    int pendingCount = gDirtyRectCount;
    memcpy(pending, gDirtyRects, sizeof(SDL_Rect) * pendingCount);
    for (int i = 0; i < pendingCount; i++) {
        SDL_Rect moved = pending[i];
        moved.x -= screenDx;
        moved.y -= screenDy;
        renderMarkDirtyAmbient(&moved);
    }

    // The world-space list needs the same treatment, or ring cells around
    // fresh world updates (the dude and the hex cursor - the screen
    // center) display one shift behind.
    SDL_Rect worldPending[kMaxDirtyRects];
    int worldPendingCount = gWorldDirtyRectCount;
    memcpy(worldPending, gWorldDirtyRects, sizeof(SDL_Rect) * worldPendingCount);
    for (int i = 0; i < worldPendingCount; i++) {
        Rect moved;
        moved.left = worldPending[i].x - screenDx;
        moved.top = worldPending[i].y - screenDy;
        moved.right = moved.left + worldPending[i].w - 1;
        moved.bottom = moved.top + worldPending[i].h - 1;
        renderMarkWorldDirty(&moved);
    }

    // Transparent windows above the iso window (touch overlay buttons) get
    // recomposited by the suppressed refresh with their marks dropped -
    // mark their rects explicitly.
    {
        Rect above[50];
        int aboveCount = windowGetVisibleRectsAbove(gIsoWindow, above, 50);
        SDL_Rect isoArea = { 0, 0, gSdlTextureSurface->w, gSdlTextureSurface->h };
        for (int i = 0; i < aboveCount; i++) {
            SDL_Rect wr;
            wr.x = above[i].left;
            wr.y = above[i].top;
            wr.w = above[i].right - above[i].left + 1;
            wr.h = above[i].bottom - above[i].top + 1;
            // Only the part overlapping the iso view can be touched by the
            // suppressed recomposite - marking the interface bar strips
            // whole merged everything into full-screen rects.
            SDL_Rect clippedAbove;
            if (SDL_IntersectRect(&wr, &isoArea, &clippedAbove) == SDL_TRUE) {
                renderMarkDirtyAmbient(&clippedAbove);
            }
        }
    }

    // The cached colour-cycle cell boxes travel with the content; without
    // this, fires freeze mid-pan until the next presence rescan.
    for (int i = 0; i < gCycleGridCols * gCycleGridRows; i++) {
        CycleCell& cell = gCycleCells[i];
        if (cell.maxX >= 0) {
            cell.minX = static_cast<short>(cell.minX - screenDx);
            cell.maxX = static_cast<short>(cell.maxX - screenDx);
            cell.minY = static_cast<short>(cell.minY - screenDy);
            cell.maxY = static_cast<short>(cell.maxY - screenDy);
        }
    }

    gIsoRingOx = isoRingWrap(gIsoRingOx + screenDx, gIsoRingW);
    gIsoRingOy = isoRingWrap(gIsoRingOy + screenDy, gIsoRingH);
    return true;
}

double renderIsoGetZoom()
{
    return gIsoZoom;
}

void renderIsoSetZoom(double zoom)
{
    // EDG maps (authored edge alignment) keep the classic 1x view.
    if (mapEdgeIsEnabled()) {
        zoom = 1.0;
    }
    // Pure clamp - snapping mid-gesture popped the scale every time the
    // value crossed the snap threshold. The gesture release settles it.
    if (zoom < 1.0) {
        zoom = 1.0;
    }
    if (zoom > 1.5) {
        zoom = 1.5;
    }
    if (zoom != gIsoZoom) {
        gIsoZoom = zoom;
        gIsoZoomDirty = true;
        sharedFpsLimiter.notifyActivity();
    }
}

// The visible crop of the oversized world window, in world (window-local)
// coordinates. At 1x it is the centered screen-sized rect; zooming out
// grows it up to the full world window.
void renderIsoZoomCrop(int* cropX, int* cropY, int* cropW, int* cropH)
{
    // The iso window's height is NOT the surface height when the interface
    // bar sits below the map view - mixing the two skews the vertical scale
    // against the horizontal one (visible aspect distortion while zooming).
    int marginX;
    int marginY;
    mapGetIsoMargins(&marginX, &marginY);
    int worldW = 0;
    int worldH = 0;
    Window* isoWin = gIsoWindow != -1 ? windowGetWindow(gIsoWindow) : nullptr;
    if (isoWin != nullptr) {
        worldW = isoWin->width;
        worldH = isoWin->height;
    } else if (gSdlTextureSurface != nullptr) {
        worldW = gSdlTextureSurface->w + 2 * marginX;
        worldH = gSdlTextureSurface->h + 2 * marginY;
    }
    int screenW = worldW - 2 * marginX;
    int screenH = worldH - 2 * marginY;

    double zoom = gIsoZoom;
    if (zoom > 1.001 && !isoGpuModeActive()) {
        // The classic path composes the 1x center crop - inputs must match.
        zoom = 1.0;
    }

    int w = static_cast<int>(screenW * zoom + 0.5);
    int h = static_cast<int>(screenH * zoom + 0.5);
    if (w > worldW) w = worldW;
    if (h > worldH) h = worldH;
    *cropW = w;
    *cropH = h;
    *cropX = (worldW - w) / 2;
    *cropY = (worldH - h) / 2;
}

// Screen point -> world point through the current zoom crop.
void renderIsoScreenToWorld(int* x, int* y)
{
    int cropX;
    int cropY;
    int cropW;
    int cropH;
    renderIsoZoomCrop(&cropX, &cropY, &cropW, &cropH);
    int marginX;
    int marginY;
    mapGetIsoMargins(&marginX, &marginY);
    int screenW = 1;
    int screenH = 1;
    Window* isoWin = gIsoWindow != -1 ? windowGetWindow(gIsoWindow) : nullptr;
    if (isoWin != nullptr) {
        screenW = isoWin->width - 2 * marginX;
        screenH = isoWin->height - 2 * marginY;
    }
    if (screenW < 1) screenW = 1;
    if (screenH < 1) screenH = 1;
    *x = cropX + static_cast<int>(static_cast<long long>(*x) * cropW / screenW);
    *y = cropY + static_cast<int>(static_cast<long long>(*y) * cropH / screenH);
}

// Draw the current zoom crop of the world ring scaled to the iso view
// area (the pinch zoom-out), as up to 2x2 wrapped ring pieces. The dst
// height is the ISO view height, not the surface height - the interface
// bar area below is painted by its own window.
static void isoRingComposeZoomed(int screenW, int screenH)
{
    SDL_SetTextureScaleMode(gIsoRingTexture, SDL_ScaleModeLinear);

    int cropX;
    int cropY;
    int cropW;
    int cropH;
    renderIsoZoomCrop(&cropX, &cropY, &cropW, &cropH);
    int marginX;
    int marginY;
    mapGetIsoMargins(&marginX, &marginY);
    screenW = gIsoRingW - 2 * marginX;
    screenH = gIsoRingH - 2 * marginY;
    double scaleX = static_cast<double>(screenW) / cropW;
    double scaleY = static_cast<double>(screenH) / cropH;

    int startTx = isoRingWrap(cropX + gIsoRingOx, gIsoRingW);
    int firstW = gIsoRingW - startTx < cropW ? gIsoRingW - startTx : cropW;
    int spansX[2][3] = { { startTx, 0, firstW }, { 0, firstW, cropW - firstW } };
    int startTy = isoRingWrap(cropY + gIsoRingOy, gIsoRingH);
    int firstH = gIsoRingH - startTy < cropH ? gIsoRingH - startTy : cropH;
    int spansY[2][3] = { { startTy, 0, firstH }, { 0, firstH, cropH - firstH } };

    for (int iy = 0; iy < 2; iy++) {
        if (spansY[iy][2] <= 0) {
            continue;
        }
        for (int ix = 0; ix < 2; ix++) {
            if (spansX[ix][2] <= 0) {
                continue;
            }
            SDL_Rect src;
            src.x = spansX[ix][0];
            src.y = spansY[iy][0];
            src.w = spansX[ix][2];
            src.h = spansY[iy][2];
            SDL_FRect dst;
            dst.x = static_cast<float>(spansX[ix][1] * scaleX);
            dst.y = static_cast<float>(spansY[iy][1] * scaleY);
            dst.w = static_cast<float>(src.w * scaleX);
            dst.h = static_cast<float>(src.h * scaleY);
            SDL_RenderCopyF(gSdlRenderer, gIsoRingTexture, &src, &dst);
        }
    }
}

void renderMarkDirty(const SDL_Rect* rect)
{
    sharedFpsLimiter.notifyActivity();
    renderMarkDirtyAmbient(rect);
}

// Same as renderMarkDirty, but does not register user activity - for ambient
// animation (palette cycling) that should not keep the idle limiter awake.
static bool gMarkSuppressed = false;

void renderSetMarkSuppressed(bool suppressed)
{
    gMarkSuppressed = suppressed;
}

void renderMarkDirtyAmbient(const SDL_Rect* rect)
{
    if (gMarkSuppressed) {
        return;
    }
    if (gSdlTextureSurface == nullptr) {
        return;
    }

    SDL_Rect full;
    full.x = 0;
    full.y = 0;
    full.w = gSdlTextureSurface->w;
    full.h = gSdlTextureSurface->h;

    // Mirror the UNCLIPPED rect into the world-space ring list first: the
    // screen clip below drops the part beyond the screen edge, which in
    // world terms is the band just outside the 1x crop - without the
    // mirror that band goes stale in the ring and shows as a thin seam
    // trailing the motion at zoom-out.
    {
        int marginX;
        int marginY;
        mapGetIsoMargins(&marginX, &marginY);
        Rect world;
        if (rect == nullptr) {
            world.left = 0;
            world.top = 0;
            world.right = full.w + 2 * marginX - 1;
            world.bottom = full.h + 2 * marginY - 1;
        } else {
            world.left = rect->x + marginX;
            world.top = rect->y + marginY;
            world.right = rect->x + rect->w - 1 + marginX;
            world.bottom = rect->y + rect->h - 1 + marginY;
        }
        renderMarkWorldDirty(&world);
    }

    SDL_Rect clipped;
    if (rect == nullptr) {
        clipped = full;
    } else if (SDL_IntersectRect(rect, &full, &clipped) == SDL_FALSE) {
        return;
    }

    // Merge into any rect that overlaps or lies within the slack distance;
    // repeat, since a merge can bridge previously separate rects.
    bool merged = true;
    while (merged) {
        merged = false;
        for (int i = 0; i < gDirtyRectCount; i++) {
            SDL_Rect inflated = gDirtyRects[i];
            inflated.x -= kDirtyMergeSlackPx;
            inflated.y -= kDirtyMergeSlackPx;
            inflated.w += 2 * kDirtyMergeSlackPx;
            inflated.h += 2 * kDirtyMergeSlackPx;

            if (SDL_HasIntersection(&inflated, &clipped) == SDL_TRUE) {
                SDL_UnionRect(&gDirtyRects[i], &clipped, &clipped);
                gDirtyRects[i] = gDirtyRects[gDirtyRectCount - 1];
                gDirtyRectCount--;
                merged = true;
                break;
            }
        }
    }

    if (gDirtyRectCount < kMaxDirtyRects) {
        gDirtyRects[gDirtyRectCount++] = clipped;
    } else {
        // Overflow: fold into the first rect.
        SDL_UnionRect(&gDirtyRects[0], &clipped, &gDirtyRects[0]);
    }
}

void renderPresent()
{
    if (gSdlRenderer == nullptr || gSdlTexture == nullptr || gSdlTextureSurface == nullptr) {
        return;
    }

    // Nothing changed since the last present - do not burn a texture upload,
    // a draw call and a swap on an identical frame. A zoom change re-composes
    // even with no dirty rects.
    if (gDirtyRectCount == 0 && !gIsoZoomDirty) {
        return;
    }

    // Palette fades and window drags present without going through
    // inputGetInput, so they would keep touching the GPU while the app is
    // backgrounded (iOS terminates for that). Keep the marks - the first
    // present after resume repaints everything that changed.
    if (!gProgramIsActive) {
        return;
    }

    // GPU iso mode: entering, leaving or (re)creating the ring needs one
    // full refresh so the newly authoritative texture holds everything.
    bool isoMode = isoGpuModeActive();
    if (isoMode) {
        Window* isoWin = windowGetWindow(gIsoWindow);
        if (!isoRingEnsure(isoWin->width, isoWin->height)) {
            isoMode = false;
        }
    }
    if (isoMode != gIsoModeLastPresent || (isoMode && !gIsoRingContentValid)) {
        renderMarkDirtyAmbient(nullptr);
    }
    gIsoModeLastPresent = isoMode;

    // Convert and upload every dirty rect separately - each exactly once per
    // presented frame, with the palette as it stands now.
    bool uploadFailed = false;
    bool ringSawFullRect = false;
    constexpr int kMaxAboveRects = 50;
    Rect aboveRects[kMaxAboveRects];
    int aboveRectCount = 0;
    bool cursorVisible = false;
    Rect cursorRect = { 0, 0, -1, -1 };
    if (isoMode) {
        isoRingRebuildLut();
        aboveRectCount = windowGetVisibleRectsAbove(gIsoWindow, aboveRects, kMaxAboveRects);
        if (aboveRectCount < 0) {
            aboveRectCount = 0;
        }
        cursorVisible = !cursorIsHidden();
        if (cursorVisible) {
            mouseGetRect(&cursorRect);
        }
    }
    for (int i = 0; i < gDirtyRectCount; i++) {
        const SDL_Rect& r = gDirtyRects[i];

        if (gSdlSurface != nullptr) {
            SDL_Rect src = r;
            SDL_Rect dst = r;
            SDL_BlitSurface(gSdlSurface, &src, gSdlTextureSurface, &dst);
        }

        bool classicTextureNeeded = true;
        if (isoMode && gIsoRingContentValid && r.y + r.h <= gIsoRingH) {
            // The composed frame samples the classic texture only at the
            // windows above the iso window, the cursor and the debug
            // overlay - a pure-world rect need not reach the GPU twice.
            classicTextureNeeded = false;

            SDL_Rect probe;
            for (int a = 0; a < aboveRectCount && !classicTextureNeeded; a++) {
                SDL_Rect wr = { aboveRects[a].left, aboveRects[a].top,
                    aboveRects[a].right - aboveRects[a].left + 1, aboveRects[a].bottom - aboveRects[a].top + 1 };
                classicTextureNeeded = SDL_IntersectRect(&r, &wr, &probe) == SDL_TRUE;
            }
            if (!classicTextureNeeded && cursorVisible) {
                SDL_Rect cr = { cursorRect.left, cursorRect.top,
                    cursorRect.right - cursorRect.left + 1, cursorRect.bottom - cursorRect.top + 1 };
                classicTextureNeeded = SDL_IntersectRect(&r, &cr, &probe) == SDL_TRUE;
            }
        }

        const unsigned char* pixels = static_cast<const unsigned char*>(gSdlTextureSurface->pixels)
            + static_cast<size_t>(r.y) * gSdlTextureSurface->pitch
            + static_cast<size_t>(r.x) * gSdlTextureSurface->format->BytesPerPixel;

        if (classicTextureNeeded && SDL_UpdateTexture(gSdlTexture, &r, pixels, gSdlTextureSurface->pitch) != 0) {
            uploadFailed = true;
        }

        if (isoMode && !ringSawFullRect
            && r.w >= gSdlTextureSurface->w && r.h >= gSdlTextureSurface->h) {
            // A full-screen repaint validates the whole ring, margins
            // included - the world buffer always holds the full world.
            // Partial ring updates come from the world-space list alone
            // (every ambient mark mirrors into it unclipped).
            SDL_Rect fullRing = { 0, 0, gIsoRingW, gIsoRingH };
            if (!isoRingUpload(fullRing)) {
                uploadFailed = true;
            }
            ringSawFullRect = true;
        }

        gStatUploadBytes += static_cast<long long>(r.w) * r.h * gSdlTextureSurface->format->BytesPerPixel;
        gStatRects++;
        if (static_cast<long long>(r.w) * r.h > static_cast<long long>(gStatMaxRectW) * gStatMaxRectH) {
            gStatMaxRectW = r.w;
            gStatMaxRectH = r.h;
        }
    }
    // World-space dirt (margins included) reaches the ring directly.
    if (isoMode) {
        SDL_Rect ringArea = { 0, 0, gIsoRingW, gIsoRingH };
        for (int i = 0; i < gWorldDirtyRectCount; i++) {
            if (ringSawFullRect) {
                break;
            }
            SDL_Rect part;
            if (SDL_IntersectRect(&gWorldDirtyRects[i], &ringArea, &part) == SDL_TRUE) {
                if (!isoRingUpload(part)) {
                    uploadFailed = true;
                }
            }
        }
    }
    if (!uploadFailed) {
        gWorldDirtyRectCount = 0;
    }

    if (isoMode && ringSawFullRect && !uploadFailed) {
        gIsoRingContentValid = true;
    }

    sharedFpsLimiter.notifyPresent();
    gStatPresents++;
    SDL_RenderClear(gSdlRenderer);
    if (isoMode && gIsoRingContentValid) {
        bool zoomed = gIsoZoom > 1.001;

        if (zoomed) {
            isoRingComposeZoomed(gSdlTextureSurface->w, gSdlTextureSurface->h);
        } else {
            int composeMarginX;
            int composeMarginY;
            mapGetIsoMargins(&composeMarginX, &composeMarginY);
            SDL_SetTextureScaleMode(gIsoRingTexture, SDL_ScaleModeNearest);
            isoRingCompose(gSdlTextureSurface->w, gSdlTextureSurface->h, composeMarginX, composeMarginY);
        }

        // Everything above the iso window comes from the classic texture -
        // its pixels for exactly these rects are always freshly uploaded.
        for (int i = 0; i < aboveRectCount; i++) {
            SDL_Rect wr;
            wr.x = aboveRects[i].left;
            wr.y = aboveRects[i].top;
            wr.w = aboveRects[i].right - aboveRects[i].left + 1;
            wr.h = aboveRects[i].bottom - aboveRects[i].top + 1;
            SDL_Rect full = { 0, 0, gSdlTextureSurface->w, gSdlTextureSurface->h };
            SDL_Rect clipped;
            if (SDL_IntersectRect(&wr, &full, &clipped) == SDL_TRUE) {
                SDL_RenderCopy(gSdlRenderer, gSdlTexture, &clipped, &clipped);
            }
        }

        // The software cursor is composited into the classic texture too.
        // Under zoom the world part of the cursor scales with the world (the
        // classic texture's pixels equal the ring's for the same rect, so
        // the paste is seamless); pieces over UI windows draw 1:1 on top.
        if (cursorVisible) {
            SDL_Rect cr;
            cr.x = cursorRect.left;
            cr.y = cursorRect.top;
            cr.w = cursorRect.right - cursorRect.left + 1;
            cr.h = cursorRect.bottom - cursorRect.top + 1;
            SDL_Rect full = { 0, 0, gSdlTextureSurface->w, gSdlTextureSurface->h };
            SDL_Rect clipped;
            if (SDL_IntersectRect(&cr, &full, &clipped) == SDL_TRUE) {
                if (zoomed) {
                    // The cursor is a SCREEN entity: draw its own art 1:1
                    // at the finger position with real transparency.
                    unsigned char* art;
                    int artW;
                    int artH;
                    unsigned char artKey;
                    mouseGetCursorArt(&art, &artW, &artH, &artKey);
                    if (art != nullptr && artW > 0 && artH > 0) {
                        if (gCursorArtTexture != nullptr
                            && (gCursorArtTextureW != artW || gCursorArtTextureH != artH)) {
                            SDL_DestroyTexture(gCursorArtTexture);
                            gCursorArtTexture = nullptr;
                        }
                        if (gCursorArtTexture == nullptr) {
                            gCursorArtTexture = SDL_CreateTexture(gSdlRenderer, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING, artW, artH);
                            if (gCursorArtTexture != nullptr) {
                                SDL_SetTextureBlendMode(gCursorArtTexture, SDL_BLENDMODE_BLEND);
                                gCursorArtTextureW = artW;
                                gCursorArtTextureH = artH;
                            }
                        }
                        if (gCursorArtTexture != nullptr) {
                            static std::vector<Uint32> cursorScratch;
                            cursorScratch.resize(static_cast<size_t>(artW) * artH);
                            for (int cyi = 0; cyi < artH; cyi++) {
                                for (int cxi = 0; cxi < artW; cxi++) {
                                    unsigned char colorIndex = art[cyi * artW + cxi];
                                    cursorScratch[static_cast<size_t>(cyi) * artW + cxi] =
                                        colorIndex == artKey ? 0u : (gIsoRingLut[colorIndex] | 0xFF000000u);
                                }
                            }
                            SDL_UpdateTexture(gCursorArtTexture, nullptr, cursorScratch.data(), artW * 4);
                            SDL_Rect cursorDst = { cursorRect.left, cursorRect.top, artW, artH };
                            SDL_RenderCopy(gSdlRenderer, gCursorArtTexture, nullptr, &cursorDst);
                        }
                    }
                } else {
                    SDL_RenderCopy(gSdlRenderer, gSdlTexture, &clipped, &clipped);
                }
            }
        }

        // Debug FPS overlay lives in the interface-bar area now, but that
        // bar may be hidden (worldmap) - draw its strip from the classic
        // texture explicitly.
        if (settings.debug.show_fps) {
            int oy = gSdlTextureSurface->h - 16;
            SDL_Rect fr = { 0, oy, gSdlTextureSurface->w < 420 ? gSdlTextureSurface->w : 420, 16 };
            SDL_RenderCopy(gSdlRenderer, gSdlTexture, &fr, &fr);
        }
    } else {
        SDL_RenderCopy(gSdlRenderer, gSdlTexture, nullptr, nullptr);
    }
    // render movie SDL texture if present
    // render movie SDL texture if present
    // render movie SDL texture if present
    movieRenderDirectOverlay();

    SDL_RenderPresent(gSdlRenderer);
    gIsoZoomDirty = false;
    gIsoShiftsSincePresent = 0;

    // A transiently failed texture upload (Metal around app suspension)
    // would otherwise leave stale pixels - ghost cursor images - in the
    // texture forever: the surface is correct, but nothing re-marks the
    // region. Keep the marks so the next present retries them.
    if (!uploadFailed) {
        gDirtyRectCount = 0;
    }
}

// Buffer size and raw pixel data for freezing the loading screen
static int gLoadingScreenBackupSize = 0;
unsigned char* gLoadingScreenPixelsBackup = nullptr;

void initLoadingScreenFreeze()
{
    mouseHideCursor();

    // The GPU pan skips CPU conversions for pure-world rects, so the
    // texture surface can be behind the composite - complete it before
    // snapshotting, and force the classic present while frozen (the
    // freeze repairs gSdlTextureSurface; the ring would ignore it).
    if (gSdlSurface != nullptr && gSdlTextureSurface != nullptr) {
        SDL_BlitSurface(gSdlSurface, nullptr, gSdlTextureSurface, nullptr);
    }
    gLoadingScreenFrozen = true;

    // Calculate the total size of the screen surface buffer
    gLoadingScreenBackupSize = gSdlTextureSurface->pitch * gSdlTextureSurface->h;

    if (gLoadingScreenPixelsBackup != nullptr) {
        free(gLoadingScreenPixelsBackup);
    }

    // Allocate memory and make a deep copy of clean screen pixels
    gLoadingScreenPixelsBackup = (unsigned char*)malloc(gLoadingScreenBackupSize);
    if (gSdlTextureSurface->pixels && gLoadingScreenPixelsBackup) {
        memcpy(gLoadingScreenPixelsBackup, gSdlTextureSurface->pixels, gLoadingScreenBackupSize);
    }
}

void restoreLoadingScreenFreeze()
{
    // Overwrite any intermediate map loading artifacts
    // with the original frozen menu pixels before drawing the cursor
    if (gSdlTextureSurface->pixels && gLoadingScreenPixelsBackup) {
        memcpy(gSdlTextureSurface->pixels, gLoadingScreenPixelsBackup, gLoadingScreenBackupSize);
    }
}

void freeLoadingScreenFreeze()
{
    gLoadingScreenFrozen = false;

    if (gLoadingScreenPixelsBackup != nullptr) {
        free(gLoadingScreenPixelsBackup);
        gLoadingScreenPixelsBackup = nullptr;

        mouseShowCursor();
    }
}

} // namespace fallout
