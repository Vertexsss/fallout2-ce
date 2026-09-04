// TEMP PROFILING (not committed): main-loop phase timer.
#ifndef FALLOUT_PROF_TMP_H_
#define FALLOUT_PROF_TMP_H_

#include <SDL.h>

namespace fallout {

enum ProfPhase {
    PROF_PBK = 0, // whole _process_bk (tickers + mouse + buttons + kb)
    PROF_TICKERS, // tickersExecute total
    PROF_ANIM, // _object_animate ticker
    PROF_SCRIPT, // _doBkProcesses ticker
    PROF_GMOUSE, // gameMouseRefresh ticker
    PROF_CYCLE, // colorCycleTicker
    PROF_SOUND, // _gsound_bkg_proc
    PROF_RENDER, // isoWindowRefreshRectGame (scroll strip rendering)
    PROF_WREF, // _GNW_win_refresh (window composite blits into the screen surface)
    PROF_PRESENT, // renderPresent (convert + upload + compose + swap)
    PROF_SLEEP, // SDL_Delay inside the fps limiter
    PROF_TILEREF, // tileRefreshGame (regular scene re-render of a rect, all callers)
    PROF_GM_PATH, // _make_path (hover test) inside gameMouseRefresh
    PROF_SCR_UPD, // _updatePrograms total
    PROF_SCR_SFALL, // sfall_gl_scr_update inside _updatePrograms
    PROF_SCR_EVT, // doEvents + intLibUpdate inside _updatePrograms
    PROF_SCR_CRIT, // _script_chk_critters
    PROF_SCR_TIMED, // _script_chk_timed_events
    PROF_PR_BLIT, // renderPresent: SDL_BlitSurface 8bit->ARGB per dirty rect
    PROF_PR_UPC, // renderPresent: SDL_UpdateTexture classic texture
    PROF_PR_RING, // renderPresent: isoRingUpload (LUT convert + upload)
    PROF_PR_COMPOSE, // renderPresent: RenderClear .. draw calls
    PROF_PR_FLIP, // renderPresent: SDL_RenderPresent
    PROF_AI, // _combat_ai wall time (includes animation waits)
    PROF_SP, // _make_straight_path_func
    PROF_COUNT
};

inline double gProfMs[PROF_COUNT];
inline long long gProfLoops;
inline long long gProfAnimOn;
inline long long gProfAnimOff;
inline long long gProfRrIn;
inline long long gProfRrOut;
inline long long gProfTileRefCalls;
inline long long gProfTileRefArea;
inline long long gProfPathCalls;
inline long long gProfProgCount;
inline long long gProfCritCalls;
inline double gProfCritMaxMs;
inline int gProfCritMaxIndex;
inline double gProfCritByIdxMs[4096];
inline long long gProfCritByIdxCalls[4096];
inline int gProfCritScriptCount;
inline bool gProfInCrit;
inline int gProfTickIdx = -1;
inline void* gProfTickProc[32];
inline double gProfTickMs[32];
inline long long gProfTickRr[32];
inline long long gProfGmMemo;
inline long long gProfGmShowCalls;
inline long long gProfGmHover;
inline double gProfOpMs[1024];
inline long long gProfOpCalls[1024];
inline double gProfCafPart[8]; // _combat_anim_finished parts: display, apply_damage, scr_end_combat, whole
inline double gProfSetEndPart[8]; // _anim_set_end parts
inline long long gProfSpCalls;
inline long long gProfAiTurns;
inline long long gProfPresents;
inline long long gProfPrRects;
inline long long gProfPrWorldRects;

inline double profNow()
{
    static double freq = 0.0;
    if (freq == 0.0) {
        freq = static_cast<double>(SDL_GetPerformanceFrequency()) / 1000.0;
    }
    return static_cast<double>(SDL_GetPerformanceCounter()) / freq;
}

struct ProfScope {
    int idx;
    double t0;
    ProfScope(int i)
        : idx(i)
        , t0(profNow())
    {
    }
    ~ProfScope()
    {
        gProfMs[idx] += profNow() - t0;
    }
};

} // namespace fallout

#endif
