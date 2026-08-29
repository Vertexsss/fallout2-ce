#include "svga.h"

#include <limits.h>
#include <stdio.h>
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
#include "memory.h"
#include "mouse.h"
#include "movie.h"
#include "scan_unimplemented.h"
#include "settings.h"
#include "text_font.h"
#include "tile.h"
#include "win32.h"
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

static void paletteRebuildPresence()
{
    memset(gPaletteIndexPresent, 0, sizeof(gPaletteIndexPresent));

    gCycleGridCols = (gSdlSurface->w + kCycleCellSize - 1) / kCycleCellSize;
    gCycleGridRows = (gSdlSurface->h + kCycleCellSize - 1) / kCycleCellSize;
    if (gCycleGridCols > kCycleGridMaxCols) gCycleGridCols = kCycleGridMaxCols;
    if (gCycleGridRows > kCycleGridMaxRows) gCycleGridRows = kCycleGridMaxRows;

    for (int i = 0; i < gCycleGridCols * gCycleGridRows; i++) {
        gCycleCells[i] = { 0x7FFF, 0x7FFF, -1, -1 };
    }
    gCycleAnyPresent = false;

    const int cellW = (gSdlSurface->w + gCycleGridCols - 1) / gCycleGridCols;
    const int cellH = (gSdlSurface->h + gCycleGridRows - 1) / gCycleGridRows;

    const unsigned char* row = static_cast<const unsigned char*>(gSdlSurface->pixels);
    for (int y = 0; y < gSdlSurface->h; y++) {
        for (int x = 0; x < gSdlSurface->w; x++) {
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
        row += gSdlSurface->pitch;
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
                for (int i = 0; i < gCycleGridCols * gCycleGridRows; i++) {
                    const CycleCell& cell = gCycleCells[i];
                    if (cell.maxX >= 0) {
                        SDL_Rect r = { cell.minX, cell.minY,
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

static void destroyRenderer()
{
    // The recorded dirty regions are only meaningful for the surface they
    // were recorded against - forget them before that surface goes away.
    gDirtyRectCount = 0;

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

    bufferFill(static_cast<unsigned char*>(gSdlSurface->pixels), width, height, gSdlSurface->pitch, COLOR_BLACK);
    if (width > kPadding * 2 && height > kPadding * 2) {
        fontDrawText(static_cast<unsigned char*>(gSdlSurface->pixels) + gSdlSurface->pitch * kPadding + kPadding, text, width - kPadding * 2, gSdlSurface->pitch, COLOR_LIGHT_GREY);
    }

    SDL_Rect rect;
    rect.x = 0;
    rect.y = 0;
    rect.w = width;
    rect.h = height;
    // Ambient: the counter itself must not keep the idle limiter awake,
    // otherwise it distorts the very numbers it displays.
    renderMarkDirtyAmbient(&rect);
}

void renderMarkDirty(const SDL_Rect* rect)
{
    sharedFpsLimiter.notifyActivity();
    renderMarkDirtyAmbient(rect);
}

// Same as renderMarkDirty, but does not register user activity - for ambient
// animation (palette cycling) that should not keep the idle limiter awake.
void renderMarkDirtyAmbient(const SDL_Rect* rect)
{
    if (gSdlTextureSurface == nullptr) {
        return;
    }

    SDL_Rect full;
    full.x = 0;
    full.y = 0;
    full.w = gSdlTextureSurface->w;
    full.h = gSdlTextureSurface->h;

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
    // a draw call and a swap on an identical frame.
    if (gDirtyRectCount == 0) {
        return;
    }

    // Convert and upload every dirty rect separately - each exactly once per
    // presented frame, with the palette as it stands now.
    bool uploadFailed = false;
    for (int i = 0; i < gDirtyRectCount; i++) {
        const SDL_Rect& r = gDirtyRects[i];

        if (gSdlSurface != nullptr) {
            SDL_Rect src = r;
            SDL_Rect dst = r;
            SDL_BlitSurface(gSdlSurface, &src, gSdlTextureSurface, &dst);
        }

        const unsigned char* pixels = static_cast<const unsigned char*>(gSdlTextureSurface->pixels)
            + static_cast<size_t>(r.y) * gSdlTextureSurface->pitch
            + static_cast<size_t>(r.x) * gSdlTextureSurface->format->BytesPerPixel;

        if (SDL_UpdateTexture(gSdlTexture, &r, pixels, gSdlTextureSurface->pitch) != 0) {
            uploadFailed = true;
        }

        gStatUploadBytes += static_cast<long long>(r.w) * r.h * gSdlTextureSurface->format->BytesPerPixel;
        gStatRects++;
        if (static_cast<long long>(r.w) * r.h > static_cast<long long>(gStatMaxRectW) * gStatMaxRectH) {
            gStatMaxRectW = r.w;
            gStatMaxRectH = r.h;
        }
    }

    sharedFpsLimiter.notifyPresent();
    gStatPresents++;
    SDL_RenderClear(gSdlRenderer);
    SDL_RenderCopy(gSdlRenderer, gSdlTexture, nullptr, nullptr);
    // render movie SDL texture if present
    movieRenderDirectOverlay();
    SDL_RenderPresent(gSdlRenderer);

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
    if (gLoadingScreenPixelsBackup != nullptr) {
        free(gLoadingScreenPixelsBackup);
        gLoadingScreenPixelsBackup = nullptr;

        mouseShowCursor();
    }
}

} // namespace fallout
