# Karp - Stage 1 Plan: "Go To Location"

> Status: approved; M0-M6 implemented, M6 follow-ups next. Work proceeds milestone by milestone
> (section 4).
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
    k_console.h / k_console.cpp  Karp Console (SDL window, ImGui, map canvas, tabs, log pane)
  third_party/imgui/          Dear ImGui v1.93.0 (latest release at plan time): core files,
                              backends/imgui_impl_sdl2.*, backends/imgui_impl_sdlrenderer2.*,
                              LICENSE.txt, VERSION
```

### 2.2 Engine hooks (complete list)

| # | Where | Hook | Purpose |
|---|---|---|---|
| H1 | `src/doom/d_main.c` after the `-warp` block (:2327) | Two `//!` doc blocks (`@category game`) + `if (M_CheckParm("-autoplay")) K_EnableAutoplay();` and `if (M_CheckParm("-karpconsole")) K_EnableConsole();` | Turn the bot and/or the console on. Warn if `-autoplay` is used without `-warp` (the bot still activates on any level). |
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
  | `-autoplay -karpconsole` | Full stage 1: map view, click a target, the bot drives there |
  | `-autoplay` | The bot runs and logs to stdout and `karp.log`. In stage 1 nothing sets a target without the console, so it stays IDLE; later stages choose priorities on their own. |
  | `-karpconsole` | Viewer: the human plays normally. Clicks plan and draw a path, but nothing drives the player. Useful for debugging the map reader and pathfinder. |
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
- `K_Map_GetPlayer()`: the local player's live position and facing (read-only).
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
linedef it runs along, or -1 when both cells are in the same sector.

**Passability** of a portal, leaving a given cell, is evaluated **live** (`K_Graph_CanCross`). It
is cheap, and it makes doors and lifts work naturally in later stages. It mirrors
`P_TryMove`/`PIT_CheckLine` (`src/doom/p_map.c:212-285, 564-631`), with the sector of each cell's
subsector:
- blocked if the crossed linedef is one-sided (a map error) or has `ML_BLOCKING`;
- blocked if `floor(B) - floor(A) > 24` (step-up). This is directional: dropping down is always
  fine, because players have `MF_DROPOFF`;
- blocked if `min(ceil A, ceil B) - max(floor A, floor B) < 56` or `ceil(B) - floor(B) < 56`
  (closed doors).

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
- **Start and goal cells** via `K_Map_CellAt`. A player standing on a hole in the mesh starts from
  the nearest cell within 64 units (`K_Map_NearestCell`).
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
- **Blocked crossings are not skipped but cost a large penalty** (10^7 each, more than any open
  route). One search then gives the shortest open route if there is one, and otherwise the route
  with the fewest blocked crossings, whose first blocked crossing tells why the goal cannot be
  reached. Crossings are judged live (`K_Graph_CanCross`).
- **Legs along a wall.** When a route enters and leaves a cell by two portals on the same edge,
  the leg between them runs along that edge. That is common and harmless along a partition line,
  where the edge is all portals, but between two doorways in one wall it would scrape the wall. A
  leg that runs along a stretch of border that is not a portal (sampled at 1/4, 1/2 and 3/4) goes
  through the cell's centroid instead.
- **Output** (`k_path_t`): the result, the route's cells and the portals between them (for
  progress tracking in M6), the points to follow (start, portal middles, the occasional centroid,
  goal), the length, the first blocked crossing with its kind and height, nodes searched and time.
- **Results:** `FOUND`; `BLOCKED` (every route crosses something the player cannot cross now: a
  step too high, a closed door, a blocking line); `UNCONNECTED` (nothing connects them, e.g. an
  area reached only by teleporter); `NO_START` / `NO_GOAL` (not on any floor).
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
- **Progress:** the player's cell is looked up in the route's cells. Moving further along the
  route (perhaps having cut a corner) moves the target waypoint to the first one inside that
  cell, never back; being pushed back to an earlier route cell sets the target to that cell's way
  out again. The route remembers which cell each leg runs through (`k_path_t.leg_cells`) for
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
4. **Heading:** turn toward the desired velocity, at most 11.25 degrees per tic (2048 angleturn
   units, tunable). Cosmetic for now; later stages need it for aiming and using. When recording a
   demo without `-longtics`, the turn is rounded to a multiple of 256; no remainder needs carrying,
   since the next command aims from the actual angle again.
5. Idle or failed: zero movement.

The controller settings are adjustable in the console's Bot tab, but not saved yet.

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

### 3.5 `k_brain.c` - priorities

- Stage 1 types: `IDLE`, `GOTO_LOCATION {x, y, ss}`.
- `k_priority_t { type, params, status (PENDING/ACTIVE/DONE/FAILED), score, created_tic, reason }`.
- One active priority. A console click replaces any current GOTO. DONE and FAILED fall back to
  IDLE, and the finished entry stays in a short history list.
- The interface is designed for later stages: each priority type has `evaluate()` (a score from the
  world state, e.g. low health -> medkit) and `update()` (tells nav where to go). The highest score
  wins.
- `K_Brain_RequestGoto(x, y)` is called from the console. It rejects clicks outside a level or
  off the floor (`K_Map_CellAt` returns -1), with a log line. Otherwise the click becomes the
  GOTO and nav plans it; if there is no route, the GOTO fails at once (back to IDLE) and the reason
  goes to the log and the status line. Without `-autoplay` (viewer mode) the GOTO is planned and
  drawn, but no command is ever written.
- `K_Brain_Tick()`, once per game tic while the bot owns the controls, ticks nav and ends the
  GOTO when nav reports arrival (`priority GOTO (x,y) done - arrived in 4.2 s`) or failure
  (`priority GOTO failed - stuck at (x,y)`).
- `K_Brain_Reset()` drops any GOTO on every level load (logged).
- `K_Brain_Status()` returns a one-line description of the current intent for the console status
  line, e.g. `GO TO (1024, -320) - 14 waypoints left, 812 units`, `IDLE - GO TO (1024, -320) done
  in 4.2 s`, `IDLE - GO TO (1024, -320) failed: <reason>`; in viewer mode `VIEWER - GO TO ... - 14
  waypoints, 812 units` (planned only).
- IDLE means zero movement (the bot owns controls).
- So far the brain holds just the current GOTO and how the last one ended; the priority list,
  statuses, history and scores arrive with the Priorities tab (M7).

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
    the blocking crossing as a thick red line, and a red ring when it is blocked. While the bot
    drives, a white ring marks the waypoint it is driving to.
  - Player arrow (live position and heading).
  - Hover tooltip: subsector # (and which of its cells), sector #, live floor/ceiling, area,
    neighbours by crossing ("4 neighbours: 3 open, 1 step too high").
  - **Left click -> `K_Brain_RequestGoto(x, y)`**; right or middle drag pans. The click that
    focuses the console also counts.
- **Side tabs:**
  - `Priorities`: active priority, status, history, Cancel button.
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
   **M6 follow-ups**, each in its own commit and re-measured with the drive test (section 3.4):
   1. **Narrow gaps.** Work out how wide each portal really is between walls and treat one
      narrower than the player as a new "too narrow" crossing. Start at 32 units (the player's
      width); getting the player lined up exactly enough to pass a 32-unit gap may prove too hard,
      so it may need to become ~34. Waypoints also move at least 16 units away from wall ends, so
      routes stop scraping door jambs.
   2. **Impassable lines** (`ML_BLOCKING`). A portal along one is already refused, but only by the
      one line the portal records: a portal along several collinear lines can miss an impassable
      one among them, and the clearance checks of follow-up 1 must treat impassable lines like
      walls (TODO in `K_Graph_CanCross`).
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
      enough to stay on it.
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
      across. Following a jump link means a straight, full-speed run-up and no braking.
8. **M7 Priorities tab** - IDLE/GOTO lifecycle, history, cancel; polish.

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

Monsters and combat; doors, lifts, switches and keys (the graph keeps sector references and
evaluates passability live, which is the foundation for these); item priorities (medkits, ammo);
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
