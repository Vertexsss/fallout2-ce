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
static bool gRenderDirty = false;
static SDL_Rect gRenderDirtyRect;

// A palette write happened; the full re-conversion of the indexed surface is
// deferred to the next present, so several palette ticks between presents
// cost one blit instead of several.
static bool gPaletteBlitPending = false;

// Which palette indices currently appear on the game surface. Lets ambient
// palette animation (color cycling) be skipped entirely when the cycled
// colors are not on screen - the "very fast" group ticks 30 times per second
// on every map whether or not its color is visible.
static bool gPaletteIndexPresent[256];
static bool gPalettePresenceValid = false;
static unsigned int gPalettePresenceScanTicks = 0;

static void paletteRebuildPresence()
{
    memset(gPaletteIndexPresent, 0, sizeof(gPaletteIndexPresent));

    const unsigned char* row = static_cast<const unsigned char*>(gSdlSurface->pixels);
    for (int y = 0; y < gSdlSurface->h; y++) {
        for (int x = 0; x < gSdlSurface->w; x++) {
            gPaletteIndexPresent[row[x]] = true;
        }
        row += gSdlSurface->pitch;
    }

    gPalettePresenceValid = true;
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
        if (visibleChange) {
            gPaletteBlitPending = true;
            renderMarkDirtyAmbient(nullptr);
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
        gPaletteBlitPending = true;
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
    SDL_BlitSurface(gSdlSurface, &srcRect, gSdlTextureSurface, &destRect);

    renderMarkDirty(&srcRect);
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

    SDL_BlitSurface(gSdlSurface, nullptr, gSdlTextureSurface, nullptr);
    renderMarkDirty(nullptr);
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
    // The recorded dirty region is only meaningful for the surface it was
    // recorded against - forget it before that surface goes away.
    gRenderDirty = false;
    gPaletteBlitPending = false;

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

    // The new surface starts out blank - re-convert the game surface into it
    // on the next present and upload it in full.
    gPaletteBlitPending = true;
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

    unsigned int elapsed = now - sampleStartTicks;
    if (elapsed >= 500) {
        fps = sampleFrames * 1000.0 / elapsed;
        sampleFrames = 0;
        sampleStartTicks = now;
    }

    char text[32];
    snprintf(text, sizeof(text), "FPS: %.1f", fps);

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
    SDL_BlitSurface(gSdlSurface, &rect, gSdlTextureSurface, &rect);
    renderMarkDirty(&rect);
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

    if (gRenderDirty) {
        SDL_UnionRect(&gRenderDirtyRect, &clipped, &gRenderDirtyRect);
    } else {
        gRenderDirtyRect = clipped;
        gRenderDirty = true;
    }
}

void renderPresent()
{
    if (gSdlRenderer == nullptr || gSdlTexture == nullptr || gSdlTextureSurface == nullptr) {
        return;
    }

    // Nothing changed since the last present - do not burn a texture upload,
    // a draw call and a swap on an identical frame.
    if (!gRenderDirty) {
        return;
    }

    // A palette change re-colors every pixel - re-convert the indexed surface
    // once, now that we know this frame is actually going to be presented.
    if (gPaletteBlitPending) {
        gPaletteBlitPending = false;
        if (gSdlSurface != nullptr) {
            SDL_BlitSurface(gSdlSurface, nullptr, gSdlTextureSurface, nullptr);
        }
    }

    const unsigned char* pixels = static_cast<const unsigned char*>(gSdlTextureSurface->pixels)
        + static_cast<size_t>(gRenderDirtyRect.y) * gSdlTextureSurface->pitch
        + static_cast<size_t>(gRenderDirtyRect.x) * gSdlTextureSurface->format->BytesPerPixel;

    SDL_UpdateTexture(gSdlTexture, &gRenderDirtyRect, pixels, gSdlTextureSurface->pitch);
    SDL_RenderClear(gSdlRenderer);
    SDL_RenderCopy(gSdlRenderer, gSdlTexture, nullptr, nullptr);
    // render movie SDL texture if present
    movieRenderDirectOverlay();
    SDL_RenderPresent(gSdlRenderer);

    gRenderDirty = false;
}

} // namespace fallout
