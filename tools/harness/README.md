# Profiling / benchmark harness (desktop)

Everything here is **development tooling**. Nothing in this directory is
compiled into the game, and `bench.patch` must never be applied in a commit.

## Pieces

| file | role |
|---|---|
| `bench.patch` | All TEMP C++ hooks as one diff (env-gated: `FALLOUT_BENCH`, `FALLOUT_AUTOGAME`, `FALLOUT_ALLMAPS`, `FALLOUT_TELEPORT`, `FALLOUT_PANBENCH`). Apply with `git apply tools/harness/bench.patch` from the repo root; revert with `git apply -R`. |
| `autogame.py` | Injects the "new game" key sequence into `_gsound_bkg_proc` (main menu -> char screen -> spams SPACE 12..34s to skip every cutscene, incl. `elder.mve`). Env: `FALLOUT_AUTOGAME=1`. |
| `allmaps.py` | Bench flag (never freeze unfocused, no 5fps cap, present/audio not gated) + per-present profiling log in `renderPresent` (`[p] m=<map> a=<axis> u=<kB> f=<full?> d=<dirty> s=<shifts>`). |
| `allmaps2.py` | Map-survey driver: walks ~78 real location maps, sweeps the camera edge-to-edge THROUGH the center on 4 axes (X, Y, diag, anti), skips movies, tags frames with the ACTUAL loaded map, worldmap watchdog. Env: `FALLOUT_ALLMAPS=1`. |
| `bench2.py` | Map-entry safety: skips `map_enter` scripts, refuses `gameDialogEnter` and `_combat` under `FALLOUT_BENCH` (cutscene on Arroyo village, dialog on Vault 15 entry otherwise block the survey). |
| `teleport.py` | `FALLOUT_TELEPORT=<mapid>`: teleport once after the new game settles and leave the window running for hand-testing. |
| `panbench.py` | Toggles the in-game PAN button programmatically and logs the view bias (verifies edge-to-edge bounce). |
| `analyze_maps.py` / `compare_maps.py` | Per-map aggregation; before/after comparison of two runs. |
| `ppm2png.py`, `f2scan.py` | Frame-dump conversion; DAT2/maps.txt reader used for map names. |

The Python patchers are **not idempotent**: each edits the working tree
once. Sequence for a survey run, from a clean tree:

```
git apply tools/harness/bench.patch        # or: autogame.py, allmaps.py, allmaps2.py, bench2.py
<build>
cd <game dir>
SDL_VIDEO_WINDOW_POS=3000,3000 FALLOUT_BENCH=1 FALLOUT_AUTOGAME=1 FALLOUT_ALLMAPS=1 ./fallout2-ce.exe 2> allmaps.txt
python tools/harness/analyze_maps.py
git apply -R tools/harness/bench.patch     # ALWAYS before committing
```

`FALLOUT_GAME_DIR` points the scripts at the game directory (default
`<repo>/../game`).

## Bench game config (in the game dir, not shipped)

```
windowed=1            # so SDL_VIDEO_WINDOW_POS can park the window off-screen
edge_scroll=0         # the parked OS cursor otherwise drives edge scroll
follow_hero=0
master_volume=0 music_volume=0 sndfx_volume=0 speech_volume=0
```

## Main-loop phase profiler

`bench_prof.patch` is `bench.patch` PLUS the phase-profiler hooks (apply it
INSTEAD of `bench.patch`, on a clean tree, after copying `prof_tmp.h` into
`src/`):

```
cp tools/harness/prof_tmp.h src/
git apply tools/harness/bench_prof.patch
<build>
python tools/harness/cfgmode.py bench          # bench cfg (edge scroll/follow/volumes off)
cd <game dir>
SDL_VIDEO_WINDOW_POS=3000,3000 FALLOUT_BENCH=1 FALLOUT_AUTOGAME=1 FALLOUT_PHASEBENCH=42 ./fallout2-ce.exe 2> phase42.txt
python tools/harness/phases.py phase42.txt     # STATIC vs PAN window averages
python tools/harness/cfgmode.py play
```

`FALLOUT_PHASEBENCH=<mapid>` teleports at 34s, sits still until 54s,
auto-pans (PAN button) until 74s and exits. Every second a `[t]` line
carries ms/s per phase: tickers (`anim`, `scr`, `gm`, `cyc`, `snd`), scene
re-render (`tileref`, `render` = scroll strips), window composite (`wref`),
`present` split into blit/classic upload/ring upload/compose/flip, `sleep`,
plus counters (on/off-screen animation ticks, refresh rects in/out of the
crop, pathfinder calls). At exit: `[cs]` = the most expensive critter scripts
(index into scripts.lst), `[op]` = the most expensive script opcodes inside
critter procs, `[tk]` = per-ticker time and refresh counts over the static
window, `[gm]` = gameMouseRefresh branch counters. Set `show_fps=0` in the
bench cfg or the overlay redraw counts as a present every frame.

## Pathfinder differential test, combat bench, animation-step profile

`bench_prof2.patch` is a superset of `bench_prof.patch` (apply one or the
other, never both; same `prof_tmp.h` copy step). It adds:

- **Pathfinder differential wrapper**: `pathfinderFindPath` runs the
  classic flat-array search (verbatim copy) and the current one on EVERY
  call and compares path length, rotations and `_moveBlockObj`; any
  difference prints `[pf] MISMATCH ...`. `FALLOUT_PATHSTRESS=1` additionally
  pushes ~90 random from/to pairs (near, mid, far; both blocking callbacks;
  with and without `requireEmptyDest`) through the wrapper once per loaded
  map, so a full `FALLOUT_ALLMAPS=1` survey covers all 77 maps:
  `... FALLOUT_ALLMAPS=1 FALLOUT_PATHSTRESS=1 ./fallout2-ce.exe 2> pdiff_all.txt; grep -c MISMATCH pdiff_all.txt`
  (`[pf] stress map=.. | total calls=.. mismatch=.. classic_ms=.. new_ms=..`).
  Use it whenever the pathfinder is touched.
- **Combat bench**: `FALLOUT_COMBATBENCH=<mapid> FALLOUT_ALLOWCOMBAT=1`
  teleports, then starts combat the way scripts do (`scriptsRequestCombat`
  with the nearest living non-party critter as attacker) and ends the
  player's turns with SPACE; `[t]` lines gain `ai=` (wall ms of
  `_combat_ai`), `sp=`/`spcalls=` (`_make_straight_path_func`), `pf=`
  (pathfinder ms), `combat=`. Map 11 (Klamath rat caves) works.
- **Animation-step profile** (dumped at the combat bench exit): `[as]`
  self time per `animationRunSequence` step kind and per `_object_animate`
  part, `[acb]` per callback pointer, plus `_anim_set_end` /
  `_combat_anim_finished` sub-timers. Finding to remember: the HP digit
  roll on the interface bar (`interfaceRenderHitPoints(true)`) blocks inside
  `inputPauseForTocks` for ~150 ms per hit - wall-clock scopes that wrap it
  (the animation ticker) look huge but it sleeps and presents, it does not
  spin.

## Hard-won facts

- Unfocused, the game **freezes**: `inputGetInput` spins in
  `_GNW95_lost_focus`. The root bench fix is ignoring deactivation in
  `inputHandleProgramActivationChange`; the intro MVE is audio-synced, so a
  partial bench (present only) hangs it black.
- Do not minimize the window (`SW_MINIMIZE` kills the D3D device, exit 127);
  park it off-screen instead.
- `mapHandleTransition` treats `map == 0` as "no transition".
- Tile 20100 (grid center) and several maps' default start are **exit
  grids** -> instant world map. The bench suppresses exit-grid transitions in
  `_obj_move_to` instead of guessing tiles.
- Encounter maps (`desert*`, `cave*`, `mountn*`, `coast*`, `city*`, `rnd*`,
  `enc*`) are excluded; the location id list lives in `allmaps2.py`.
- The driver steps ~0.5 tile/frame, so quiet axes present ~31/s; compare
  axes by ratio, not absolute presents.
