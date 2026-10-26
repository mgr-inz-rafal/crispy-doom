# Karp - Stage 1 Plan: "Go To Location"

> Status: approved; M0-M6 implemented, plus doors and lifts, GO TO EXIT, the goal stack and
> teleporters; now playing through Doom II map by map (section 4).
> All Karp code lives under `karp/`; engine edits are single-line hooks tagged `// [Karp]`.

## 1. Context

Karp is an autonomous single-player Doom bot built into this crispy-doom fork. The long-term bot
navigates a live world (moving monsters, doors, lifts), solves the simple key/switch puzzles, and
picks what to do from a priority list (e.g. "hurt -> go get a medkit"). The bot always has full
knowledge of the map (no blind exploration).

Stage 1 builds the skeleton every later stage stands on:

1. New param `-autoplay` (used with `-warp`) turns the bot on.
2. New param `-karpconsole` opens a separate **Karp Console** window.
3. The console decodes the loaded map and draws it top-down (automap style).
4. Clicking a spot in the console sets the bot priority to **GO TO LOCATION**.
5. The bot plans a path over the map's floor (convex cells derived from subsectors) and drives
   the player there.

Stage 1 assumptions: no monsters (`-nomonsters`), no doors/lifts/switches on the route, no
jumps over gaps (an M6 follow-up). Static geometry only. The first version planned no player-width handling at all;
the M6 drive test showed it is needed (gaps narrower than the player, solid things, narrow
ledges), so it comes in the M6 follow-ups (section 4).

Decisions taken with the user:
- Console UI: **Dear ImGui** in a second SDL2 window, same process. ImGui is vendored, because the
  vcpkg `imgui` port has dropped its SDL2 backends.
- Map data: read from the **engine's loaded level** after `P_SetupLevel`, so every node format
  crispy supports comes for free. Karp builds its own derived data on top.
- Under `-autoplay` the **bot always owns movement**: human movement, turning and fire input is
  discarded while in a level. Menu, pause, save and quit keys keep working.
- **Karp never changes crispy's configuration**, not even temporarily. Fullscreen or windowed is
  the user's choice (Alt+Enter toggles it as usual). If a fullscreen game covers the console, the
  user can switch to windowed mode or move the console to another display.
- The console is **opt-in** via `-karpconsole`. Without it no second window is created, and Karp
  logs to stdout and `karp.log` only.
- The console **remembers its monitor, position, size and maximized state** between sessions. They
  are saved in Karp's own `karp.cfg`, never in `crispy-doom.cfg`.
- The bot plays **only through input commands** (`ticcmd_t`), exactly like a human on keyboard and
  mouse, so it stays network- and demo-compatible (section 2.4).
- The console has an always-visible **rolling log** of what the bot is doing and thinking.

## 2. Architecture

```
            engine (C)                                  karp (C, console in C++)
  +---------------------------+            +------------------------------------------+
  | D_DoomMain  params -------+--enable--->| karp.c      state, lifecycle, gate,       |
  | D_DoomLoop  after gfx ----+--video---->|             SDL event filter             |
  | P_SetupLevel end ---------+--level---->| k_map.c     geometry snapshot, subsector |
  | RunTic after G_Ticker ----+--think---->|             polygons, nav graph          |
  | G_BuildTiccmd end --------+--command-->| k_path.c    A* over the cell graph       |
  | D_RunFrame ---------------+--frame---->| k_nav.c     path follow + steering       |
  | I_GetEvent (fn pointer) --+--filter--->| k_brain.c   priorities (IDLE, GOTO)      |
  +---------------------------+            | k_log.c     rolling log                   |
                                           | k_console.cpp  ImGui window (map, tabs)  |
                                           +------------------------------------------+
```

Single-threaded: everything runs on the main thread inside the existing game loop.

**Per tic - think** (`K_Ticker`, after `G_Ticker`): read the fresh world state; the brain picks
the active priority; nav tracks path progress, detects off-path/stuck and replans. Runs exactly once
per game tic.

**Per command - act** (`K_BuildTiccmd`, end of `G_BuildTiccmd`): the controller turns the current
waypoint into `forwardmove`/`sidemove`/`angleturn`, the same fields keyboard, mouse and joystick
produce. The command then travels the normal input path: local command queue -> network (in
netgames) -> `G_Ticker` on every machine -> demo recording.

**Per frame** (`K_Frame`): redraw the console (rate-limited).

**Language split:** all bot logic is C99 (matches the codebase, can include engine headers).
`k_console.cpp` is the only C++ file besides ImGui. It must **not** include engine headers. It talks
only to Karp's public headers (`k_map.h`, `k_brain.h`, `k_nav.h`, `k_log.h`), which use plain C
types (`double`, `int`) and `extern "C"` guards. That keeps the engine headers away from the C++
compiler and the console decoupled from engine internals.

### 2.1 Files

```
karp/
  CMakeLists.txt              adds karp sources + vendored imgui to the crispy-doom target
  docs/plan.md                this document
  src/
    karp.h / karp.c           engine-facing API (the only Karp header engine files include);
                              global state, gate, SDL event filter
    k_map.h / k_map.c         map reader + floor cells (convex pieces of subsector floor)
    k_graph.h / k_graph.c     navigation graph: portals, adjacency, live passability
    k_path.h / k_path.c       A* pathfinder
    k_nav.h / k_nav.c         path following, steering controller, stuck detection
    k_brain.h / k_brain.c     priority system
    k_log.h / k_log.c         rolling log (console pane, stdout, karp.log)
    k_settings.h / k_settings.c  Karp's own persistent settings (karp.cfg), e.g. console placement
    k_specials.h / k_specials.c  what lines and things do: exit lines, lines that move sectors
                                 (switches, walk-over lines), keys lying in the level
    k_console.h / k_console.cpp  Karp Console (SDL window, ImGui, map canvas, tabs, log pane)
  third_party/imgui/          Dear ImGui v1.93.0 (latest release at plan time): core files,
                              backends/imgui_impl_sdl2.*, backends/imgui_impl_sdlrenderer2.*,
                              LICENSE.txt, VERSION
```

### 2.2 Engine hooks (complete list)

| # | Where | Hook | Purpose |
|---|---|---|---|
| H1 | `src/doom/d_main.c` after the `-warp` block (:2327) | Three `//!` doc blocks (`@category game`) + `if (M_CheckParm("-autoplay")) K_EnableAutoplay();`, `if (M_CheckParm("-karpconsole")) K_EnableConsole();` and `if (M_CheckParm("-quitonexit")) K_EnableQuitOnExit();` | Turn the bot and/or the console on, and let test runs end by themselves. Warn if `-autoplay` is used without `-warp` (the bot still activates on any level). |
| H2 | `src/doom/d_main.c` `D_GrabMouseCallback`, after the menu/pause check (:517) | `if (K_OwnsControls()) return false;` | While the bot drives there is nothing for the mouse to do, so a windowed game leaves the cursor free to reach the console. A fullscreen game still grabs (crispy decides that before asking the callback); Alt+Tab releases it. |
| H3 | `src/doom/d_main.c` `D_DoomLoop`, right after `I_InitGraphics()` (:611) | `K_PostVideoInit();` | With `-karpconsole`: create the console and install the event filter. Whenever Karp is on: register `I_AtExit(K_Shutdown, true)` |
| H4 | `src/doom/p_setup.c` end of `P_SetupLevel`, before the closing brace (:1402) | `K_LevelLoaded();` | Rebuild the map snapshot and nav graph, reset brain and nav, bump the level generation |
| H5 | `src/doom/d_net.c` `RunTic`, after `G_Ticker()` (:93) | `K_Ticker();` | Think once per game tic on fresh state (read-only) |
| H6 | `src/doom/g_game.c` end of `G_BuildTiccmd`, after the lowres block (between :981 and :982) | `K_BuildTiccmd(cmd, maketic);` | Write the bot's input into the command before it enters the network layer (section 3.6) |
| H7 | `src/doom/g_game.c` `G_PrepTiccmd`, right after the `strafe` declaration (:1398) | `if (K_OwnsControls()) { mousex = mousey = 0; return; }` | Skip crispy's per-frame mouse look while the bot drives. Without it, moving the mouse over the focused game window turns the view until the next tic, then the bot's command snaps it back. |
| H8 | `src/doom/d_main.c` `D_RunFrame`: end of function (after :584) and before the wipe-branch `return` (:548) | `K_Frame();` | Redraw the console (rate-limited); the second call keeps it alive during screen melts |
| H9 | `src/i_video.c` / `i_video.h` (shared by all games) | `I_SetEventFilter(fn)`; call `if (event_filter && event_filter(&sdlevent)) continue;` at the top of the `SDL_PollEvent` loop in `I_GetEvent` (:474); `I_GetWindow()` accessor (`struct SDL_Window;` forward declaration, since i_video.h does not include SDL.h) | Route console events to Karp. Only Doom registers a filter, and only with `-karpconsole`; otherwise it stays NULL. |

`karp.h` is included only by the four doom files above (`d_main.c`, `p_setup.c`, `d_net.c`,
`g_game.c`). H9 is generic and does not reference Karp. None of the hooks write a configuration
variable.

Ordering note: with `-warp`, `G_InitNew` (and so `P_SetupLevel`/H4) runs inside `D_DoomMain`
(`d_main.c:2481`), **before** `D_DoomLoop` creates any window. Map building must therefore not
depend on the console. The console just draws whatever map data exists when it opens.

### 2.3 Build

- Top-level `CMakeLists.txt`: add `enable_language(CXX)` before the `add_subdirectory` loop, and
  `add_subdirectory(karp)` after it (2 lines).
- `karp/CMakeLists.txt`:
  - `target_sources("${PROGRAM_PREFIX}doom" PRIVATE ...)` with **absolute** paths.
    `cmake_minimum_required(3.7.2)` leaves CMP0076 OLD, so relative paths would not resolve as
    expected. Calling `target_sources` from another directory is allowed; only
    `target_link_libraries` had that restriction.
  - `target_include_directories` for `karp/src`, `karp/third_party/imgui`, `src`, `src/doom`.
  - Add `karp/src` to the `doom` static library's include path (for `karp.h`).
  - `target_compile_features(... cxx_std_17)`.
- Sources go straight into the executable, so there is no doom <-> karp static-lib link cycle.
  CMake's MSVC C++ defaults (`/EHsc /GR`, `/MD`) match the C side, and the `SDL2main` WinMain is
  unaffected.
- Only `crispy-doom.exe` gets Karp; Heretic, Hexen and Strife are untouched.
- Autotools is not supported for Karp (the doom-lib hooks would fail to link there). CMake/MSVC
  only, as in `karp/docs/README-build.md`.
- `docs/params.md` (upstream, hand-written) is left alone. The `//!` block keeps docgen happy.

### 2.4 Input-only rule (network and demo safety)

In a netgame every machine runs the same simulation, and only `ticcmd_t` input travels between
them. A bot that touched the world any other way would desync the game (and break demos). So:

1. **Karp acts only by filling `ticcmd_t` in `G_BuildTiccmd`.** The command then goes through the
   normal path: command queue -> `NET_CL_SendTiccmd` -> every machine's `G_Ticker`. This is why
   the bot does not override the command in `G_Ticker`: that runs after the network layer, so only
   the local copy would change.
2. **Command values stay within human input ranges:** `forwardmove` +-50 (above that crispy flags
   "turbo"), `sidemove` +-40, any `angleturn` (a mouse can produce any turn), standard `buttons`
   bits. Fractional values are what an analog stick plus mouse produce.
3. **Karp never modifies game state**: no writes to mobjs, sectors, players or globals, and no calls
   with side effects. In particular:
   - never call `P_Random` (it shares the synchronised RNG);
   - never call `P_CheckPosition`/`P_TryMove` (they pick up items and move things);
   - never call `P_PathTraverse`/`P_CheckSight` (they bump `validcount`);
   - never call `R_PointToAngle2` (it overwrites `viewx`/`viewy`); use `atan2` on doubles.

   Pure lookups such as `R_PointInSubsector` are fine. Karp answers geometry queries from its own
   `k_map` data.
4. **Think and act from the local player's view only** (`consoleplayer`), so in a future netgame
   each machine can run its own bot for its own player.

Stage 1 still refuses to drive in netgames (the activity gate requires `!netgame`; Karp logs this
once and stays idle), simply because it is untested there. The architecture above keeps lifting that guard a later-stage task, not a
redesign.

## 3. Modules

### 3.1 `karp.c` - glue, lifecycle, event filter

- Two independent switches:
  - `K_EnableAutoplay()` sets `karp_autoplay`: the bot drives the player;
  - `K_EnableConsole()` sets `karp_console`: the console window opens.

  Karp is "on" when either is set. With neither set, every hook is a no-op, so a normal run behaves
  exactly like upstream crispy.

  | Params | Result |
  |---|---|
  | `-autoplay -karpconsole` | The bot heads for the level's exit (GO TO EXIT, 3.5); a click on the map sends it somewhere else first |
  | `-autoplay` | The same without the console; it logs to stdout and `karp.log` |
  | `-karpconsole` | Viewer: the human plays normally. Clicks plan and draw a path, but nothing drives the player. Useful for debugging the map reader and pathfinder. |

  `-quitonexit` (any combination) quits the game as soon as a level is finished, so a test run
  ends by itself and leaves a complete `karp.log`. Test runs on real maps use the normal game
  (no fast-forwarding), so they can be watched: `-warp NN -nomonsters -autoplay -karpconsole
  -quitonexit`.
- **No configuration changes.** Karp never writes `fullscreen`, `use_mouse`, window sizes or any
  other crispy config variable, not even temporarily. Video mode and the game window stay entirely
  under the user's control. Karp's own preferences live in its own file, `karp.cfg` (3.9).
- `K_PostVideoInit()`:
  - with `-karpconsole`: load `karp.cfg`, open the console (3.7) and install the event filter;
  - whenever Karp is on: register `I_AtExit(K_Shutdown, true)`. It runs before
    `I_ShutdownGraphics`, which would otherwise destroy every SDL window via `SDL_QuitSubSystem`.
- `K_Shutdown()`: save `karp.cfg` if anything changed, clear the event filter, shut down ImGui,
  destroy the console renderer and window.
- The log has its own exit handler (`K_Log_Shutdown`), registered as soon as either switch is
  enabled. Because it is registered earliest it runs last, so `karp.log` captures everything said
  while shutting down.
- **Activity gate**, exposed as `K_OwnsControls()` (used by H2, H5, H6, H7):
  `karp_autoplay && !netgame && gamestate == GS_LEVEL && gameaction == ga_nothing && !paused
   && !menuactive && !demoplayback && players[consoleplayer].mo && playerstate == PST_LIVE`.
  Never cache `players[consoleplayer].mo` across tics; loading a savegame replaces it.
- **Event filter** (only installed with `-karpconsole`; called for every SDL event in `I_GetEvent`):
  - any event whose `windowID` is the console's: feed `ImGui_ImplSDL2_ProcessEvent`, consume.
    Keyboard events carry the focused window's ID, so keys typed in the console never reach the game;
  - `SDL_WINDOWEVENT_CLOSE` on the console: hide the window (never destroy it), consume;
  - `SDL_WINDOWEVENT_CLOSE` on the game window: post `ev_quit` with `D_PostEvent`, do not consume.
    With two windows SDL only sends `SDL_QUIT` when the last window closes, and a hidden window
    still counts. Without this the game's X button stops working; with it you get the normal quit
    prompt;
  - everything else, Alt+Enter included, goes to the game unchanged.

### 3.2 `k_map.c` - map reader

Built in `K_LevelLoaded()` from the engine globals (`vertexes`, `lines`, `sides`, `sectors`, `segs`,
`subsectors`, `nodes`; `r_state.h:77-96`). All level memory is `PU_LEVEL` and is freed on the next
load, so Karp rebuilds everything on every `P_SetupLevel` into its own `malloc`ed arrays.
Coordinates are converted from `fixed_t` to `double` map units.
- Use `vertex_t.x/.y`, never the render-only `r_x/r_y` (slime-trail nudged).
- A subsector's sector is `subsector_t.sector`, never a seg's `frontsector`.

Output (`k_map_t`, plain C types):
- `lines[]`: endpoints, flags, special, front/back sector index. `K_Map_LineClass()` gives the
  automap-style draw class (one-sided, floor change, ceiling change, plain two-sided) from live
  sector heights, so a door or lift changes colour as it moves.
- `K_Map_GetPlayer()`: the local player's live position, facing, momentum, the floor it stands on
  and the cell it stands on (`K_Map_StandingCell`, below), and how long it stays frozen after a
  teleport (read-only).
- `sectors[]`: index into engine `sectors[]`. Heights are read **live**: floors will move in later
  stages, and loading a savegame changes heights after `P_SetupLevel`.
- `cells[]`: convex pieces of floor, each with its subsector, polygon, area-weighted centroid and
  bbox. These are the nodes of the navigation graph (see below).
- `subsectors[]`: sector index, its range of cells, total floor area and centroid, and flags.
- Map bounds for the console view.

The navigation graph (portals, adjacency, passability) lives in its own module, `k_graph.c`
(3.2.1).

**Floor cells.** In vanilla nodes a subsector's segs only cover its linedef edges; edges along
partition lines are missing, and nothing in the engine rebuilds them. One algorithm handles any
node format:
1. Start with the map bbox, padded, as a polygon.
2. Recurse from the root node (`numnodes-1`). At each node, split the polygon by the partition line
   (`x, y, dx, dy`): the right side goes to `children[0]`, the left side to `children[1]`. This
   matches `R_PointOnSide`, which returns 0 for points right of `(dx,dy)`. Stop at
   `children & NF_SUBSECTOR` (`0x80000000`). The result is the leaf's **BSP region**.
3. Split the region by **every linedef that passes through it** into convex pieces.
4. Classify each piece by the sector at its centroid (see below). Floor pieces become cells; void
   pieces are dropped.
5. A map with `numnodes == 0` (a single subsector) uses the bbox as its region.

Why split by all lines through the region, and not just clip by the leaf's own segs (the first
version, M3)? A subsector's **segs** are guaranteed convex, but its **floor** is not, and its
region need not stop at its own walls:
- a wall corner or stub inside the region leaves floor beside the wall, past the end of its line,
  which clipping by that line's half-plane throws away;
- void is never visible, so nodebuilders give it to whichever leaf is convenient, beyond walls
  whose segs belong to other leaves; clipping by the leaf's own segs keeps that void as "floor".
The M4 hole check (3.2.1) found both: holes of up to 1,000+ units of border.

**Sector at a point** (`SectorAtPoint`): a ray from the point finds the first linedef it hits; the
point is in that line's front sector if it is on the line's right, else its back sector, and in
void if that is a one-sided line seen from behind (or nothing is hit). Three rays at
non-axis-aligned angles vote. An even-odd test over sector outlines was tried first, but vanilla
maps have unclosed sector outlines that flip its parity and drop real floor; the ray test is local,
so a gap somewhere else in an outline cannot fool it.

**Leaves without floor.** A polygon has "no usable floor" when its area is below 0.01 or its
average width (area over half the perimeter) is under 1 map unit; nothing fits there, since the
player is 32 units wide. Each leaf is classified:
- **No area** (harmless, no cells): the BSP region itself has no usable floor. These are leaves
  from splitting twice along the same line, and hairline slivers from vertex rounding.
- **Behind walls** (harmless, no cells): every piece of the region is void, e.g. inside a pillar.
- **Split** (informational): the floor is not convex and needs several cells.
- **Other-sector floor** (informational): some piece lies, by the surrounding lines, in a
  different sector than the leaf's (sector tricks, buggy nodes). The engine uses the leaf's sector
  for floor height there, so the graph does too.

Measured on 100 maps (Doom, Doom II, Plutonia; ~54,000 subsectors, ~52,000 cells): 0 unreached
subsectors and 0 centroid mismatches on every map; under 60 subsectors are split. Builds take
1-70 ms per map (the ray test checks every linedef; a line grid can cut this if ever needed).
Extended node formats and UDMF are out of scope (the user's decision): maps can always be rebuilt
with a better nodebuilder if needed.

**Load-time sanity checks** (shown in the console's Map tab and logged): leaves without area,
behind walls, split, with other-sector floor; subsectors not reachable through the BSP tree; cells
whose centroid `R_PointInSubsector` places in another subsector; build time. The first few
problems of each kind are logged individually with their location.

`K_Map_CellAt(x, y)` finds a point's subsector via `R_PointInSubsector` (a pure lookup), then the
cell of that subsector containing the point (within 1 unit), or -1 when the point is in the void.
If that subsector has no such cell (a hairline sliver without floor, or a point just across a
partition line), it falls back to any cell within 1 unit. `K_Map_NearestCell(x, y, max)` scans
all cells for the nearest one within a distance; the pathfinder uses it when the player stands on
one of the few holes in the mesh. `K_Map_SubsectorAt` wraps `K_Map_CellAt`. The console's hover
info and click validation use them.

**Where the player stands** (`K_Map_StandingCell`). The engine stands the player on the highest
floor under its 32x32 box (`mo->floorz`), not on the floor under its centre: with its centre just
over the edge of a ledge, it still stands on the ledge. The cell containing the centre is then the
one below, and judged from there the ledge is a step too high, so every route from it looked
blocked (Doom II MAP03: the bot drifted a unit off a 32-wide strip on its way to a teleporter, and
every goal failed). So when the player's floor is more than a step (24) above the floor under its
centre, the player's cell is the nearest cell under its box that has that floor. Up to a step,
the centre's cell is kept: the player can step back up from it, and a cell under the box can be
too small to plan from (Doom II MAP05: an 8-unit ledge in front of a door, with every way out of
it too narrow). Route progress and the start of every plan use this cell, and the Bot tab shows
it.

#### 3.2.1 `k_graph.c` - navigation graph

**Portals.** Two cells are neighbours where their edges lie on the same line, run in opposite
directions (all cells are counter-clockwise) and overlap by at least 1 unit; each overlap is a
portal segment, which path smoothing will need later. Vanilla nodebuilders round split vertices
to whole units, so a seg (and an edge built from it) can sit at a very slightly different angle
from its linedef. So only the **shared stretch** has to lie on both edges: within 1 unit of the
other edge at its middle and 2 units at its ends. Requiring the whole of a long line to be within
tolerance failed on long lines; a thin wall still never matches, as it is off by its full
thickness everywhere. The distance is to the other edge **segment**, not its infinite line, and
the stretch must be at least 1 unit long measured along both edges. Without that, two edges
meeting nearly at right angles could share a short bogus stretch beyond the end of one of them;
the M5 pathfinder test found such portals, with their middles outside one of their cells.
Edges and linedefs are bucketed in a uniform 128-unit grid. Each portal records the two-sided
linedef it runs along, or -1 when both cells are in the same sector. A portal that runs along
several collinear lines is **split** where each line starts (`SplitPortals`), so every portal
runs along one line and is judged by it: its special, whether it blocks the player. Doom II MAP12
has a diagonal key door between two short jambs on the same line; the portal had recorded a
jamb, so the door looked like a closed sector that nothing opens.

**Passability** of a portal, leaving a given cell, is evaluated **live** (`K_Graph_CanCross`). It
is cheap, and it makes doors and lifts work naturally in later stages. It mirrors
`P_TryMove`/`PIT_CheckLine` (`src/doom/p_map.c:212-285, 564-631`), with the sector of each cell's
subsector:
- blocked if the crossed linedef is one-sided (a map error) or has `ML_BLOCKING`;
- blocked if `floor(B) - floor(A) > 24` (step-up). This is directional: dropping down is always
  fine, because players have `MF_DROPOFF`;
- blocked if `min(ceil A, ceil B) - max(floor A, floor B) < 56` or `ceil(B) - floor(B) < 56`
  (closed doors).

**Doors and lifts** (manual ones, worked by using the line being crossed). Some blocked crossings
become passable after an action, also judged live, with the engine's own rules:
- **Door:** no headroom, but the line has a manual door special (1, 26-28, 31-34, 117, 118: the
  special is read from the engine's line, since one-shot doors lose it), the player is on its
  front side (use only works from there) and the door is the line's back sector. It opens to the
  lowest surrounding ceiling less 4 (`EV_VerticalDoor`), computed now; if that is not high enough
  the crossing stays blocked. Without the key a key door is **locked** (its own crossing kind,
  named with the key in the "no path" reason).
- **Lift:** a step too high, but the line is a lift switch (21, 62, 122, 123) tagged with the
  sector beyond, the player is on its front side, and the lift's bottom (the lowest surrounding
  floor, as `EV_DoPlat` computes it) is within 24 of the player's floor.
- **Lift ride:** a step too high off a floor that will rise to within 24: a lift that is down or
  coming back up (an active lift or plat remembers its top), or any floor on its way up (a plat
  raising it, a floor mover going up: Doom II MAP03's strip to the teleporter). A ride is not
  judged for width at the height it starts from, where the sides of the way off can be walls,
  but when it gets there.
- **Lifts that come down by themselves:** a perpetual lift (53, 87; a stopped one has no
  thinker), or one on its way down. A lift too high now to step onto, or to fit over, is a lift
  crossing if its bottom works: wait for it (no press), step on. On one that is too high to fit
  under the way off, the crossing off is a lift ride: ride it down. The wait is worked out from
  the lift's state (`T_PlatRaise`: still to go up, the wait at the top, the way down). Doom II
  MAP12's exit area is reached over perpetual lifts that go up and down all the time.
- Standing in a door sector means its door is open, so crossings out of one are judged with the
  door's open height: a manual door's, and that of any door already opening (a switch opened it,
  and the route goes on through it before it is fully up). The clearance check takes them as
  open too. Going in is still a door crossing to wait at.
- Each also gives the time it is expected to take: stopping and pressing (~1 second), plus the
  door's travel at 2 units/tic (8 blazing), or for a lift the trip down and up at 4 (8) and its
  3-second wait at the bottom. The pathfinder charges that time as distance (16 units a tic), so
  an open route that is not much longer still wins.

Doors and lifts worked from elsewhere (remote switches, walk-over lines) are the brain's job: it
goes and works the line first (3.5). Not yet: things blocking a doorway.

**Narrow gaps** (`K_CROSS_NARROW`). The cells treat the player as a point, so a crossing the
heights let through is still no way if it is narrower than the player: gaps between bars or
raised blocks, strips beside a pillar. A crossing is a way through only if some point on it (9
are sampled) is at least **17 units** from every obstacle: the player's radius (16) plus one, so
the narrowest gap that counts is 34. A gap exactly as wide as the player (32) needs it lined up
to the fraction of a unit; Doom II MAP02 has one the bot got stuck in. Obstacles are the lines
that stop a player standing on the crossing's floor from overlapping them (`PIT_CheckLine`): walls,
impassable lines (`ML_BLOCKING`), and two-sided lines with a side too high to step onto or too low
to fit under. Which floor: the engine stands the player on the highest floor under its box, under
the lowest ceiling. Lines before the crossing are judged from the cell the player comes from; a
line wholly past it is reached with the box over the cell beyond too, so it is judged from the
higher of the two floors and the lower of the two ceilings (only for a real step of up to 24, not
for a lift to lower or a ride up). Otherwise stairs whose steps are shallower than the player's
radius look too narrow: on Doom II MAP03 the steps are 8 high and 16 deep, and from the floor below
the first one, the second one's riser looked like a wall 16 units from the crossing. A drop is no obstacle (the player can hang over it), and manual doors count as open,
since the player opens any it gets to (otherwise a closed door's faces, 8 units apart, would
narrow every crossing near a doorway). Judged live, so bars that lower open the gap.
`K_Graph_NarrowSectors` names the sectors that narrow a gap (the far side of the obstacle
lines), so the brain can look for a line that moves them.

**Where a route crosses a portal** (`K_Graph_PortalPoint`): not always its middle, but the point
on it that is at least 24 units from every obstacle, the one nearest the middle if several are;
where no point is that clear, the clearest one. In open rooms that is the middle; next to a wall
corner it moves away from the corner, so the player does not clip it (Doom II MAP02 again).

**Teleporters** (`K_CROSS_TELEPORT`). Lines 39 (once) and 97 (repeatable) teleport the player
when crossed from their front side (125 and 126 are for monsters only). At level load the lines
are grouped by tag into **teleporters**, each with its destination found as `EV_Teleport` finds
it (the first teleport destination thing, in thinker order, in the first sector with the tag),
the way the player faces on arrival, the destination's cell and the bounding box of its lines. A
tag whose destination is missing or off the floor is logged; crossing its lines does nothing.
- A portal along a teleport line records the cell on the line's front side. Crossing out of that
  cell is a teleport if the heights and the clearance let the player across (judged as for any
  crossing, into the cell beyond the line) and the line still has its special: a once-only
  teleporter loses it when used, so this is judged live. Its "wait" is the 18 tics the player
  stands frozen on arrival.
- The player lands at the destination, not in the cell beyond the line: the search treats the
  crossing that way even when the heights block it (a teleporter on a raised ledge still leads to
  its destination once the ledge is lowered).
- The graph log counts teleporters; the graph overlay draws teleport crossings in purple and a
  dashed line from each teleporter to its destination, with a ring there.

**Hole check.** Every stretch of every cell's border should border a neighbour (a portal) or lie
along a one-sided wall. A stretch that does neither, is at least 4 units long and more than 2 units
from any wall is a **hole** in the navigation mesh (shorter gaps are rounding at corners, gaps
along a wall are hairline slivers of void). The graph also counts isolated cells and connected
components (groups joined by portals, open or not; areas reached only by teleporter form their own
groups).

Measured on the same 100 maps: 99,940 portals; **41 holes left in total** (1,033 units of
border; from ~1,600 holes in the first version), most of them under 30 units: sectors with broken
outlines that both tests read as void, narrow strips, and a few neighbouring cells whose edges did
not match. Graph builds take under 6 ms per map. E1M1: 425 portals, 0 holes, one component.

### 3.3 `k_path.c` - pathfinder

`K_Path_Find(path, start, goal)` plans from a start point to a goal point:
- **Start and goal cells** via `K_Map_CellAt`; the caller can give the start cell instead, and
  nav and the brain give the cell the player stands on (`K_Map_StandingCell`, 3.2). A player
  standing on a hole in the mesh starts from the nearest cell within 64 units
  (`K_Map_NearestCell`).
- **A\* over portal crossings, not cells.** A search node is "crossed portal p into the cell on
  side s" (two nodes per portal), plus the start and the goal. A route runs from the start point
  through the **middle of each portal** it crosses to the goal point, and crossing a cell costs the
  straight line from the portal it came in by to the one it leaves by. Both lie on the border of
  the convex cell, so that leg stays inside it and cannot cut through a wall corner.

  The first version of this plan went through each cell's centroid instead (`portal_mid(1),
  centroid(2), portal_mid(2), ...`). That is just as safe, but it detours deep into long cells
  and back: a corridor cell entered and left near one end sent the route to its middle. Portal
  middles alone are shorter, and the search optimises exactly the line that is drawn and driven.
- The heuristic is the straight line to the goal, which no route can beat (consistent, so an
  expanded node is final). Binary-heap open set; per-node arrays sized to the portal count, reset
  with a search stamp.
- **Teleporters.** The node "crossed a teleport line into its back side" stands at the
  teleporter's destination, in the destination's cell, and the search goes on from there. A
  teleporter is a shortcut the straight line does not know about, so the heuristic becomes the
  lesser of the straight line to the goal and, for each teleporter, the straight line to the box
  around its lines plus the heuristic at its destination (those are worked out per search, letting
  one teleporter lead to another, until they settle). It still never overestimates and stays
  consistent, so the search still finds the shortest route.
- **Blocked crossings are not skipped but cost a large penalty**, more than any open route. One
  search then gives the shortest open route if there is one, and otherwise the route with the
  cheapest blocked crossings, whose first blocked crossing tells why the goal cannot be reached
  (with how many there are, and the cell it is crossed from). The penalty comes from the brain
  (`K_Path_SetBlockCost`): 10^6 for a crossing it knows how to get past (a key lies in the level, a
  line moves the sector), 10^8 for one it does not, so blocked routes run through fixable
  crossings. Crossings are judged live (`K_Graph_CanCross`).
- **Waypoints**: the route's line passes each portal at `K_Graph_PortalPoint` (3.2.1), not exactly
  at the middle the search costed, so it keeps clear of wall corners.
- **Legs along a wall.** When a route enters and leaves a cell by two portals on the same edge,
  the leg between them runs along that edge. That is common and harmless along a partition line,
  where the edge is all portals, but between two doorways in one wall it would scrape the wall. A
  leg that runs along a stretch of border that is not a portal (sampled at 1/4, 1/2 and 3/4) goes
  through the cell's centroid instead.
- **Output** (`k_path_t`): the result, the route's cells and the portals between them (for
  progress tracking in M6), the points to follow (start, portal middles, the occasional centroid,
  goal), the length, the first blocked crossing with its kind and height, nodes searched and time.
  At a teleporter the points go on from the point on its line to one 32 units past it (so the
  player surely crosses it), then to the destination; that last leg is marked as a jump
  (`k_path_t.jumps`), is not walked and does not count in the length.
- **Results:** `FOUND`; `BLOCKED` (every route crosses something the player cannot cross now: a
  step too high, a closed door, a blocking line); `UNCONNECTED` (nothing connects them, no portal
  nor teleporter, e.g. a closed-off area); `NO_START` / `NO_GOAL` (not on any floor).
  `K_Path_Describe` turns a result into one line for the log and the status line, e.g. `a step
  64 units high at line 312 (1024,-320) - the player climbs at most 24` or `only 0 units of
  headroom at line 151 (1520,-3264) - the player needs 56`.

Measured with a temporary self-test (300 random start/goal pairs per map, 30,000 routes over the
100 maps): 0 errors. It checked every result against an independent reachability search (open
crossings only, and all portals), that every route is continuous and every leg stays inside its
cell, and that every open route is exactly as short as a plain Dijkstra search finds. Searches
average 0.005-0.36 ms per map, 1 ms at worst. About 10% of routes go through a centroid somewhere,
in cells that are entered and left through two openings in one wall.

Later: funnel/string-pulling over the portal list, with portals shrunk by the player radius, for
smooth runs.

### 3.4 `k_nav.c` - path following and steering

`K_Nav_SetGoal(x, y)` plans a route from the player's position (`K_Path_Find`), logs the outcome
and starts driving it; `K_Nav_Reset` drops it on every level load. `K_Nav_GetStatus` reports the
state (idle, driving, failed), the waypoint being driven to, distance left, speed, time, replans
and prediction error for the console. Without `-autoplay` nothing ticks, so a route is only
drawn.

Physics facts the controller is built on (`p_user.c:151-216`, `p_mobj.c:160-302`):
- thrust per tic = `move * 2048 / 65536` units, so forwardmove 50 gives 1.5625;
- each tic: turn, then thrust (on the ground only), then move by the momentum (each axis clamped
  to 30), then friction 0.90625 (on the ground only; momentum under 1/16 with no input stops);
- running forward tops out at 16.7 units/tic of movement; coasting from there takes ~161 units to
  stop, braking with full reverse thrust ~45.

**Think - `K_Nav_Tick()`, once per game tic** (from `K_Ticker`, through the brain):
- **Progress:** the cell the player stands on (3.2) is looked up in the route's cells. Moving
  further along the route (perhaps having cut a corner) moves the target waypoint to the first
  one inside that cell, never back; being pushed back to an earlier route cell sets the target to
  that cell's way out again. The route remembers which cell each leg runs through (`k_path_t.leg_cells`) for
  this. A waypoint on the way also counts as reached within 24 units (tunable).
- **Off the route:** a cell that is not on the route but is one open crossing from a route cell
  (a corner cut through a neighbouring cell) is tolerated. Anywhere else (fell off a ledge, got
  pushed) the route is replanned from where the player is, with a log line.
- **Stuck:** moved under 8 units over the last 35 tics: replan, with a log line. After 3 times in
  a row the goal fails ("stuck at (x,y)"); the count starts over once the player gets 64 units
  from where it last got stuck. A replan that finds no route fails the goal at once.
- **Arrival:** at the last waypoint, within 16 units of the goal and slower than 1 unit/tic: the
  goal is done and the route is dropped.
- **Prediction check:** the position predicted for the tic that just ran is compared with the
  real one; the average over about a second is the prediction error shown in the Bot tab.

**Act - `K_Nav_BuildCommand(tic)`, once per command** (from `K_BuildTiccmd`):
1. **Predict.** In single player a command is built 0-2 tics before it runs (`d_loop.c:169`;
   `gametic` is the next tic to run, `maketic` the one being built), and more in netgames. Nav
   keeps the commands it sent in a 16-entry ring keyed by tic and replays the ones still waiting
   to run (`gametic` up to the new tic) on the current player state with the physics above,
   ignoring walls. The result is the position, momentum and angle when this command will run.
   If a waypoint is reached during those tics, the next one is the target.
2. **Speed profile.** The speed wanted now is the lowest of: the top speed (16.7 units/tic,
   tunable) and, for each waypoint within 400 units along the route, `sqrt(v_k^2 + 2 a s_k)`,
   where `s_k` is the distance along the route to it, `a` the assumed braking (1.5 units/tic^2,
   tunable, about half of what full reverse thrust gives) and `v_k` the speed to take that
   waypoint at: full speed where the route runs straight on, falling with the cosine of the turn
   to 20% of it through a right angle or sharper, and 0 at the goal. The plan's first version
   used `k * distance` toward the goal only; corners need slowing for too. Toward the goal the
   speed is also capped at the distance left, so one tic never carries the player past it.
3. **Velocity controller.** Desired velocity `v_des` = that speed toward the target waypoint.
   Required thrust `T = v_des - momentum`, decomposed in the player frame using the **post-turn**
   angle `a'`: `forward = T . (cos a', sin a')`, `side = T . (sin a', -cos a')`, scaled to command
   units (`/ (2048/65536)`). If either exceeds its limit (forward +-50, side +-40), both are scaled
   down so the thrust direction is kept, then rounded. Movement therefore does not depend on
   facing, and pushing against the momentum brakes. Airborne, no movement is sent (it would do
   nothing).

   **Sliding along walls.** The route keeps its legs inside cells, but not clear of the corners
   along them, so a leg can pass close enough to one that the player's box catches on it, pushing
   straight in, with nothing for the engine to slide it along (Doom II MAP03: leaving a switch
   alcove past a pillar, the bot stood there until it gave up). So within 20 units of anything
   that stops the player (`K_Graph_ObstaclesNear`: walls, impassable lines, steps too high, too
   little headroom), the part of `v_des` toward it is dropped, and touching it (closer than 17),
   the player eases away at 1 unit/tic. Drops are not obstacles, so this never steers toward a
   ledge's edge on purpose.
4. **Heading:** turn toward the desired velocity, at most 11.25 degrees per tic (2048 angleturn
   units, tunable). Cosmetic for now; later stages need it for aiming and using. When recording a
   demo without `-longtics`, the turn is rounded to a multiple of 256; no remainder needs carrying,
   since the next command aims from the actual angle again.
5. Idle or failed: zero movement.

The controller settings are adjustable in the console's Bot tab, but not saved yet.

**Line goals.** Besides a point, nav can be sent to work a line (`K_Nav_SetLineGoal`): press a
switch, or walk across a line. `K_Nav_LineGoalPoint` gives the spot: 24 units in front of a
switch's front side (use only works from there), or just past a walk-over line on the side away
from the player; 32, 16 and 40 units are tried in turn until the spot is on floor. On arriving at
a switch's spot the player presses use as for a door (below), facing the line; a press after
which nothing happens within 15 tics is repeated, 3 times at most. What the switch does is up to
the caller (an exit ends the level). A walk-over line works only if the player crosses it, but
the route to the spot past it can go round the line's end instead (Doom II MAP05); then the
player first goes to the spot just as far before the line, and from there across.

**Doors and lifts on the route** (`UpdateAction` in Think, `SteerAction` in Act). The target never
moves past the way out of a cell whose crossing is not open yet, and the speed profile treats
that waypoint as a stop. "The" crossing is the first one from the player's route cell on that is
not open, not just the next one: the player can stand just off the route, in a cell beside it
(Doom II MAP01: on the edge of a small light sector right in front of the exit door), while the
route cell it was last in leads on openly. When the target reaches it and the crossing is a
door, lift or lift ride, an action starts:
1. **Walk up:** drive to a spot 24 units in front of the middle of the door's or lift's whole line
   (on the player's side) and stop; the use check accepts the whole line too. Not in front of the
   portal: a doorway can be split into several portals, and standing in front of one at its edge
   put the spot closer to the side wall than the player's radius (Doom II MAP01's exit door, where
   the bot pushed against the wall and never pressed). Doors and lifts sit centred in their
   alcoves. If the spot in front of the line's middle is off the floor, the portal's middle is
   used. Close enough after 2 seconds, since the spot may be in a wall. The player starts facing
   the line on the way, turning at up to 45 degrees a tic.
2. **Press use** (door, lift), holding the spot: only when the engine's use trace would hit the
   line (64 units straight ahead from where the player is when that tic starts, after its turn;
   the check uses the predicted position and angle and allows 60), and only when it is worth it.
   Pressing on a door that is opening or open closes it, and on a busy lift does nothing, so the
   door must be still or closing and the lift still. A door already opening, or a lift already
   on its way (or one that comes down by itself, 3.2.1), needs no press. Use is released for at
   least a tic between presses, or the engine ignores it.
3. **Wait**, holding the spot, for as long as the door or lift keeps moving. When the crossing
   turns open the action is over and the route goes on: through the door, or onto the lift. On
   the lift the crossing off it is a lift ride: walk to a spot 24 units from its edge and wait
   for the ride, up or down. A door that starts closing before the player got through is pressed again
   (that reopens it); a press after which nothing moves within 15 tics (a miss, or a lift that
   went back up without the player) is repeated, 3 times at most.
4. **Giving up:** nothing moving for 5 seconds while still blocked (a lift waits 3 at the
   bottom), never able to press within 3 seconds, or 3 presses that moved nothing: the goal
   fails with the line in the reason. If the crossing becomes blocked outright (a door that no
   longer opens high enough because a neighbouring ceiling moved, a key door), the route is
   replanned.

Stuck detection pauses while pressing and waiting. The Bot tab shows the action ("door at line
151: waiting"), the log its start and end ("opening the door at line 151", "door at line 151
open after 1.4 s", "waiting for the lift (sector 41) to come down at line 678", "riding the lift
(sector 41) to the way off", "lift ride over after 4.9 s"), and the graph overlay draws
crossings that need an action in blue. The player presses use only through the ticcmd's `BT_USE`, like a human.

`K_Nav_Clear` drops the goal so the player stands still, e.g. while a sector the brain just moved
settles.

**Teleporters on the route.** A teleporter needs no action: the target stops at the point past
its line, as at a door, but the speed profile does not slow down for it, since the teleport stops
the player anyway, nor for the corner at the destination. The bot runs across the line, lands at
the destination, and progress picks up from there (the destination's cell is the next route cell).
A route can come back to a cell it went through before a teleporter, so the route cell is looked
up from the player's current one on first.
- A position jump no step could make (over 64 units in a tic) is a teleport: logged ("teleported
  from (x,y) to (x,y)"), and neither a misprediction nor progress for the stuck check.
- The player does not always land exactly on the destination: the engine moves a fast player in
  steps (`P_XYMovement`), and the step that crosses the line goes on from the destination, so at
  running speed it lands a few units past it (8 on Doom II MAP03), possibly in the next cell.
  Landing next to the route (one open crossing from a route cell) counts as being on it, at that
  route cell; landing anywhere else, the bot replans at once.
- For 18 tics after a teleport the player can neither move nor turn (`reactiontime`). The pose
  reports it, the prediction replays it, and the bot sends empty commands meanwhile.
- If the teleporter on the route stops working before the bot gets there (a once-only one used
  up, or blocked), the route is replanned.

**Measured** with the drive test on Doom (36 maps): with doors, 250 goals could be picked (204
before; areas behind doors became reachable) and 148 were reached; 46 doors were opened along
the way. The first run found one bug: one-shot doors lose their line special the moment they
are used, so a door already opening must count as a door whatever its line says (it now does,
using the height its thinker is heading for). In a lift mode (every goal just past a lift),
every lift the bot reached worked: called, boarded, ridden up. Nearly all failures were still
"stuck" (gaps narrower than the player, solid things), mostly before the bot reached a door or
lift. Seen in play but not looked into: on E1M2 the bot got stuck at the red door.

**Measured** with a temporary drive test (8 random reachable goals per map, one after another,
run fast-forward), on Doom (36 maps): 138 of 204 goals reached, 66 failed, none timed out. 12 maps
reached every goal, most without a single replan. Average speed along the route 9-13 units/tic.
The prediction is off by 0.0-0.2 units per tic where the player moves freely; it grows only
while the player pushes against something (prediction ignores collisions). 10 maps got no goals:
their start areas are closed off by doors, which are out of scope for stage 1.

Almost all failures cascade from a few places where the player got stuck: the replan gives the
same route, and every later goal starts from the same spot. The causes, each to be fixed and
tested in its own commit (section 4, M6 follow-ups): routes through gaps narrower than the
player, solid things (torches, lamps, barrels, columns, trees) that are not part of the geometry,
and no way to work free once stuck.

### 3.5 `k_brain.c` - priorities: a stack of goals

The brain keeps a **stack of goals**; the top one is worked on, and each goal below waits for the
one above it. Under `-autoplay` every level starts with one goal, **GO TO EXIT**. When the top
goal cannot be reached, the brain works out what is in the way and pushes the fix in front of
it, so the list grows into a chain of events that finishes the level, e.g.:
```
USE SWITCH line 87 (tag 2)                            <- worked on
GET BLUE KEY - blue keycard at (928,576), 2943 units
GO TO EXIT - exit switch at line 495
```

**A goal is a condition plus candidates.** The condition says when it is done; the candidates
are the places it can be met, and which one is gone for is chosen when the goal is planned, not
fixed when it is pushed (the map can have several of anything, and an author can put one where
it cannot be reached):

| Goal | Done when | Candidates (what the player does there) |
|---|---|---|
| GO TO EXIT | the level ends (`gameaction == ga_completed`) | every exit line: 11 and 51 pressed, 52 and 124 crossed (secret exits too) |
| GET <colour> KEY | the player holds the keycard or skull of that colour, however it got it | every key of that colour lying in the level (cards and skulls, by sprite); the player goes to a pickup spot (below) |
| USE TRIGGER (sector) | the crossing it was pushed for is open, or will be after a wait (a door opening, a lift on its way, a floor to ride up); else the sector moved and stopped (or after 15 s: perpetual lifts never stop) | every line with the sector's tag whose special moves sectors (`K_Specials_MoverTrigger`): switches pressed, walk-over lines crossed. Shown as `USE SWITCH line N (tag T)` or `CROSS LINE N (tag T)` once one is chosen |
| GO TO (x, y) | the player arrived | the point (a console click) |

**Planning a goal** (`PlanGoal`) scores every candidate not yet tried by route
(`K_Path_Find` with the brain's block cost, 3.3):
- the shortest **open** route wins, and nav gets the candidate (a point, or a line to press or
  cross, 3.4);
- if none is open, the candidate whose blocked route is cheapest wins (fewest blocked crossings,
  then shortest), and the fix for its **first** blocked crossing is pushed (`FixFor`):
  - a **locked door** -> GET KEY of its colour, if such a key lies in the level;
  - a **step too high or too little headroom** -> USE TRIGGER for the sector beyond, or the one
    the player stands in, if some line moves it;
  - a **gap too narrow** -> USE TRIGGER for a sector that narrows it (`K_Graph_NarrowSectors`),
    e.g. bars that lower; else for the sector on either side of the crossing, e.g. a lift that
    takes the player down to where the way on has headroom (Doom II MAP05: a passage whose
    ceiling is level with the floor in front of it until that floor, a lift, comes down);
  - a **switch that needs a key** the player lacks (remote locked doors 99 and 133-137, e.g.
    Doom II MAP02's red bars) -> GET KEY of that colour, before going to press it;
  - **keys first:** if a door further along the same route needs a key that lies in the level,
    GET KEY is pushed instead of the fix for the first crossing. A key stays, while what a switch
    does may not last until the player gets there: on Doom II MAP12 the bot ran 11,000 units to
    lower a lift, fetched the yellow key while it went back up, and had to lower it again;
  - USE TRIGGER only for a sector some line still moves (one-shot lines lose their special);
- if nothing can be fixed, the goal **fails**, with the reason for the last candidate tried.

**Pickup spots.** A key is picked up when the player's box touches the key's (radius 20), so
within 36 units on each axis, not only at the key itself. If the player cannot stand at the key
(less than 17 units from anything that stops it: `K_Graph_Clearance`), the candidate is the
nearest spot within 28 units on each axis where it can (searched in 4-unit steps). Doom II MAP05's
red key lies on an impassable line, at the end of a shaft, and is picked up from the room beside.

When a goal is done it comes off the stack and the goal below is planned again, from wherever the
player is: the world has changed. Fixes are optimistic: the switch that targets a sector is used
without working out exactly where it moves the sector; after it moves, planning again shows
whether the way is open, and if not, the next blocker is pushed.

**Setting off at once.** A USE TRIGGER goal remembers the blocked crossing it was pushed for. As
soon as that crossing is open, or passable after a wait (a door opening, a lift on its way down,
a floor to ride up), the goal is done and the goal below is planned again: the player sets off
right after the press, as a human would, and waits at the door or lift instead of at the switch.
Doom II MAP03 got 6 seconds faster: switch 431 raises a strip slowly, and the bot now drops onto
it from the walkway while it rises and rides it up to the teleporter. Otherwise the goal waits,
with the player standing still, for the sector to start moving after its line is used (2 seconds
for a walk-over line) and then to stop.

**Guards against loops and early give-ups:**
- a candidate that does not work out (nav fails, the press moves nothing, the key was not picked
  up) is not chosen again for that goal; the next candidate is tried;
- a goal whose candidates all fail is marked failed for the rest of the level, never pushed
  again, and the goal below looks for another way;
- a goal already on the stack is never pushed again (a key behind its own door);
- at most 8 goals deep; one sector is triggered at most 3 times a level (a door that closes again
  before the player gets there);
- a failed GO TO EXIT is not added again: no retrying every tic.

**Clicks:** `K_Brain_RequestGoto(x, y)` (from the console) rejects clicks outside a level or off the
floor, with a log line, and pushes GO TO (x, y) on top (a new click replaces one still being gone
to); it is planned at once, so the route shows in viewer mode too. When it ends on an otherwise
empty stack, GO TO EXIT starts again with the level's failures forgotten: the player may now
stand somewhere that changes things.

**Log and console.** Every push, choice, done and failure goes to the log, e.g. `GO TO EXIT: the
exit switch at line 495 is blocked by a door at line 350 (600,1528) that needs the blue key -
added GET BLUE KEY`, `USE SWITCH line 101 (tag 5) done - sector 30 moved (floor 48, ceiling
124)`, `USE SWITCH line 498 (tag 17) done - the way on is open, or opening (lift)`, `GO TO EXIT
done - level finished in 8.0 s through the normal exit`. The status line
shows the top goal (with waypoints left while driving); the console's Priorities tab shows the
whole stack, top first, and the goals that failed this level in red. `K_Brain_Reset()` empties
the stack on every level load and puts GO TO EXIT back under `-autoplay`.

Later: temporary goals pushed in front of the chain (get a medikit), scores, and danger in the
candidate scoring (a guarded medikit loses to a lonely one far away).

### 3.6 `K_BuildTiccmd` - command injection (H6)

Called at the end of `G_BuildTiccmd`, after crispy's fliplevels and lowres handling, so Karp writes
final, world-space values into the command that is about to be queued and sent. If the gate fails,
the human command is left untouched (intermissions, menus and so on behave normally). Otherwise:
1. Compute the command (`K_Nav_BuildCommand(maketic)`, prediction included); nav records it in its
   ring of sent commands.
2. Always write `forwardmove`, `sidemove` and `angleturn`; set `lookfly`, `arti`, `lookdir` to 0.
3. If `cmd->buttons & BT_SPECIAL` (pause/unpause, save), keep `buttons`; otherwise clear them.
   Movement is still overwritten in that case, so no human movement leaks through.
4. If `lowres_turn` (demo recording without `-longtics`), nav has already rounded the turn to a
   multiple of 256 (section 3.4).
5. Set `localview.ticangleturn = cmd->angleturn`, the same thing crispy's keyboard turning does
   (`g_game.c:898-900`), so the renderer interpolates the turn. Like crispy's keyboard turning,
   this is a single slot: if several commands are built before a tic runs, the view interpolation
   can be briefly off. That is cosmetic only.
6. Leave `consistancy` and `chatchar` alone.

`G_BuildTiccmd` already zeroes `localview.angle/rawangle` (`g_game.c:918-919`), and H7 stops
`G_PrepTiccmd` from adding per-frame mouse turn while the bot drives. The mouse stays enabled in
the config; Karp simply ignores it while it owns the controls.

### 3.7 `k_console.cpp` - Karp Console

- **Window:** created in `K_PostVideoInit()`, **only with `-karpconsole`**:
  1. `SDL_CreateWindow("Karp Console", ..., SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN)`.
  2. `SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED)` **without** `PRESENTVSYNC`. The game
     renderer already vsyncs, and a second vsync would halve the frame rate. Fall back to software.
  3. Place it (below), `SDL_ShowWindow`, then `SDL_RaiseWindow` on the game window, so menu keys
     reach the game at startup.
- **Placement:** the game window is **never moved or resized**.
  1. **Remembered placement first.** If `karp.cfg` holds a console rect, create the window at it.
     SDL2 window coordinates span the whole desktop, so the saved x/y alone put it back on the same
     monitor. The rect is used only if its title-bar strip (top 32 px) overlaps some display's
     usable bounds by at least 64x16 px, so the window can always be grabbed. Otherwise (monitor
     unplugged, layout changed) log it and fall through to step 2. If the saved state was
     maximized, call `SDL_MaximizeWindow` after showing it, which maximizes it on that monitor.
  2. **Default** (first run or unusable saved rect): if the game is windowed and there is room to
     its right on the same display (game window via `I_GetWindow()`, display usable bounds), open
     there; otherwise open centred on that display. The default (960x720) and minimum (480x360)
     sizes are multiplied by the display's scale factor, so the console looks the same size at
     150% scaling as at 100%.

  A fullscreen game may cover the console. The user then switches to windowed mode (Alt+Enter),
  Alt+Tabs to the console, or drags it to another display, where it will reopen next time.
- **Remembering placement:** polled every console frame rather than driven by window events.
  While a window is being maximized SDL can report the new size before the maximized flag, but
  between event pumps the two always agree. The rect is stored only while the window is not
  maximized, so the restored size is the normal one; the maximized flag is stored separately.
  Each change marks the settings dirty. `K_Frame` writes `karp.cfg` once the window has been
  still for 1 second, so a crash or a killed process still keeps the last placement;
  `K_Shutdown` writes any remaining change. Hiding the console keeps the last rect.
- **ImGui setup:**
  - backends `imgui_impl_sdl2` + `imgui_impl_sdlrenderer2`;
  - `io.IniFilename = NULL`;
  - `ImGuiConfigFlags_NoMouseCursorChange` (the cursor is process-global; leave it to the game);
  - `ImGui_ImplSDL2_SetGamepadMode(ImGui_ImplSDL2_GamepadMode_Manual, nullptr, 0)` (do not open
    the game's controllers);
  - font scaled by `ImGui_ImplSDL2_GetContentScaleForWindow` (the exe is DPI-aware);
  - in `K_Frame`: `if (io.WantTextInput && !SDL_IsTextInputActive()) SDL_StartTextInput();`.

  The backend sets two process-wide hints: `MOUSE_AUTO_CAPTURE=0` and
  `MOUSE_FOCUS_CLICKTHROUGH=1`. The second means the click that focuses the game window is also
  delivered to the game. That is harmless under `-autoplay` (the bot clears `buttons`); in viewer
  mode it can fire one shot. Acceptable for stage 1.
- **Redraw:** `K_Frame()` renders at most ~60 Hz (`SDL_GetTicks`), so it never steals much frame
  time. Events arrive at tic rate (35 Hz), which is fine for a debug UI.
- **Layout:**
  ```
  +----------------------------------------------------------+
  | GO TO (1024, -320) - 14 waypoints, 812 units             |  status line (K_Brain_Status)
  +--------------------------------------+-------------------+
  |                                      | [Priorities][Bot] |
  |            map canvas                | [Map]             |
  |                                      |                   |
  +--------------------------------------+-------------------+
  | 00:41.2 BRAIN  new priority GOTO (1024,-320) - user click |  rolling log pane
  | 00:41.2 NAV    path found: 23 cells, 1840 units, ...      |  (always visible,
  | 00:43.9 NAV    off path in cell 211 - replanning          |   resizable splitter)
  +----------------------------------------------------------+
  ```
- **Map canvas** (ImDrawList):
  - Map-to-screen transform with zoom (mouse wheel, around the cursor), pan (right or middle
    drag), a "Fit" button, and Y flipped like the automap. The view fits the map automatically
    whenever a new level loads.
  - "Follow player" toggle keeps the player centred; panning turns it off.
  - Map coordinates under the cursor, shown in the bottom-left corner.
  - Walls colored like the automap (`am_map.c:1736-1859`): one-sided red, floor-height change
    brown, ceiling-height change yellow, plain two-sided dim gray (toggle).
  - Overlays (checkboxes): floor cells (filled, coloured by subsector), centroids (red where the
    engine disagrees), the navigation graph (portals and links: green open both ways, yellow one
    way with an arrow, red blocked).
  - The route, whenever there is a goal: its cells faintly filled, the line to follow with a dot
    per waypoint, and a goal ring. Blue line and green ring when the route is open; orange line,
    the blocking crossing as a thick red line, and a red ring when it is blocked. A teleport is a
    dashed purple line. While the bot drives, a white ring marks the waypoint it is driving to.
  - Player arrow (live position and heading).
  - Hover tooltip: subsector # (and which of its cells), sector #, live floor/ceiling, area,
    neighbours by crossing ("4 neighbours: 3 open, 1 step too high").
  - **Left click -> `K_Brain_RequestGoto(x, y)`**; right or middle drag pans. The click that
    focuses the console also counts.
- **Side tabs:**
  - `Priorities`: the goal stack, top (worked on) first, each with what it is going for or waiting
    on, and the goals that failed this level in red (3.5). Later: a Cancel button.
  - `Bot`: position, facing, current cell; the route's goal, result, cells, waypoints, length and
    search time, with the reason when it is blocked; driving state, speed, waypoint "n of N",
    distance left, time, replans, prediction error, the reason when it failed; and sliders for the
    controller settings (top speed, braking, waypoint radius, turn rate).
  - `Map`: load stats and sanity-check results.
- **Rolling log pane** (bottom, always visible; data from 3.8):
  - newest lines at the bottom; auto-scrolls unless the user has scrolled up (then shows a
    "jump to latest" button);
  - each line shows level time, category and message, colored by category, with warnings and
    errors highlighted;
  - per-category toggles, a text filter, Clear and Copy buttons.
- **Close:** closing the console hides it until the next run; the game keeps running.

### 3.8 `k_log.c` - rolling log

The log is the bot's "thought stream": what it decided and why, not a per-tic dump.
- Ring buffer of ~2000 entries: `{ leveltime, category, level, text }`.
- Categories: `SYS` (startup, config, windows), `MAP` (load stats, sanity checks), `BRAIN`
  (priority changes and the reasons for them), `NAV` (paths, replans, stuck, arrival), `CTRL`
  (controller details, off by default). Later stages add their own, e.g. `COMBAT`, `ITEMS`.
- API: `K_Log(category, level, fmt, ...)`.
- **Log on state changes only**, never every tic. Identical consecutive lines collapse into one
  with a repeat counter (`... (x12)`), so a stuck loop cannot flood the pane.
- Stage 1 messages, for example:
  - `BRAIN new priority GOTO (1024,-320) - user click`
  - `BRAIN priority GOTO done - arrived in 142 tics`
  - `BRAIN priority GOTO failed - no path: a step 64 units high at line 312 (1024,-320) - the
    player climbs at most 24`
  - `NAV path found: 23 cells, 1840 units, 25 waypoints - 0.05 ms, 112 nodes searched`
  - `NAV off path (in cell 211) - replanning`
  - `NAV stuck at (512,-96) for 35 tics - replanning (2/3)`
  - `MAP E1M1 graph: 425 portals, 0 holes (0 units), 0 isolated, 1 components (largest 238 of
    238 cells), 0.8 ms`
  - `BRAIN click at (3000,4000) ignored - not on any floor`
- Sinks: the console pane, stdout, and `karp.log` in crispy's config directory (`configdir`, the
  exe folder on Windows; truncated each run), so a session can be reviewed after the game exits.

Later stages will write their decisions into the same stream, e.g. "looking for blue key",
"avoiding missile", "low health - going for medkit".

### 3.9 `k_settings.c` - Karp's own settings file

- File: `karp.cfg` in crispy's config directory (`configdir`, `src/m_config.h:41`; set at
  `d_main.c:1658`, before the console opens). It is owned and written only by Karp, and crispy never
  reads it. `crispy-doom.cfg` is never touched.
- Format: plain text, one `key value` per line, unknown keys kept and ignored, so newer and older
  builds can share the file. Stage 1 keys:
  ```
  console_x          1930
  console_y          40
  console_w          960
  console_h          720
  console_maximized  0
  ```
- API: `K_Settings_GetInt(key, default)`, `K_Settings_SetInt(key, value)` (marks the file dirty
  and returns non-zero only if the value actually changed), `K_Settings_SaveIfDirty()`. The file
  is loaded on first use, so nothing can overwrite it before it has been read.
- Writes go to `karp.cfg.tmp`, then replace `karp.cfg`, so a crash mid-write cannot corrupt it.
- Later console preferences (log pane height, overlay toggles, category filters) go in the same
  file.

## 4. Milestones

Each milestone builds and runs; the user reviews and commits between milestones.

1. **M0 Skeleton** - vendor ImGui, `karp/CMakeLists.txt`, the two top-level CMake lines, `karp.c`
   with `K_EnableAutoplay`/`K_EnableConsole`, `k_log.c` (stdout + `karp.log`), H1.
   Done when: the build passes, `-autoplay` writes a log line, and a normal run is unchanged.
2. **M1 Console window** - H3, H8, H9, event filter, `k_settings`, remembered placement, console
   with status line and rolling log pane.
   Done when:
   - without `-karpconsole` no second window appears;
   - the console reopens on the same monitor, at the same position and size, after a restart;
   - typing in the console does not affect the game;
   - the game's X opens the quit prompt;
   - Alt+Enter still toggles fullscreen;
   - startup log lines show in the pane.
3. **M2 Map view** - H4, `k_map` line snapshot, automap-style drawing, zoom/pan, live player arrow.
4. **M3 Subsector polygons** - polygons, centroids, sanity checks (logged), overlay, hover info.
5. **M4 Nav graph** - portals, live passability, graph overlay.
6. **M5 Pathfinder** - A*, `k_nav` planning, GOTO from a click; the console draws the route and
   the Bot tab shows it, the log says why when there is none (no movement yet).
7. **M6 Driving** - H2, H5, H6, H7, prediction, controller, arrival, replanning, stuck detection.
   **How we proceed from here:** play the maps one by one with the bot and fix whatever stops it
   from finishing them, rather than perfecting each feature first. That gets a "not yet perfect,
   but working" bot through the levels; the follow-ups below are the known candidates. We start
   with Doom II (more line types; Doom is mostly a subset), with `-nomonsters`.

   Playthrough log (Doom II):
   - MAP01: finished in 8.0 s (exit door, exit switch). Fixes it took: the closed crossing ahead
     is looked for from the player's route cell on, not only at it; the spot for working a door
     is in front of the middle of its line, not of a portal at the doorway's edge.
   - MAP02: needs keys and switches, which led to the goal stack (3.5): the bot pressed the
     switches for two doors (tags 5 and 8), found that the red bars need the red key, and went
     for it. Narrow gaps (between the bars, and a 32-unit passage beside a closed block) and
     wall corners it clipped led to the clearance checks (3.2.1). It then stops at the red key:
     the key is reached by a **running jump** (follow-up 7). Skipped for now, to come back to.
   - MAP03: the exit is reached through a **teleporter** (line 418, a walk-over teleport to sector
     7), which led to teleporters (follow-up 8). A run on Doom E4M3 (by accident) showed the bot
     teleporting and carrying on, until it got stuck on a torch (follow-up 3). On MAP03 the
     teleporter sits in a wall niche 112 units up, reached once switch 431 raises the strip in
     front of it; the switch is behind the blue door. The bot works that chain out, but the blue
     key looks out of reach behind "a gap narrower than the player" at the foot of a staircase
     (line 98). That gap is false: the steps are 8 units high but only 16 deep, and the clearance
     check judges every line near a crossing from the floor the player starts on (32), so the
     second step (64) counts as a wall 16 units from the first riser. In the engine the player
     stands on the highest floor under its box: once on the first step (56), the next one is only
     8 higher. Fixed: lines wholly past a crossing are judged from the higher of its two floors
     (3.2.1). The bot then took the blue key, rode the fast lift, pressed switch 431 and headed
     along the raised strip for the teleporter, where it drifted with its centre a unit off the
     strip: still standing on it, but Karp placed it in the cell below, from which nothing was
     reachable. Fixed: the player's cell is the one it stands on (3.2). Finished in 36.3 s:
     blue key, switch 431, teleporter, switches 445 and 498, red key, lift, red door, exit switch.
   - MAP04: the exit is behind bars opened by red key switches, and the red key lies on
     **stepping stones**: pillars 64 high (sectors 38, 42, 44) with 24-unit gaps of floor 0
     between them. The first pillar is a lift, lowered by walking over a 16x16 box elsewhere
     (sector 78, lines 408 and 409, walk-over lift, tag 15). Gaps narrower than the player need
     no jump, but the graph has no links between cells that share no border, so a route over the
     lift never reaches the key and the bot never goes for the box (follow-up 7). Skipped for
     now, to come back to.
   - MAP05: finished in 39.0 s (two keys, five walk-over lines for lifts and a rising floor, two
     switches, four doors, exit switch). Fixes it took (3.2, 3.4, 3.5):
     - a passage whose ceiling is level with the floor in front of it looked too narrow, with
       nothing to fix it; the floor in front is a lift that takes the player down to it, so for
       a gap too narrow the brain now also tries the sectors on either side of the crossing;
     - pressing a switch, the bot's box touched an 8-unit ledge in front of the door, which the
       standing cell from MAP03 then picked, and every way out of it was too narrow: the cell
       under the box now counts only when more than a step above the one under the centre;
     - the red key lies on an impassable line: the bot goes to a pickup spot beside it;
     - a walk-over line was "crossed" by going round its end: when the route does not cross the
       line, the bot walks up to it first.
   - MAP06: the blue key is reached by a **jump** (follow-up 7). Skipped for now, to come back to.
   - MAP07: a **boss map**: the way to the exit opens only when monsters die (follow-up 10), so
     with `-nomonsters` it cannot be finished at all. Skipped until combat.
   - MAP08: the way to the exit is over ledges reached by **jumps** (they start lowered and rise
     during play; follow-up 7). Skipped for now, to come back to.
   - MAP09: another **jump**, found by the user playing it: from a lift at about (700,1647)
     (sector 12) across to about (380,1444) (sector 6, floor -64). Without it the bot sees no
     way on and chases switches it cannot reach. Skipped for now, to come back to.
   - MAP10: finished in 45.4 s with no changes (yellow key, blue key, nine doors, exit switch).
   - MAP11: the exit area (the north part of the central circle) is reached by a **jump**, per
     the user; the bot sees only a 296-unit step at line 796. Skipped for now, to come back to.
   - MAP12: finished in 84.9 s (yellow and blue keys, a teleporter, eight switches, perpetual
     lifts, exit line). Fixes it took (3.2.1, 3.4, 3.5):
     - a diagonal blue door between two short jambs on one line looked like a closed sector:
       portals are now split per line, which also settles the impassable-lines TODO;
     - the bot lowered a lift first and fetched the yellow key meanwhile, the lift going back up:
       keys first;
     - the exit area is reached over perpetual lifts, judged as steps too high: lifts that come
       down by themselves are waited for, ridden up and ridden down;
     - USE TRIGGER for a door whose one-shot switch was used already: only sectors some line
       still moves.
     The new key order sent MAP03's bot out of a switch alcove past a pillar, where it stuck
     against the corner: the controller now slides along walls (3.4).
   - **Setting off right after a press** (3.5): the bot no longer stands at a switch until what
     it moved has stopped, but heads for the door, lift or rising floor at once and waits there.
     Times: MAP03 40.0 -> 34.2 s, MAP05 33.0 -> 30.7 s, MAP12 84.9 -> 77.1 s (MAP01 and MAP10
     have no switches on the way). It took three more changes: a door already opening counts as
     open for the way out of it (MAP12's route went on through a door still rising), rides work
     on any rising floor and are judged for width on arrival (MAP03's strip), and a teleporter
     reached by a ride waits for the ride first.

   **Doors and lifts** (manual ones) came first, ahead of the follow-ups below (sections 3.2.1,
   3.4). The edge cases noted for them: doors move at different speeds and lifts can take long,
   so the bot waits for as long as the sector moves; a door may not open high enough, and how
   high depends on the neighbouring ceilings, which can move, so the open height is judged live
   and a door that falls short makes the route replan. Remote doors and switches come later.

   **M6 follow-ups**, each in its own commit and re-measured with the drive test (section 3.4):
   1. **Narrow gaps.** Done (3.2.1): a crossing narrower than 34 units is "too narrow" (32 proved
      too tight, as expected), and waypoints keep 24 units from obstacles where the portal allows.
   2. **Impassable lines** (`ML_BLOCKING`). Done: the clearance check treats them as walls, and
      portals are split per line (3.2.1), so one along several collinear lines no longer misses
      an impassable one among them.
   3. **Solid things.** Torches, lamps, barrels, columns, trees and the like block the player but
      are not part of the geometry. Read the solid non-monster things at level load and block the
      parts of portals and cells they cover, widened by the player's radius; barrels can be
      destroyed, so check those live.
   4. **Downhill and drops.** Going down stairs the player keeps gaining speed: each step down
      leaves it briefly airborne, with no friction and no control, so it overshoots the target,
      and next to a pit that can mean falling in. Slow down earlier for crossings that drop
      (like a corner), and brake harder where it matters.
   5. **Narrow paths.** Some maps need narrow walkways and bridges, e.g. Doom II MAP24's long
      ledges only 4-8 units wide over the nukage. Entering a narrow portal or cell, slow down
      enough to stay on it. Doom II MAP03's raised strip to the teleporter is exactly the
      player's width: once the bot drifted a unit off it (and still stood on it), once it fell
      off and had to go round.
   6. **Stuck recovery.** Before replanning when stuck, block the crossing ahead for a while, so
      the new route really differs. If the player stays stuck too long, try semi-random moves in
      different directions, strafing included, to work free.
   7. **Running jumps.** Some maps can only be finished by running off a ledge at full speed and
      clearing a gap to a floor on the other side (at least one Doom II map; which one will turn up
      in playtesting). The graph then needs "jump" links between cells that share no border,
      where the gap, the drop and the run-up allow it: the player must reach top speed before the
      edge (16.7 units/tic running, about 21 when strafe-running), falls while it flies, and can
      land only on a floor at most 24 units above where it is by then. Gaps narrower than the
      player need no jump at all: its floor is the highest one under its 32x32 box, so it walks
      across. Following a jump link means a straight, full-speed run-up and no braking. Doom II
      MAP02's red key needs one.
      **Gap links** are the simpler half: two cells whose edges face each other across a gap
      narrower than the player (say up to 28 units, leaving margin), overlapping along the edge,
      with nothing in between that stops the player and a lower floor there. Judged live like a
      portal (far floor within 24, headroom; a lift that is down rides up first), crossed
      straight over the middle of the overlap without cutting corners. Doom II MAP04's red key
      needs them: stepping stones with 24-unit gaps, one overlapping the next by only 8 units.
      By the user's choice, gaps and jumps were to come **last**, after the other follow-ups.
      But six of the first eleven Doom II maps need them (MAP02, MAP04, MAP06, MAP08, MAP09,
      MAP11), so they will probably have to come sooner.
   8. **Teleporters.** Done (3.2.1, 3.3, 3.4): crossing a teleport line from its front side leads
      to the destination, judged live; the pathfinder's heuristic allows for the shortcut, and nav
      runs across the line and carries on from the destination, knowing the 18-tic freeze. Not
      yet: a route that only passes near a teleport line can still clip it by accident.
   9. **Walk-over lifts and timing.** A lift lowered by crossing a line (Doom II MAP03 has fast
      ones, special 120) goes down, waits 3 seconds and comes back up, so the bot must head for it
      right after crossing: finish the lower-the-lift goal as soon as the lift starts moving, and
      treat a lift on its way down, or waiting at the bottom, as a crossing to wait at (like a door
      that is already opening). Missing the window makes it cross the line again (3 times at most).
      Done (3.2.1, 3.5): a lift on its way down, or a perpetual one, is waited for, and a trigger
      goal ends as soon as the way it was pushed for opens or starts opening, so the bot heads
      for the lift right away.
   10. **Boss-death triggers** (needs monsters and combat, so a later stage). Some levels move
       sectors when the last monster of a kind dies, not from any line (`A_BossDeath` and
       `A_KeenDie` in `p_enemy.c`):
       - Doom II MAP07: the last Mancubus (`MT_FATSO`) lowers the sectors tagged 666 to the
         lowest neighbouring floor; the last Arachnotron (`MT_BABY`) raises those tagged 667
         (`raiseToTexture`). crispy applies the same rule to some Master Levels maps
         (`P_CheckMapTag666`).
       - Doom II MAP32: when the last Commander Keen dies, the door tagged 666 opens.
       - Doom: E1M8 (the last Baron of Hell lowers tag 666), E4M6 (the last Cyberdemon
         blaze-opens door 666), E4M8 (the last Spider Mastermind lowers tag 666); E2M8 and E3M8
         end the level when the boss dies. Doom II MAP30 ends when the Icon of Sin's brain dies.
       The brain would need a goal like KILL ALL <kind>, pushed when a route is blocked by a
       sector only such a death moves. Without monsters those levels cannot be finished.
8. **M7 Priorities tab** - the goal stack shown in the console is done (3.5, 3.7); still to come:
   history, cancel, polish.

## 5. Verification

- Build: VS Code task **build crispy-doom** (Release x64, output in `build/src/Release/`).
- Run (Doom II):
  `build\src\Release\crispy-doom.exe -iwad $env:DOOM_IWAD -warp 01 -nomonsters -autoplay -karpconsole`
  (Doom 1: `-warp 1 1`).
- Regression (no Karp params): no console window, mouse grabbed as usual, closing the window
  behaves as before.
- `-autoplay` alone: no console window; log lines in stdout and `karp.log`.
- `-karpconsole` alone: map view works, a click draws a path, the human keeps full control.
- Fullscreen with `-karpconsole`: Alt+Tab reaches the console, Alt+Enter toggles windowed mode, and
  the console can be dragged to a second display.
- Config: copy `crispy-doom.cfg` before an `-autoplay -karpconsole` session in which no settings
  are changed, then diff it after quitting. Expect no difference.
- Console placement:
  - drag the console to monitor 2, resize it, quit, relaunch: it reopens on monitor 2 at the same
    position and size;
  - maximize it on monitor 2, quit, relaunch: it reopens maximized on monitor 2, and un-maximizing
    restores the previous size;
  - move it, then kill the process (Task Manager) a few seconds later: the new placement survives;
  - edit `karp.cfg` to an off-screen position (or disconnect monitor 2): it falls back to the
    default placement with a log line.
- Map checks: walls match the in-game automap (Tab, with IDDT). The Map tab and log show 0
  degenerate polygons and ~0 centroid mismatches on E1M1, MAP01, and a PWAD built with ZDBSP
  extended nodes.
- Behaviour checks:
  - click across an open room: the bot runs there and stops within ~16 units without overshooting;
  - click up a staircase: the path climbs the steps;
  - click on a ledge higher than 24 units: the path goes around, or "no path" if it is unreachable;
  - drop off a ledge, then click back above it: the path avoids the unclimbable step;
  - click in the void: rejected with a log line;
  - click a new target while moving: immediate replan;
  - pause (Pause key) and open the menu (Esc): the bot stops, then resumes cleanly;
  - every decision above shows up in the rolling log and in `karp.log`.
- Input-only check (proves the bot acts purely through commands): record a GOTO run with
  `-record test -longtics`, then play it back **without** `-autoplay` (`-playdemo test`). Playback
  must follow the same route and end at the same spot. A demo contains only ticcmds, so this is
  the same guarantee a netgame needs.

## 6. Out of scope (later stages)

Monsters and combat; remote doors and lifts, switches, walk-over triggers and collecting keys
(manual doors and lifts work, section 3.2.1; the rest builds on the same live crossings); item
priorities (medkits, ammo);
path smoothing with player radius (beyond the M6 follow-ups); damaging-floor costs; lifting the
netgame guard; Heretic/Hexen.

### 6.1 Ideas for a late stage

- **Navigation checkpoints (TODO).** Spend time at map load precomputing "checkpoints": points
  that help the bot find its way through complex maps. Routes between them are tested and
  confirmed in advance (how is open; for example by simulating the movement, or by remembering
  legs that worked in play). When the bot plans a route, it checks for checkpoints that could
  guide it and **tries** to go through them, but they are never obligatory. The aim is not to save
  processing power (there is plenty) but better routes: from here to there it can be better to
  pass four confirmed checkpoints than to take a shorter route through a complex maze, along
  narrow ledges or past awkward obstacles.
