# Karp - Stage 1 Plan: "Go To Location"

> Status: approved; M0-M6 implemented, plus doors and lifts, GO TO EXIT, the goal stack and
> teleporters; now playing through Doom II map by map (section 4). Combat, the next stage, is
> planned in section 7.
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
    k_trace.h / k_trace.c     player trace: what a human driving does, in the log
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
  bbox, and the block whose square it lies under, if any. These are the nodes of the navigation
  graph (see below).
- `blocks[]`: the solid things that are not monsters (see below).
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

**Solid things (blocks).** A solid thing stops the player at the square of its radius plus the
player's, at any height (`PIT_CheckThing`; Karp supports vanilla levels only, so crispy's walk
over/under option stays off), and the engine does not slide the player along it as along a
wall. At level load every solid thing that is not a monster becomes a **block**: decorations
(torches, lamps, columns, trees, hanging bodies), barrels, voodoo dolls (`k_block_t`: its centre,
radius, and whether it is a barrel). Monsters (`MF_COUNTKILL`, lost souls) move about: they are
steered round, not built into the map. Each floor piece a block's square overlaps is cut into
the piece under the square and up to four round it (edge by edge round the square, what lies
beyond the edge is kept and the rest goes on), so the square is cut out like a pillar and
everything built for walls works round it: portals, narrow crossings, pinches, waypoints kept
clear (3.2.1). The cells under a square stay, though: a block is judged live
(`K_Map_BlockStands`, once a tic), by looking for a solid thing of its type within 4 units of
where it stood, in the blockmap, never by pointer (a removed thing's memory is used again). A
barrel that blows up is gone, and one pushed further off its square counts as gone too (then an
obstacle that moves, like a monster). `K_Map_CellBlock` gives the standing block over a cell;
crossing into such a cell is blocked (3.2.1). Blocks are bucketed in a 128-unit grid
(`K_Map_BlocksNear`). Doom II has 0-63 per map, most of them decorations; MAP23 has 208
barrels, and its cells go from 200 to 1,696.

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
- **Split** (informational): the floor is not convex, or holds a solid thing, and needs several
  cells.
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

**Cells too thin to stand in** (`K_Map_CellFloor`). The same rule makes some cells' own floors
unreachable: where every corner of a cell is within the box's reach of a two-sided line with a
floor more than a step above the cell's (the reach is convex, so the corners are enough), the box
is over that floor wherever the centre is in the cell, and the player stands on it there. Such a
cell takes that floor wherever a cell's floor is asked for: the player's cell, the crossings
(heights and width), the path's walls and nav's careful points. Walking onto it from the higher
floor is level, walking off its far side is a drop, and from its own floor there is no way in.
Doom II MAP25: the only ways off the start platform are a blood channel (272) ending in a niche 16
wide (sector 41, floor -72), and past the niche a teleporter pad on the niche's floor, entered
from behind (so it does not teleport), with a pit round it. Karp put the player down in the niche,
where the channel's edge was a wall 16 from line 523 into the pad, so every way out was too
narrow. Now the player walks off the channel's end, stays up on 272 over the niche and drops at
line 523, as in the engine (the user: a plain step off the ledge, nothing to jump). Only floors
that never move count, the cell's and the higher one (no tag: every line that moves a floor finds
it by its tag), so lifts are judged by their sectors' floors as ever: MAP12's perpetual lifts have
thin edges 64 below the switch alcoves beside them, and MAP05 a strip 16 deep in front of a raised
lift whose face is its switch; both broke when their cells stood on the higher floor. Only cells
at most 48 wide (32 by 32 turned 45 degrees, the widest the reach gets across a line) are looked
at. The console's hover info shows such a cell's floor ("too thin: stood on at ...").

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
  but when it gets there. Unless it crushes, a floor stops where the player's head meets the
  ceiling (its ceiling less 56), and a lift turns straight back down there (`T_PlatRaise`): Doom
  II MAP24's red key platform tops out 48 units under its ceiling, so with the player on it, it
  rises to 120 of its 128, turns back, waits 3 seconds at the bottom and comes up again, for as
  long as the player stays on. The way off is judged where the player first can step off (the
  floor beyond less 24), not at the top, since higher up there is only less headroom.
- **Lifts that turn back:** a lift also turns back for a monster or barrel at its edge that it
  would lift into a lower ceiling (anything shootable whose box touches the lift sector is
  lifted with it). Each time one turned back on the player before it could get off (3.4),
  riding that lift counts 5 seconds more as a wait, up to 100 times, still well under a blocked
  crossing: a route another way wins once it is worth it, but where there is none the lift stays
  the way, however often it turns back. A ride on it that works clears the count.
- **Lifts that come down by themselves:** a perpetual lift (53, 87; a stopped one has no
  thinker), or one on its way down. A lift too high now to step onto, or to fit over, is a lift
  crossing if its bottom works: wait for it (no press), step on. On one that is too high to fit
  under the way off, the crossing off is a lift ride: ride it down. The wait is worked out from
  the lift's state (`T_PlatRaise`: still to go up, the wait at the top, the way down). Doom II
  MAP12's exit area is reached over perpetual lifts that go up and down all the time.
- **Floors that come down:** the same for a floor beyond on its way down (`T_MoveFloor`, a switch
  or walk-over line set it going) to within 24 of the player's floor: a lift crossing, waited at,
  not a step; and off a floor lowering to where the way off fits, a ride down. Neither, nor a lift
  coming, is judged for width before it gets there: Doom II MAP02's switches 130 and 311 lower
  bars 16 wide across a water channel (a unit a tic, 2 s to a step), whose far faces stay walls
  16 units behind the crossing while they are up. So the brain's USE TRIGGER is done right after
  the press, and the bot waits at the bar, not at the switch (MAP02 4 s faster, MAP12 3.7 s).
- Standing in a door sector means its door is open, so crossings out of one are judged with the
  door's open height: a manual door's, and that of any door already opening (a switch opened it,
  and the route goes on through it before it is fully up). The clearance check takes them as
  open too. Going in is still a door crossing to wait at.
- Each also gives the time it is expected to take: stopping and pressing (~1 second), plus the
  door's travel at 2 units/tic (8 blazing), or for a lift the trip down and up at 4 (8) and its
  3-second wait at the bottom. The pathfinder charges that time as distance (16 units a tic), so
  an open route that is not much longer still wins.

Doors and lifts worked from elsewhere (remote switches, walk-over lines) are the brain's job: it
goes and works the line first (3.5).

**Solid things** (`K_CROSS_THING`). Crossing into a cell under a standing block (3.2) is blocked,
whatever the heights say: the player gets round a solid thing, never past it, until it is gone
(a barrel blown up). Nothing the brain can do fixes it yet (shooting barrels is to come), so the
pathfinder costs it as a crossing with no fix. Blocks are obstacles in every clearance check
below (narrow crossings, pinches, waypoints, `K_Graph_Clearance`), at any height, with the
distance measured as the engine collides two boxes: the larger of the x and y distances to the
block's square. So a gap between a thing and a wall, or between two things, is judged like any
other: Doom II MAP19's rows of blue torches, 32 apart across and 48 along, are walls (their
squares, grown by the player's radius, overlap); MAP23's row of barrels north of the start, 32
apart, leaves 12-unit gaps, a fence; its rows further down leave 44, a slalom the player gets
through. Chords also start at the corners of block squares on a cell's border, since a cell
along one side of a square has the square's corners partway along an edge.

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
the first one, the second one's riser looked like a wall 16 units from the crossing. A drop is no obstacle (the player can hang over it).
A closed manual door counts as open where the player opens it on the way (otherwise a door's
faces, 8 units apart, would narrow every crossing near a doorway). That is: where the player
stands in it (or, in steering, where its box is in it); for a door set in a wall (it has a
one-sided line), where it is within 17 units of every point on the crossing, locked or not, so
that, closed, it alone would leave no way through and the player gets there only through the open
door (one-way doors leave strips 8 and 16 units deep behind them on Doom II MAP16 and MAP03;
MAP16's blue door, locked, sits at the back of an alcove 16 deep: the door's own crossing says the
key is missing); and for a door set in a wall seen across a face that opens it (a door line, from
its front side) when the player has the key it needs, if any. There is no way past a door set in a
wall but through it, so whatever its closed faces narrow is on that way (MAP01: a light sector in
the 24-deep room before the exit door; MAP24: a 17-deep alcove before an exit door). Otherwise a
closed door is a block: a way past it that it narrows leads through it, as a door crossing that
gets it opened, or past it once it is opened from a face (the brain's fix for a narrow gap, 3.5).
MAP26's yellow-key pillar, a free-standing door 16 square with door lines all round, leaves 24
units on either side in a corridor 64 wide; counted open because it could be, it was squeezed
past, never opened, and the bot stuck against it. MAP16's red-key pillar, a door opened from two
of its four faces, leaves gaps exactly as wide as the player on either side, which first looked 90
units wide with the pillar "open", so the bot never went for the red key; MAP21's yellow bars, door
faces all round, leave a gap of 24 between two of them. MAP19's gate of three key bars (8, 16 and
8 wide) leaves gaps of 8: every crossing between two bars has both within 17 units, so the
"near all along" rule counted both open and the gaps looked 64 wide, which is why that rule is
for doors set in a wall only. Judged live, so bars that lower or open widen the gap. Not covered:
a door set in a wall at one end only, with floor on its other sides, could still be squeezed past.
`K_Graph_NarrowSectors` names the sectors that narrow a gap (the far side of the obstacle lines),
so the brain can look for a line that moves them, or a door face that opens them.

**Gaps inside cells** (`K_Graph_Pinched`). A gap can also be narrower than the player inside a
cell, between two crossings that are each wide enough: MAP21's yellow bars, a closed block beside
a wall mid-cell, leave 32 units (exactly the player, so no way through), with the crossings on
either side 24 units off. In a convex cell the narrowest way across between two obstacles on its
border starts at the end of one of them: a corner, or the end of a line partway along an edge. So
at level load each cell gets chords from each such point to the nearest point of every other edge,
and to each other such point, at most 35 units long (two clearances) and not along an edge; MAP21
has 83, the regression maps 53-247. A chord is judged like a crossing, live (once a tic): too
narrow if no point on it is 17 units from every obstacle, sampled every 2 units, and also 16 units
past its ends, since the player's box can hang over open floor beyond (else every thin cell beside
a wall would look like a gap). Going through a cell from one stretch of its border to another (a
portal, or a point: the start, the goal, a teleporter's destination) is pinched if a narrow chord
has them on opposite sides. A pinch is no crossing's, but it has sectors that narrow it, found as
for a narrow crossing from the obstacle lines near its chord (`K_Graph_PinchSectors`), and the
brain can open one of those (3.5): Doom II MAP11's fence of pillars 16 apart behind the red door
leaves gaps 24 long, each pinched inside its own cell, and walk-over lines open or lower every
other pillar for a while. Until that, the fence cost as a wall, so the bot planned straight up a
136-unit step from the nukage instead, and gave up.

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

#### 3.2.2 `k_jump.c` - jumps

Designed with the user: one model for gaps narrower than the player and for running jumps;
straight runs only (vanilla maps never need strafe-running); a margin, never a jump that needs
near-perfect conditions; found at level load, judged live.
- **The run.** A jump is a straight run from a standing start, checked with the engine's own
  movement replayed on Karp's data (nothing in the game is touched, 2.4): forward thrust (50, so
  1.5625 a tic) and friction (0.90625) on the ground only, momentum capped at 30 a tic and moved
  in halves above 15 (positive moves only, as the engine does), gravity of 1 a tic (2 the first),
  and the floor and ceiling under the 32x32 box from the sector under its centre and every
  two-sided line it straddles (`P_CheckPosition`). Walls, impassable lines and standing solid
  things stop a move, as do a step over 24 and headroom under 56 (`P_TryMove`); a move stopped by
  a line slides along it, as `P_SlideMove` does (traces from the box's leading corners, up to the
  wall less a fudge, the rest along it; else one axis, then the other), and the run fails only
  where the player cannot move at all. The player stands on the highest floor under its box, so
  a gap narrower than the player needs no speed, and a wider one is flown 32 units shorter than
  it is wide: Doom II MAP02's pit is 96 wide, flown 64. Sliding gets the box round a wall's
  corner: Doom II MAP09's floors 6 and 4 touch only at a corner of door 8, closed until line 250
  past floor 4 opens it; the player slides along the door's face and steps across (the user did,
  and so does the bot now).
- **Landed** means standing, box and centre, on a floor more than 24 above the lowest one the
  centre passed over: one it could not climb to from there. Whatever lies in between is crossed
  as the engine would: one pit, or (MAP14, the user's example) a sidewalk, a low fence and water.
  Landed is not enough: the run goes on braking, 2 tics coasting (the bot's commands for them
  were built in the air, with no thrust), then full thrust against the momentum, and must stop
  (under 1 a tic, or against a wall) before the box leaves the landing floor. Doom II MAP24: a
  jump from a platform over the nukage onto a walkway 16 wide, which a player braking slides
  along but does not leave. Touching down where it could not stop, it runs on, as the engine
  carries it (the thrust goes on): Doom II MAP06's jump from a lift to the blue key, 304 units
  flown over a pit and many sectors, hops off a pillar's top 16 square, level with the lift, 8
  units out.
- **Finding them**, at level load: off every portal the floor drops more than 24 across, from
  points every 48 units along it, rays straight out and 25 and 45 degrees to either side (MAP09's
  jump out of door 33 runs along its wall onto a ledge 48 degrees off to the side), each with as
  long a run-up as the takeoff floor allows behind the edge (up to 128 units; none at all may do:
  Doom II MAP08's strip of slime 17 wide behind the yellow door leaves 22 units to take off from),
  and again with a run-up of 8. The run-up may also come from floors below the takeoff, up to a
  step below, that the player only climbs from on the way: going up a step costs no speed. Doom
  II MAP14's way to the red key is a jump out of a window, off a sill 16 deep, over a courtyard,
  a low fence and water, with the run-up in the corridor before the window. And where the start
  6 units to one side fails and the other passes, the start is moved 6 units to the passing side
  and tried once more: that window's opening is 64 wide, its edge two portals sampled 16 off its
  middle, so every start put one side of the margin into the low sill beside it. Kept if the run lands in another cell, and still lands in the
  same sector with 85% of the thrust and from starts 6 units to either side; one per pair of
  cells, the shortest, so a gap that needs little speed is taken without first backing up (MAP08's
  pillars sink once stepped on, and backing up along one cost the time it gives). MAP02: 9 jumps
  in 1.1 ms; MAP24: 64 off 500 edges in 20 ms. (A rough count over the WAD before building it:
  about 40 candidates a map, at most 173 on MAP14, a few percent of the portals: no bloat.)
- **Long run-ups.** Where neither run-up gives a jump off a point, a long one is tried: up to 320
  units, over floors within a step of one another, up or down, kept with 90% of the thrust
  instead of 85% (the user's choice), the margin each jump keeps (`k_jump_t.margin`) and is judged
  with live. The run counts a step down with nothing lower passed over as more run-up, and so
  does nav (3.4). Doom II MAP20: the way to line 271 is a jump off the foot of a walkway that steps
  down from 208 to 160, 8 or 16 at a time, over 240 units of blood onto a ledge in a wall opening
  (floor 72, ceiling 200); it needs about 14 units a tic, which 128 units of level floor do not
  give. At full speed it has about 11% to spare, short of the 15% the usual margin asks (the user
  took it at a run, coming down the walkway from a teleporter). Only edges with no jump get one,
  so the jumps found before stay as they were. MAP20: 112 jumps, 41 more as if moved, 150 ms
  (113 before).
- **A fall is no jump.** A run that falls is not kept: walking off the ledge gets there too.
  MAP25's way off the start platform was first taken as drops past a cell with no room to stand
  in, kept as jumps; the user found them awkward (planned diagonally into a wall, landing only by
  sliding down it, where a plain step off the ledge does), and they doubled the jumps (MAP14 152
  to 303, the search up to 0.5 s on MAP24). Replaced by cells too thin to stand in (3.2), with
  which walking gets there.
- **Doors on the run-up.** While the player is still on the takeoff floor, manual doors count as
  open (at their open height): its box can overlap a door it came through, and standing there
  holds it open. MAP08's hop onto the first sinking pillar starts from a strip 8 wide behind the
  red door, itself 8 thick and closed at level load, so the box at every start overlapped the
  closed door. Past the takeoff floor, doors are as they are. The run-up's centre never crosses a
  door's side, though: it can start in a door, never behind one, since nav would not know to open
  the door on the way.
- **Again when a sector stops moving.** A door that opened or a floor that rose can make jumps the
  heights at level load did not (MAP08: the jump back from the yellow key's ledge, behind a door
  closed at load). Once a tic the graph notes the sectors with a mover; when one stops, the edges
  within 512 units of it are searched again (`K_Jump_FindNear`). Jumps already found stay as they
  are, new ones are added as portals after them, and the links are rebuilt ("sector 84 stopped
  moving: 1 new jumps").
- **As if a sector had moved.** That is too late when the brain must move the sector first to
  have the jump at all: Doom II MAP09's way to the yellow key takes three jumps that do not exist
  at level load. One lands on floor 6 once switch 126 raises it, one runs out of door 33 once
  switch 157 opens it, and one steps from the top of stairs that line 380 builds onto the sill of
  door 101, which switch 455 opens. So at level load, after the search above, each sector a line
  moves is searched round again (its edges within 512 units) as if it were where the line sends
  it (`K_Specials_MoverTarget`, from the special as the engine works it out: a door 4 under the
  lowest neighbouring ceiling, a floor to the next floor up, the lowest or highest neighbouring
  floor, by 24, 32 or 512, a step of stairs along the chain; not lifts, which come back, nor
  donuts or raising to a texture), with every door open (manual ones and those a line opens).
  Kept only if the run goes over that sector, and only between cells no jump joins yet: one that
  works as things are is better. Its start and landing cells are those the player stands on by the
  floors of the search (`K_Map_StandingCellOn`, the rule of `K_Map_StandingCell` with the heights
  as if moved), not the cells under its centre: Doom II MAP11's strip 20, between a closet and the
  nukage, is lowered by switch 615, and the jump off it to the walkway started at the strip's
  edge with its centre over the nukage, so it was a jump from the nukage's cell; the bot waded in
  to take it and missed it again and again. Such a jump records the sector, its heights then, and
  the first door it went through that is closed. It does not work as things are, so it is a
  blocked crossing (`K_CROSS_GAP`) whose fix moves the sector there, then opens that door (3.5).
  MAP09: 53 jumps, and 35 more once one of 25 sectors has moved, in 100 ms.
- **In the graph** a jump is a portal of its own after the shared stretches of border
  (`k_portal_t.jump`), linked from its run-up's cell only: `a` is the run-up's start, `b` the
  touchdown. Judged live, once a tic, by running it again with full and 85% thrust (heights
  move, a solid thing can stand in the way): `K_CROSS_JUMP` if it works, else `K_CROSS_GAP`, a
  blocked crossing the brain can fix: raise the takeoff floor back to where the jump was found,
  or lower the landing (MAP08: walk-over line 599 lowers the island the way back starts from into
  the slime, and switch 572 raises it again). But off a lift that is down, or on its way, and comes back up
  to the floor the jump was found on, it is a ride (`K_CROSS_RIDE`, the wait the lift's state
  gives): Doom II MAP06's lift is lowered by the walk-over line the player crosses to get on it,
  so the jump only works once it is back up (the user's description). Off a floor on its way down
  for good, it is a gap if the floor will be more than a step below where the jump starts by the
  time the player can get there (as the crow flies, at running speed): Doom II MAP13's walk-over
  line 902 lowers the ring round the red key's platform, and the bot planned to teleport into the
  pit, wait for the ring to come down within a step, get on and jump from it onto the platform,
  which only works while the ring is high. MAP08's sinking pillars, under 100 units ahead, still
  count. Nor is it a jump while the cell it starts from is more than a step below the floor it
  was found on (a ride off a lift coming back up, else a gap): the run from its start can land,
  its box standing on a higher floor next to that cell, but the player does not get up there.
  Doom II MAP24: the jump found as if line 332 had raised the western nukage pool starts on the
  strip beside it, and the bot waded through the pool to take it instead of keeping to the ledge
  (the user saw it); MAP28: the jump found as if line 402 had raised lift 82, a step onto its
  edge from the floor round it, and the bot pushed against the lift instead of calling it. It
  costs its distance plus 2 s (70 tics), so a walk not much
  longer wins (the user: jump where it saves a lot, or where nothing else gets there). The graph
  log and the Map tab count jumps; the overlay draws each dashed orange from the run-up's start
  to the touchdown, with a ring there, faint while it does not work.
- Not yet: with combat, clear the monsters near a jump first (the user: they stand at any height
  and can knock the player off).

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
- **Jumps** (3.2.2). A jump's portal is crossed from its run-up's cell only, into the landing
  cell: it costs the way to the run-up's start, the run to the touchdown and its 2 s, never less
  than the straight line, so the heuristic stays as it is. The node stands at the touchdown, and
  enters its cell there, a point (for the pinch check, as a teleporter's destination); the way
  out of the run-up's cell is its start. A jump that does not work now is a blocked crossing, with
  the penalty below.
- **Floors that hurt** (nukage, blood: specials 5, 7, 4, 16, 11; `K_Map_FloorDamage`, none under a
  radiation suit as far as it keeps the damage out). Walking across such a cell, and waiting in
  it for a door or lift, costs the health it takes at running speed, a point as much as 100 units
  of running: a short wade beats a long way round. If the route found would take more than half
  the player's health, the search runs again at 100000 a point, a tenth of a fixable blocked
  crossing (3.5): then any dry way wins, and so does fixing one first. A jump's flight costs
  nothing, the player is off the floor. Doom II MAP20: with no such cost, the bot went for line
  271 by the jump above, but walked to it for half a minute over the big blood floor and died
  (the user saw it: their own way, with more health and armour); the dry way needed a door
  opened by switch 73 first, and an open route always beats a blocked one. One cost alone did
  not do: at 100000 a point from the start, MAP11's bot raised a floor by two switches and two
  lift rides (24 s) to stay out of a little nukage, and MAP26 lost 11 s. The route's damage is in
  the log ("about 6 health off floors that hurt"). A stand-in, by the user's wish, until the bot
  knows its health, armour and suits; armour is not counted. A crossing's wait is charged where
  it is spent (`K_Graph_CrossingWait` gives the part past the crossing): a lift lowered by its
  switch is waited for in the cell it is called from, but its wait at the bottom and the ride
  up are spent aboard, and a teleporter's freeze at its destination. Doom II MAP29: lift 172 out
  of a courtyard whose floor hurts (special 16) had its whole round trip, about 190 tics, put
  down as courtyard, and the route to the exit read about 117 health; now about 43.
  Still counted in full, though: a wait nav spends on a dry floor (waiting dry, 3.4), and every
  wait from now, however long the player takes to get there, so MAP20's floor 26 is counted
  twice, once waited for in front and once ridden the last step down, and its route reads about
  245 health where the bot loses some 25. Counting less was tried and left out: a ride down
  counted from now (right, and MAP20 read 23) shifted MAP12 by a tic into a jump that worked,
  5.9 s worse after; counting what comes down by itself as waited for dry led the brain to the
  south leg in MAP29's north-east (by a tricky jump, a speedrunner's way, which failed; the
  middle leg is right, the user), 15 s and 40 health lost. To come with planning that knows when
  the player gets where.
- **Blocked crossings are not skipped but cost a large penalty**, more than any open route. One
  search then gives the shortest open route if there is one, and otherwise the route with the
  cheapest blocked crossings, whose first blocked crossing tells why the goal cannot be reached
  (with how many there are, and the cell it is crossed from). The penalty comes from the brain
  (`K_Path_SetBlockCost`): 10^6 for a crossing it knows how to get past (a key lies in the level, a
  line moves the sector), 10^8 for one it does not, so blocked routes run through fixable
  crossings. Crossings are judged live (`K_Graph_CanCross`).
- **Through a cell**: a search node is a portal crossed one way, so expanding it goes through its
  cell to the next portal; that way is pinched if a narrow chord lies across it (3.2.1). A pinch
  costs what the brain says, by where it is (`K_Path_SetPinchCost`): 10^6 if it can open a sector
  narrowing it, else 10^8. It is also reported like a blocked crossing, as "a gap narrower than
  the player at (x, y)".
- **Waypoints**: the route's line passes each portal at `K_Graph_PortalPoint` (3.2.1), not exactly
  at the middle the search costed, so it keeps clear of wall corners.
- **Legs along a wall.** When a route enters and leaves a cell by two portals on the same edge,
  the leg between them runs along that edge. That is common and harmless along a partition line,
  where the edge is all portals, but between two doorways in one wall it would scrape the wall. A
  leg that runs along a stretch of border that is not a way on (sampled at 1/4, 1/2 and 3/4) goes
  through the cell's centroid instead. Not a way on: a wall, a portal to a floor more than 24
  higher, or more than 24 lower, a ledge's edge the player would fall off, or a portal to a cell
  under a standing solid thing (the side of its square). On Doom II MAP16 both
  crossings of a landing lay on its open edge; followed at careful speed (3.4), the leg along it
  dropped the bot back to the start, and the route sent it up again, round and round.
- **Output** (`k_path_t`): the result, the route's cells and the portals between them (for
  progress tracking in M6), the points to follow (start, portal middles, the occasional centroid,
  goal), the length, the first blocked crossing with its kind and height, nodes searched and time.
  At a teleporter the points go on from the point on its line to one 32 units past it (so the
  player surely crosses it), then to the destination; that last leg is marked as a teleport
  (`k_path_t.legs`, `K_LEG_TELEPORT`), is not walked and does not count in the length. At a jump
  they go to the run-up's start, then to the touchdown, a leg marked `K_LEG_JUMP`.
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
  that cell's way out again. Not when the player only slipped into an earlier route cell lying
  alongside, one with an open crossing straight into the route cell it was in: Doom II MAP26's
  ledge round a pit has a sliver of a cell along its edge, beside two later route cells, and a
  drift of a unit or two into it turned the bot back 150 units (a fall into an earlier, lower cell
  still counts as pushed back: no open crossing climbs out of it). The route remembers which cell
  each leg runs through (`k_path_t.leg_cells`) for this. A waypoint on the way also counts as
  reached within 24 units (tunable).
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
   ledge's edge on purpose. Distances are measured as the engine collides the player, a 32x32
   square: the larger of the x and y distances, so a wall end level with the box's corner is
   touching at 16 * sqrt(2) units. The direction away is from the line's nearest point, so a wall
   end caught by the side of the box turns the player round it. Measured as a circle, the end of
   an 8-unit wall stub at the top of Doom II MAP16's first staircase was 20 units off, and the
   bot pushed into it until it gave up. Obstacles are judged from the floor the player stands on
   (`K_Map_StandingCell`), not the one under its centre: with the centre past a ledge's edge, the
   ledge looked like a wall to climb from the pit, and the player was pushed off it (MAP17).

   **Going round solid things** (`AvoidBlocks`). The engine slides the player along lines, not
   things (`P_SlideMove` traces lines only): against a solid thing the move just fails, a whole
   step before the boxes touch. Dropping the part of the velocity toward it, as for walls,
   leaves nothing when a leg runs at it head on: Doom II MAP27's route passed along the edge of
   a skull pile's square (both waypoints 27 units clear, the leg between them 0), and the bot
   stood 22 units off it until it gave up. So within 32 units of a standing block, the part of
   `v_des` toward its square is turned along its side instead, the speed kept: the way the
   target lies, or round the side the player is on if the target is straight past it. Touching
   (closer than 17), the player eases away as from walls. This is the obstacle avoidance that
   monsters will use too.

   **Careful near edges.** A waypoint within 40 units of an edge the player could drop over but
   not climb back (`K_Graph_DropDistance`: a floor more than 24 below), where the route turns by
   more than 20 degrees or steps down, is taken at no more than 6 units/tic, less for a sharp
   corner (the corner factor applied to 6 instead of full speed). The target moves on from it
   only within 8 units, not 24, so the player really slows to that speed before turning. Turning
   at speed swings the player wide, and each step down leaves it airborne for about 5 tics, with
   no friction and no control: Doom II MAP17's zig-zag bridge over a pit (46 units wide, each zig
   a step of 16 down) threw the bot off at full speed, still turning as it left a step. Running
   straight past an edge needs no slowing (MAP03's straight run to the blue key). An always-on
   limit near edges was tried first; it hid the early turn at the advance radius, and slowed
   stairways for nothing. Braking for a careful waypoint counts on only half the usual figure:
   the controller lags a tic or more, and on Doom II MAP24's 8-unit ledges at right angles over
   a pit the full figure brought the bot to the corner at 7 units/tic instead of 1. And into and
   out of a careful waypoint, when the controls cannot give all the thrust wanted, the momentum
   across the way on is cancelled first and only what is left speeds the player up along it
   (slowing down along it is not held back): after that corner the bot had spent most of its
   thrust running up the next ledge and drifted 23 units over the side.

   **Long falls.** A fall carries on at the speed the player left the edge at, with no friction
   in the air, for about sqrt(2h) tics for h units. Off a careful step down where the fall could
   carry the player off the floor the route lands on, the waypoint is taken no faster than
   drifts 64 units in the fall, and that speed is held past it until the player is off the
   ledge (its box clears the edge 16 units on; before, it sped up again there). Could carry it
   off: a line from the waypoint toward the next one, as far as 6 units/tic drifts, passes over
   a cell that is neither further along the route nor of the sector the route lands in, before
   a wall stops it. Doom II MAP29: off a ledge into a rising blood channel 100 across and 476
   below, left at 8 units/tic, the bot flew 240 units on, into the blood pit beside it; now it
   leaves at 2. Slowing every drop over 48 cost the regression maps 25 s, and slowing only where
   a further edge lies below the landing missed MAP29's (the pit was level with the channel).
4. **Heading:** turn toward the desired velocity, at most 11.25 degrees per tic (2048 angleturn
   units, tunable). Cosmetic for now; later stages need it for aiming and using. When recording a
   demo without `-longtics`, the turn is rounded to a multiple of 256; no remainder needs carrying,
   since the next command aims from the actual angle again.
5. Idle or failed: zero movement.

The controller settings are adjustable in the console's Bot tab, but not saved yet.

**Line goals.** Besides a point, nav can be sent to work a line (`K_Nav_SetLineGoal`): press a
switch, or walk across a line. `K_Nav_LineGoalPoint` gives the spot: 24 units in front of a
switch's front side (use only works from there), or just past a walk-over line on the side away
from the player; 32, 16 and 40 units are tried in turn until the spot is on floor with the
player's box a unit clear of every solid thing. A walk-over line can be crossed anywhere along
it, so for one the spots along it are tried too, from the middle out (Doom II MAP27: a skull
rock inside a diamond of walk-over lines, 11 units from each, leaves room to cross only near
their ends; every spot past a middle lay under the rock, and the goal had no way there). If no
spot is clear of things, the first one on floor is used, as before. Past a walk-over line,
though, only the player's centre has to get across, so first a spot the player can stand on is
looked for (17 units from anything that stops it, as for pickup spots), 8 and 4 units past the
line too, and arriving there means having crossed the line, however near the player stops.
Doom II MAP04: lines 408 and 409 are two sides of a box 16 square in a corner, 8 units from both
walls, so the player crosses them only in a square 8 across on top of it; 24 units past their
middles lay on the walls, the route to them through the gap between the box and a wall, too
narrow (`K_Graph_Clearance` also read a spot on a wall as clear: a line through the point now
counts, at no distance). On arriving at
a switch's spot the player presses use as for a door (below), facing the line; a press after
which nothing happens within 15 tics is repeated, 3 times at most. What the switch does is up to
the caller (an exit ends the level). A walk-over line works only if the player crosses it, but
the route to the spot past it can go round the line's end instead (Doom II MAP05); then the
player first goes to the spot just as far before the line, and from there across.

**Gun switches** (specials 24, 46, 47; `P_ShootSpecialLine`) are worked from the same spot, but
shot: fire instead of use. Only a hitscan attack sets one off (`P_LineAttack` calls it for each
line its trace crosses, whatever lies beyond): the fist, the chainsaw, the pistol, the shotguns
and the chaingun, not rockets, plasma or the BFG, which are projectiles. So the player shoots with
the weapon in hand if it is one of those and has ammo, else brings up the pistol, chaingun or
shotgun (whichever has ammo, in that order; the shotgun's slot brings up the super shotgun if the
player has it, so it needs 2 shells), else the fist (whose slot brings up the chainsaw if the
player has it). The fist reaches 64 units, enough from the spot. Fire is pressed only once that
weapon is up and in its ready state (`A_WeaponReady`), so nothing is fired while switching, and
only when the trace would reach the line (2048 units for bullets). It is held for 5 tics: a
weapon reads the button only as a frame of its idle animation starts, every tic for most, but
every 4 for the chainsaw (Doom II MAP28: a chainsaw picked up on the way, three 1-tic shots at a
gun switch all fell between, and the switch was given up as doing nothing). As with use, a shot
after which nothing happens within 15 tics is repeated, 3 times at most.

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
   replanned. A ride is never given up on: a lift that turns straight back down while the player
   rides it, before it could get off (at the top a lift stops or waits first), has something in
   its way that the map does not show, a monster at its edge or a barrel. The lift is marked
   dearer (3.2.1) and the route replanned: another way if one is worth it, else the same ride
   again, after dozens of turns if need be ("the lift (sector 86) turned back at 120, short of
   the way off (1 so far)"). Until monsters can be fought, a lift a monster keeps turning back
   is waited out; later, killing the monster (or blowing up the barrel) frees it.

Stuck detection pauses while pressing and waiting. The Bot tab shows the action ("door at line
151: waiting"), the log its start and end ("opening the door at line 151", "door at line 151
open after 1.4 s", "waiting for the lift (sector 41) to come down at line 678", "riding the lift
(sector 41) to the way off", "lift ride over after 4.9 s"), and the graph overlay draws
crossings that need an action in blue. The player presses use only through the ticcmd's `BT_USE`, like a human.

`K_Nav_Clear` drops the goal so the player stands still, e.g. while a sector the brain just moved
settles.

**Waiting dry.** A crossing that opens by itself (`K_Graph_OpensByItself`: a lift or floor beyond
already on its way down, or a lift that runs for good; a door already opening) needs nothing done
at it, so the player can get there any time before the wait is over. Where the spot in front of
it is on a floor that hurts, the player waits back where the route steps onto that floor, 24 units
short of it, and goes on once the wait is down to the way there and a second more (the way timed
at 10 units a tic from a standstill, each fall's time on top, so it gets there early rather than
late); it waits so only to save at least 32 tics on that floor, about a hit. Only on the way
ahead: the last route cell before the crossing on a floor that does not hurt, the player's or one
further on, with only floors that hurt from there, walked (no jump or teleporter between). It is
judged again every tic, so where the player has to run for it at once, through the floor that
hurts, to get there in time (the only way out of a pit, say, to a lift its switch lowers), it never
waits. The target stops at that waypoint as at a closed crossing, and stuck detection pauses. Doom
II MAP20: after line 557 the bot dropped off the walkway into the blood in front of floor 26, which
the line lowers a unit a tic, and stood there 10 s while it came down; now it waits on the walkway
8 s ("waiting at (960,-3104), off the floor that hurts ahead, for the lift at line 537: open in
10.6 s, 1.4 s away"; "going on after waiting dry 8.2 s"), about 2 s in the blood, and finishes
with 95 health instead of 50. A player already on such a floor, with no dry floor ahead, still
waits where it is (going back to a dry spot behind is not done), and so does the brain when it
waits for sectors to move (3.5).

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

**Jumps on the route** (3.2.2). A jump that works is run like an open crossing: no stop at the
run-up's start, a corner like any other on the way to it. From there the bot runs straight at
full speed along the line to the touchdown, eased back onto the line while on the ground
(nothing steers in the air, and the run was checked clear, so walls and solid things are not
gone round). The speed profile does not brake for anything past it.
- While the jump lasts, only getting on, on the ground, counts as progress: the run-up crosses
  cells off the route or earlier on it (Doom II MAP24: one turned the bot back from the edge), and
  flying over a later route cell cut the corner after the landing, straight over the nukage.
- Touching down at the landing (on its floor, within 96 units of the touchdown) ends the flight:
  the target moves past the touchdown, and the bot brakes to under 2 a tic (at most 1 s) before
  going on, as the jump was checked to stop there (MAP24's walkway is 16 wide). Touching down
  anywhere else (MAP06's pillar top), it keeps running. A step across a gap narrower than the
  player never leaves the ground and ends when the target moves past the touchdown.
- On a floor that is sinking (a floor mover going down), it does not stop: Doom II MAP08's
  corridor of pillars over slime, each lowered by a walk-over line on its edge at a unit a tic,
  leaves about 24 tics on each before the next one is a step too high. The bot runs on from one
  hop to the next (each with a run-up of 8, 3.2.2).
- A jump off a lift that is down (a ride, 3.2.2): the bot stands at the run-up's start, facing
  the jump, and rides the lift until the jump works again; if the run-up had begun as the lift
  went down, the target goes back to its start (the lift sinking under the player is no takeoff,
  and its passing below the landing floor no miss).
- On the ground more than a step below the landing floor, it is down in the gap: "missed the
  jump", and the route is replanned. A jump that stops working on the way (heights moved) is
  replanned too, as any crossing that closes.
- The log shows "running up for the jump to (x,y)", "off the edge at (x,y) at v units a tic" and
  "landed at (x,y)", and no "dropped" while flying over the gap. Off the edge means off the
  ground over a floor more than a step below the one the run-up was last on: a step down on the
  way is still run-up (MAP20's long run-up down a walkway, 3.2.2).
- Not yet: a run-up's start is approached like any corner, at up to a fifth of top speed through
  a sharp turn and turning within 32 units of it. A short run-up has no room to undo that: MAP09's
  jump out of door 33 (26 units of run-up, 7.4 a tic at the edge from a standing start) was left
  at 2.7 and missed once (5 s). Stopping at the start and driving right up to it fixed that jump,
  but then the hop just before it (37 units, across a narrow gap, never off the ground) was not
  seen to be over and the bot ran on along its line into a lift's pit; tried and taken back, to
  look into.

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
| USE TRIGGER (sector) | the crossing it was pushed for is open, or will be after a wait (a door opening, a lift on its way, a floor to ride up); else the sector moved and stopped (or after 15 s: perpetual lifts never stop) | every line with the sector's tag whose special moves sectors (`K_Specials_MoverTrigger`): switches pressed, walk-over lines crossed, gun switches shot (3.4); a stair builder (7, 8, 100, 127) also moves every step of its chain, found at level load as `EV_BuildStairs` finds them (through two-sided lines with the step in front, to the sector behind with the first step's floor texture), though only the first is tagged. Shown as `USE TRIGGER for sector N (tag T)`, then `USE SWITCH line N (tag T)`, `SHOOT SWITCH line N (tag T)` or `CROSS LINE N (tag T)` with the line's tag once one is chosen |
| GO TO (x, y) | the player arrived | the point (a console click) |

**Planning a goal** (`PlanGoal`) scores every candidate not yet tried by route
(`K_Path_Find` with the brain's block cost, 3.3):
- the shortest **open** route wins, and nav gets the candidate (a point, or a line to press or
  cross, 3.4);
- if none is open, the candidate whose blocked route is cheapest wins (fewest blocked crossings,
  then shortest), and the fix for its **first** blocked crossing is pushed (`FixFor`):
  - a **locked door** -> GET KEY of its colour, if such a key lies in the level;
  - a **step too high** -> USE TRIGGER for the sector beyond, if some line lowers its floor (a
    lift does too), or for the one the player stands in, if some line raises its floor; never
    one that lowers the floor the player stands on, which only makes the step taller (Doom II
    MAP27: the bot lowered the arm it stood on, to climb the pillar beside it, and burned a
    dozen walk-over lines on that);
  - **too little headroom** -> USE TRIGGER for the sector beyond or the one the player stands
    in, if some line lowers its floor or raises its ceiling;
  - a **gap too narrow** -> USE TRIGGER for a sector that narrows it (`K_Graph_NarrowSectors`):
    one a line moves out of the way (bars that lower), or a closed manual door, opened from its
    own face (`K_MOVE_DOOR`: a door's faces are triggers of its sector, asked for only here, never
    to get through a door from behind). Among them, the one the player can open soonest: one
    moved by a line, or a door that needs no key or one in hand, else the shortest trip to a key
    of its colour and on to the door, as the crow flies; a door face that needs a key the player
    lacks gets GET KEY first, as any switch does. One at a time: if the gap is still too narrow
    once it opened, the next one is pushed (Doom II MAP19: three key bars, two of which must be
    open). Else for the sector on either side of the crossing, e.g. a lift that takes the player
    down to where the way on has headroom (Doom II MAP05: a passage whose ceiling is level with
    the floor in front of it until that floor, a lift, comes down); only lines that lower a floor
    or raise a ceiling;
  - a **gap inside a cell** (a pinch, 3.2.1) -> the same as a gap too narrow at a crossing: USE
    TRIGGER for the sector narrowing it that the player can open soonest (Doom II MAP11: line 516
    opens the fence's pillar doors). The goal is done once the gap is wide enough, or widening;
  - a **jump that does not work now** (3.2.2) -> one found as if a sector had moved: USE TRIGGER
    for that sector, to move it there (Doom II MAP09: switch 126 raises floor 6, switch 157 opens
    door 33, line 380 builds the stairs); one found through a door that is closed: USE TRIGGER for
    the door (MAP09: switch 455 opens door 101 at the top of the stairs). Else for the takeoff
    sector, if some line raises its floor (it is below where the jump was found), or for the
    landing sector, if some line lowers it (Doom II MAP08's island, switch 572);
  - which way a line moves its sectors comes from its special (`K_Specials_MoverMoves`: floor
    up, floor down, ceiling up), and a USE TRIGGER goal remembers the moves that would help:
    only lines that make one of them are its candidates. A lift only lowers: it comes back up to
    where it was, no higher, so one already at the bottom does nothing (Doom II MAP28: lift 82
    starts level with the room, below the way off, and walk-over line 402 raises it first); a
    perpetual lift also rises, to the highest neighbouring floor;
  - a **switch that needs a key** the player lacks (remote locked doors 99 and 133-137, e.g.
    Doom II MAP02's red bars), or stands behind a door that does -> GET KEY of that colour,
    before going to press it. Use stops at the first line with a special on its way, so a
    locked door between the press spot and the switch takes the press (Doom II MAP27: switch
    727 at the back of a closet behind the blue door; the bot pressed three times at the door,
    gave the switch up as doing nothing, and never went for the blue key). A one-shot door
    loses its special once opened, so then the press gets through;
  - **keys first:** if a door further along the same route needs a key that lies in the level,
    GET KEY is pushed instead of the fix for the first crossing; so is it for a door further on
    that only switches needing a key open (Doom II MAP04: door 302, special 99, blue). A key
    stays, while what a switch does may not last until the player gets there: on Doom II MAP12
    the bot ran 11,000 units to lower a lift, fetched the yellow key while it went back up, and
    had to lower it again;
  - **lifts last:** likewise, if the first crossing's fix works a lift, or a door that closes
    again by itself (opened only by lines that open, wait and close it:
    `K_Specials_DoorCloses`), and a crossing further along the route is fixed some other way,
    that one is fixed first, since the lift comes back up and the door shuts: on Doom II MAP09
    the bot lowered lift 123 on the way to the exit, then went to open door 130 further on,
    which had closed again, and came back to a lift back up, working its two switches a second
    time (now 10.6 s faster); on MAP04 it opened fast door 32 (special 114, shut after 4 s),
    went back past the start for switch 244, which lowers floor 84 for good, and found the door
    shut again on its way to the blue key (now 2.2 s faster to it). Only if that fix can be got
    to without a key the player lacks: on MAP14 it was a switch behind the red door whose key
    the bot was fetching;
  - **no fix past a dead end:** if the route has a crossing further on that nothing fixes now
    (a door needing the key the player is out fetching, say), fixing those before it gets
    nowhere, and the candidate has no fix (Doom II MAP14: the bot opened the self-closing doors
    in front of such a crossing over and over);
  - USE TRIGGER only for a sector some line still moves (one-shot lines lose their special);
- if nothing can be fixed, but sectors are still moving (`K_Graph_MovingSector`: a floor,
  ceiling, door or lift on its way; perpetual lifts and crushers do not count), the goal
  **waits**: the player stands still and the goal is planned again every second, for up to 15
  seconds. Stairs being built (Doom II MAP15) or a floor going down (MAP13) can still open a way.
  Only if some candidate has no way now: candidates already tried stay given up on, so waiting
  cannot bring them back (Doom II MAP17 stood 15 s for a walk-over line it had failed to reach).
  The log names one of the moving sectors;
- otherwise the goal **fails**, with the reason for the last candidate tried; the log first
  gives the reason for each candidate (up to 8).

**Pickup spots.** A key is picked up when the player's box touches the key's (radius 20), so
within 36 units on each axis, not only at the key itself. If the player cannot stand at the key
(less than 17 units from anything that stops it, measured with its square box:
`K_Graph_Clearance`), the candidate is the
nearest spot within 28 units on each axis where it can (searched in 4-unit steps). Doom II MAP05's
red key lies on an impassable line, at the end of a shaft, and is picked up from the room beside.

When a goal is done it comes off the stack and the goal below is planned again, from wherever the
player is: the world has changed. And before the goal on top is planned, the goals below it are
looked at, the oldest first: one with an open way now (a candidate it can work, with an open
route there) drops every goal above it, pushed for a way round it no longer needs. Doom II MAP14:
switch 416, pressed for another way round, opened the door on the way to the red key, and the bot
went on with that other way, then looped. Fixes are optimistic: the switch that targets a sector is used
without working out exactly where it moves the sector; after it moves, planning again shows
whether the way is open, and if not, the next blocker is pushed.

**Setting off at once.** A USE TRIGGER goal remembers the blocked crossing it was pushed for. As
soon as that crossing is open, or passable after a wait (a door opening, a lift on its way down,
a floor to ride up), the goal is done and the goal below is planned again: the player sets off
right after the press, as a human would, and waits at the door or lift instead of at the switch.
Doom II MAP03 got 6 seconds faster: switch 431 raises a strip slowly, and the bot now drops onto
it from the walkway while it rises and rides it up to the teleporter. Otherwise the goal waits,
with the player standing still, for the sector to start moving after its line is used (2 seconds
for a walk-over line) and then to stop. If its heights never changed, the line did nothing and
the next candidate is tried: a mover can be set going with nowhere to go, like a lift already at
the bottom, which waits there at once (Doom II MAP28: counted as moved, the brain worked four
switches and a walk-over line round and round, three times each). The crossing is checked before the goal is even planned:
it may have opened while a goal in front of it was worked on, and planning would send the player
off for nothing and count a use of the line (on Doom II MAP24 two lines used up two of their
three uses that way).

**Guards against loops and early give-ups:**
- a candidate that does not work out (nav fails, the press moves nothing, the key was not picked
  up) is not chosen again for that goal; the next candidate is tried;
- but when nav fails because its way shut on the way and a route is still there, blocked (a door
  closing before the player got back to it), the goal is planned again, which pushes the fix (the
  door's switch), up to 3 times: Doom II MAP02's fast door (special 114, closes after 4 s) shut
  while the bot stood at a switch, and its only blue key was given up for good, the bot pressing
  the door's switch every 5 s from then on;
- a goal whose candidates all fail is marked failed for the rest of the level, never pushed
  again, and the goal below looks for another way;
- a goal already on the stack is never pushed again (a key behind its own door);
- at most 8 goals deep; one line is worked at most 3 times a level (the switch of a door that
  closes again before the player gets there), and a sector is fixable while one of its lines can
  still be worked. Per line, not per sector: Doom II MAP21 raises a staircase 16 units a press, by
  four switches in a row (the fourth was refused when the limit was per sector);
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
3. If `cmd->buttons & BT_SPECIAL` (pause/unpause, save), keep `buttons`; otherwise replace them
   with the bot's: `BT_USE`, `BT_ATTACK` (gun switches), and `BT_CHANGE` with a weapon slot, as
   the number keys send it. Movement is still overwritten in that case, so no human movement
   leaks through.
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
    Legs taken carefully near an edge (3.4: into a careful waypoint, braked for, and out of it,
    drift first) are mint on a blue route and pale amber on an orange one, as are the waypoints
    between them (`K_Nav_CarefulLeg`, from the marks made when the route is planned).
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
  (controller details, off by default), `PLAYER` (what a human driving does, 3.10). Later stages
  add their own, e.g. `COMBAT`, `ITEMS`.
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

### 3.10 `k_trace.c` - player trace

When a level is hard to read from its data, the user plays it through by hand and the log shows
the way the bot should find. With `-karpconsole` and no `-autoplay` (a human drives), each tic
`K_Trace_Tick` compares the player and the level with the tic before and logs the differences
under `PLAYER`. Read-only, like the rest of Karp (2.4):
- **Where the player goes**: every sector entered (with its heights), a point every 256 units, a
  teleport (a move of more than 100 units in a tic), every line with a special crossed (last tic's
  special: a walk-over line used once loses it in the tic it is crossed).
- **What it presses**: use, with what the press reaches as `P_UseLines` traces it (64 units
  straight ahead, stopping at the first line with a special, or at one with no opening), and from
  which side; fire, with the weapon and the facing. The press was handled earlier in the tic, so
  a one-shot line has lost its special already; last tic's specials are used.
- **What changes**: lines used up, sectors that start or stop moving (a mover thinker), solid
  things gone (barrels blown up).
- **The player**: health up and down (damage as a warning), keys and weapons picked up, the
  weapon in hand.

Started from `K_Ticker` before the exit check, so the exit line crossed shows before "level
finished". Doom II MAP23 was the first map read this way (see the playtests in section 4).

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
     (Finished since, see the jumps below.)
   - MAP07: a **boss map**: the way to the exit opens only when monsters die (follow-up 10), so
     with `-nomonsters` it cannot be finished at all. Skipped until combat.
   - MAP08: the way to the exit is over ledges reached by **jumps** (they start lowered and rise
     during play; follow-up 7). Skipped for now, to come back to. (Finished since, see the jumps
     below.)
   - MAP09: another **jump**, found by the user playing it: from a lift at about (700,1647)
     (sector 12) across to about (380,1444) (sector 6, floor -64). Without it the bot sees no
     way on and chases switches it cannot reach. Skipped for now, to come back to. (Finished
     since, see the jumps below: the jump is from the lift onto floor 6 once switch 126 raises it.)
   - MAP10: finished in 45.4 s with no changes (yellow key, blue key, nine doors, exit switch).
   - MAP11: the exit area (the north part of the central circle) is reached by a **jump**, per
     the user; the bot sees only a 296-unit step at line 796. Skipped for now, to come back to.
     (Finished since, see the jumps below: what it lacked was the fence of pillars.)
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
   - MAP13: three keys in a chain (yellow behind red, red behind blue), teleporters and a lift
     ride all worked: the bot had the blue and red keys after 53 s. The yellow key is on top of a
     stepped pyramid (96-unit steps) beside a perpetual lift (sector 20, 16 to 400): from the
     lift's top it takes a **running jump** of about 140 units, dropping 96, onto the key's ledge
     (follow-up 7). Skipped for now, to come back to. Also seen: after the red key the bot gave up
     on a step that was still shrinking by a unit a tic (a floor moving down, follow-up 9).
   - MAP14: the red skull key (near the plasma gun) is reached by a **jump** over several gaps,
     straight north from about (2021,572), per the user (follow-up 7). Without it the planner
     goes round in circles (bars behind the red door, the red key behind a switch room) and
     gives up at once. Also seen: the switch room (sector 256) is entered through a doorway
     exactly 32 wide (sector 302; Karp needs 34) and has a crushing ceiling started by walk-over
     lines just outside (73, stopped by 74): the bot knows nothing of crushers or damage yet.
     Skipped for now, to come back to.
   - MAP15: the yellow key is in a room left by **stairs** that a switch builds (line 634,
     special 127). Karp knew only the tagged first step, so the bot saw no way out: stair
     builders now count as moving every step of their chain (3.5). The bot pressed the switch,
     and with no way left while the stairs rose it waited and planned again (3.5). From the top
     of the stairs, though, the way on is a **jump** from the central tower to the southwest
     region with the yellow door, per the user (follow-up 7). Skipped for now, to come back to.
     (Finished since, see the jumps below.)
     The failure log now gives every candidate's reason, which showed both exits blocked further
     on, past the stairs.
   - **Setting off right after a press** (3.5): the bot no longer stands at a switch until what
     it moved has stopped, but heads for the door, lift or rising floor at once and waits there.
     Times: MAP03 40.0 -> 34.2 s, MAP05 33.0 -> 30.7 s, MAP12 84.9 -> 77.1 s (MAP01 and MAP10
     have no switches on the way). It took three more changes: a door already opening counts as
     open for the way out of it (MAP12's route went on through a door still rising), rides work
     on any rising floor and are judged for width on arrival (MAP03's strip), and a teleporter
     reached by a ride waits for the ride first.
   - MAP16: passed, 40.3 s: blue key (top of the spiral staircase in the nukage), the blue door,
     the red key, two tag-18 lifts and the red-key pillar to the exit. Three fixes:
     - At the top of the first staircase the bot pushed into the end of an 8-unit wall stub
       beside it: wall avoidance now measures with the player's square box (3.4).
     - The red-key pillar (a 32x32 door, sector 109) in the mouth of the corridor to the exit
       counted as open, like every manual door, so the gaps beside it looked wide and the bot
       went for the exit without the red key. Doors now count as open only where the player opens
       them on the way (3.2.1): the gaps are exactly 32, too narrow, and the pillar is a locked
       door, so the red key comes first.
     - The red key's room lies behind the blue door, a one-way door with a strip 8 deep behind it
       that then looked too narrow, so the red key looked out of reach. A door the player
       would be standing over all along a crossing now counts as open there (3.2.1), and the
       brain goes for the blue key first.
     Also seen: having ridden one tag-18 lift up, the bot found the second one back up and
     pressed another tag-18 switch for it, which works out. Wall avoidance is now box-measured on
     every map; the regression maps pass at their earlier times (MAP12 79.9 s, lift timing).
   - MAP17: the red key lies past a zig-zag bridge over a pit, each zig a step of 16 down; at full
     speed the bot flew off a turn into the pit, with no way back. That led to careful speed near
     edges (3.4), which first broke MAP16 (a leg along a landing's open edge, now routed through
     the cell, 3.3) and showed that wall avoidance steered the player off ledges (now judged from
     the floor it stands on, 3.4). The bot now crosses the bridge, takes the red key and works
     through five switches and two lift rides. Also fixed: after a walk-over line it could not
     reach (line 375 is only crossable from its high side, 144 up), the goal waited 15 s for
     moving sectors that could not bring the line back (3.5); it now gives up at once. Then the
     bot sticks on a **barrel** on a staircase (four barrels, a way past them 12 units wide), and
     the engine does not slide the player along things. **Needs solid things** (follow-up 3).
     Skipped for now, to come back to. Also seen: walk-over line 375 counted as reachable because
     the route to the spot past it went round its end; only nav found the near side out of reach.
     Falls and push-backs are now logged: "dropped N units", "back in route cell".
   - MAP03, again: now stuck at a torch on the way to switch 445, at (3572,3424). With the slower,
     careful approach the bot no longer slides past it at the old speed and angle. An expected
     regression: **needs solid things** (follow-up 3). MAP01 8.1 s, MAP05 31.0 s, MAP10 45.5 s,
     MAP12 80.5 s, MAP16 40.9 s still pass.
   - MAP18: passed, 32.5 s. The way on starts behind a door opened by **shooting** its face (line
     39, special 46, GR door open stay), which Karp did not know: the passage in front looked
     "too narrow" (the closed door 8 units in), with nothing to fix it. Gun switches are now
     triggers like switches, shot with a hitscan weapon (3.4). The bot shot it with the pistol in
     hand, then took the yellow and blue keys (a teleporter, a stair switch, a lift ride and a
     teleporter back) to the exit. The weapon change was not needed there, so it is untested in
     play. The map load reports 2 small holes (8 units) and 43 cells apart from the rest; they did
     not matter.
   - MAP19: the route to the exit crosses a river-shaped sector lined by a ring of blue torches
     that together form a wall: open in the geometry, impassable with the things. The bot sticks
     on a torch at (-1394,669) and every exit route leads there. **Needs solid things**, and not
     only steering round them: the pathfinder must plan round them too (follow-up 3). Skipped for
     now, to come back to.
   - MAP20: no way to the exit from the start: every route to the raised parts (the walkway with
     walk-over line 557, tag 15, which lowers the way to the exit; the room with line 271, which
     opens the door to a teleport pad) climbs out of the big floor at -72 by a step of 144 or
     264. The way up takes a **jump** over a gap, per the user (follow-up 7). Skipped for now, to
     come back to. (Finished since, see the jumps below: the way up is a teleporter onto a block
     that sinks to the walkway; the jump is on the way to it.) Seen on the way: the pad (lines 874-877, tag 16) sits inside walk-over lines
     300-303 that lower the block it teleports onto, from 400 (above the walkway's ceiling) to
     176, so stepping onto the pad does both, in the right order.
   - MAP21: after the first teleporter the route ran past a yellow-key block (sector 58, doors on
     three faces, a plain fourth) through a strip exactly 32 units wide, and the bot stuck there.
     The narrow check only looked at crossings, and those on either side of the block were 24
     units off: gaps inside cells are now checked too (3.2.1, 3.3). The route then tried the gap
     between two yellow blocks (24 units diagonally), which looked open because the blocks' faces
     are door lines: a locked door now counts as open across its face only with the key (3.2.1).
     Then the bot went for the yellow key: four one-shot stair builders (lines 110-113, tag 18)
     each raise a staircase 16 units, and the first step (sector 51) is also a lift worked from
     its side walls (lines 133, 134). The fourth press was refused (at most 3 triggers per sector,
     now per line, 3.5); with it, the bot called the lift, rode it up the stairs, teleported, took
     the yellow key (24.2 s) and headed for the exit. A red-key door is in the way, and the red
     key lies on a raised lift (sector 16) lowered only by a switch on a platform 96 units up
     (sector 15), reached by a **jump**, per the user (follow-up 7). Skipped for now, to come back
     to. The regression maps pass at their times.
   - MAP22: ten switches, two teleporters and two lift rides went well, and the bot had the blue
     key after 17 s. The red key's door opens from switch 299 (tag 12) on a ledge (sector 16,
     floor -136) 88 units above a room; the lift in the room (sector 25, tag 11, lowered by switch
     289 behind door 23, tag 10) rises to -136 too, but 16 units short of the ledge, with room
     floor in between. The player's box walks across such a gap (its floor is the highest under
     it); Karp has no link there, so the ledge looked out of reach. The brain kept opening door 23
     and lowering the lift, found no way from the lift to the switch, and gave up once switch 276
     had been used three times. **Needs gap links** (follow-up 7). Skipped for now, to come back
     to. (Finished since, see the jumps below.)
   - MAP23: the starting area is full of barrels, and the bot sticks among them within 2 seconds,
     on its way to the first switch (line 9, tag 2). **Needs solid things**: going round barrels,
     or blowing them up (follow-up 3). Skipped for now, to come back to.
   - MAP24: blue key after 8 s, then a long run over 8-unit ledges at right angles above a nukage
     pit (sector 24 over sector 0, 512 lower), where the bot fell at a corner, rode a pad in the pit
     back to the start and fell again, round and round. Careful corners now brake earlier and
     cancel drift first (3.4): the bot takes the corners within a unit of the ledge's middle, and
     had the red key at 40 s, past two teleporters and switches 1004 and 990. Then it looped on
     the way out. The key's platform (sector 86, tag 25, lowered by switch 984 from sector 83)
     tops out at 128 under a ceiling of 176: with the player on it, it rises only to 120, where
     the player's head meets the ceiling, and turns back (3.2.1). Karp took it to reach its top,
     where 48 units of headroom left no way off, so the 64-unit step to sector 83 looked blocked.
     The brain pushed goals for both lifts; while the platform was high, it spent the time on goals
     already met, set off late, found the way closed again, and wrote off switch 978, the only way
     out of the room. Now a rising floor stops at head height and the way off is judged where the
     player can first step off: the bot rides the platform up, steps off at 43 s, presses 978 and
     teleports out over line 1013 while lift 69 is down, and **finished in 60.4 s**. Also new:
     a trigger goal whose crossing has opened ends before it is planned (3.5), and a lift that
     turns back on the player is never given up on (3.4; tested by holding the bot on the
     platform past its turn: it replanned and stepped off on the way down, 61.0 s). MAP01 8.2 s,
     MAP05 31.0 s, MAP10 45.9 s, MAP12 75.1 s, MAP16 42.2 s, MAP18 32.9 s pass, and MAP17 still
     gets its red key (then sticks at the barrels, as before).
   - MAP25: no way off the start platform (floor 280). Its only two ways down are drops through
     niches 16 units wide (sectors 41 and 47, floor -72) onto teleporter pads (38 and 46, the
     lower level's way back up; entered from the back of their lines, so they do not teleport).
     The player's box keeps it on the walkway until it has cleared the edge, then it falls onto
     the pad, never standing in the niche; Karp judges walkway-niche-pad as two crossings, and
     standing in the niche the walkway's face is 16 away: too narrow. GO TO EXIT failed at 0 s.
     **Needs gap links** (follow-up 7): a link from the walkway's end straight to the pad.
     Skipped for now, to come back to. (Finished since, see the jumps below: a drop past the
     niche.)
   - MAP26: all three keys by 42 s, then stuck 300 units from the exit, against a yellow-key
     pillar (sector 113): a free-standing door 16 square with door lines (34) all round, in the
     middle of a corridor 64 wide, leaving 24 units on either side. With the key, the pillar
     counted as open seen across any face, so the route squeezed past it, never through it, and
     nothing opened it. Now only doors set in a wall count as open across a face that opens them
     (3.2.1); the bot opens the pillar and **finished in 49.9 s**. On the way: simply dropping
     that rule broke MAP01 (a light sector before the exit door looked pinched) and MAP24 (an
     alcove 17 deep before an exit door looked too narrow), and judging chords near doors more
     loosely let MAP21's route slip past its locked red door. MAP01 8.2 s, MAP05 31.0 s, MAP10
     45.9 s, MAP12 75.1 s, MAP16 42.2 s, MAP18 32.9 s, MAP24 60.4 s pass; MAP03, MAP17 and MAP21
     end as before. Then, seen by the user: on the ledge round the pit in the big semicircular
     room (sector 13, up the spiral stairs), the bot turned back mid-ledge, went a few steps and
     turned again. Its centre had drifted a unit or two into a sliver of a cell along the pit edge
     (cell 96), which the route had already passed through, and that counted as being pushed
     back (3.4). Now it carries on: yellow key 1.4 s sooner, **48.5 s**.
   - MAP27: the red skull key sits on a central pillar (sector 50, floor 192) between four arms
     (sectors 51, 52, 58, 60, floor 128). About 28 walk-over lines (38, seven per arm, spread
     over the map and round the pillar) lower the arms to 0; switch 727 (71, S1, turbo lower)
     lowers the pillar to its highest neighbouring floor plus 8, and sits behind the blue door.
     First run: the bot pressed 727 at the blue door without the key (the door took the press),
     gave the switch up as doing nothing, then went after the step from an arm up onto the
     pillar by lowering that arm again: three minutes of walk-over lines that moved nothing, a
     yellow key fetched for one of them, past the blue key without picking it up. Now fixes go
     the right way, and a switch behind a locked door needs that key (3.5): yellow key, blue
     key, switch 727, red key, **finished in 80.9 s**. Pressed with an arm still up, 727 would
     leave the pillar at 136, out of reach for good (the arms only go down, and 727 is the only
     line tagged 32): a softlock a human avoids by crossing the arm lines on the way, as the
     bot's routes did too. Not handled: predicting where such a one-shot lowering leaves the
     sector, and lowering its neighbours first. MAP17 no longer chases walk-over line 375: it
     opens a door (sector 147) whose floor is 224 above the bot's, and a ceiling going up does
     nothing for a step. The rest pass as before.
   - MAP28: the red key is up a step of 64 from lift 82 (tag 10), which starts at the bottom,
     level with the room. Taking a lift for something that raises its floor, the brain pressed
     its switches (719-722, special 62): a lift at the bottom has nowhere to go, but the engine
     still sets it waiting, which counted as moved, so round it went, three times on each line,
     then failed. Now lifts only lower (3.5), the far walk-over line 402 that raises the lift is
     chosen, and a line after which no heights changed is tried no more. Then the gun switch for
     the exit door (line 573) "did nothing": the bot held a chainsaw, picked up on the way, which
     reads fire only every 4 tics, and each shot was a 1-tic press; fire is now held 5 tics (3.4).
     **Finished in 72.5 s**, no jump needed. The rest pass as before.
   - MAP29: GO TO EXIT failed at 10 s, "a step 320 units high at line 36". The exit hall (sector
     46) is reached only up a winding staircase (sectors 93-98, 296 to 568) from sector 93, where
     the tag 37 teleporter lands. Its pad (sector 40) is on an island (sectors 38 and 39, floor
     480-488) in a courtyard (sector 33, floor 400), with nothing to climb onto it: three pillars
     (sectors 41, 42, 44, floor 560, lowered to the courtyard by one-shot switches 825, 837 and
     975 elsewhere) stand between it and a ledge (sector 43, floor 528) about 180 units away.
     **Needs a running jump** from that ledge onto the island (follow-up 7). Skipped for now.
   - MAP30 (Icon of Sin): not tried. The boss brain spawns even with `-nomonsters`, and the
     level ends only when it dies: rockets fired into it from the top of a lift. **Needs combat
     and a hardcoded route** for that special path (by the user's choice, map-specific tricks
     are hardcoded per map, not built into the brain). Skipped for now.
   - MAP31: **finished in 41.6 s** at the first try, through twelve doors to the normal exit
     switch (line 549); the secret exit to MAP32 was the longer way.
   - MAP32: not tried. The exit opens (the doors tagged 666) when the last of four Commander
     Keens dies (`A_KeenDie`). Keens hang still and never fight back, a first target for
     aiming and shooting, but with `-nomonsters` they never spawn (they count as kills,
     `P_SpawnMapThing`), and with monsters there are 28 Wolfenstein SS and a Cyberdemon.
     **Needs combat**, efficient enough for a Cyberdemon. Skipped for now.

   **Where Doom II stands** after the first pass, with `-nomonsters`: 11 maps finish (MAP01,
   05, 10, 12, 16, 18, 24, 26, 27, 28, 31); 14 need gaps or jumps (follow-up 7: gap links for
   MAP04, 22, 25; running jumps for MAP02, 06, 08, 09, 11, 13, 14, 15, 20, 21, 29); 4 need solid
   things (follow-up 3: MAP03, 17, 19, 23); 3 need monsters and combat (follow-up 10 and later
   stages: MAP07, 30, 32).

   **Solid things** (follow-up 3, first part: blocks in the map, 3.2, 3.2.1, 3.4):
   - MAP03: **finished in 33.6 s**: the route goes round the torch on the way to switch 445.
   - MAP27 broke twice on the way and was fixed. A skull rock stands inside a diamond of
     walk-over lines, 11 units from each: every spot past a line's middle lay under the rock, so
     the second line of the diamond had no way to it; walk-over lines are now tried along their
     length (3.4). Then a leg ran along the edge of a skull pile's square and the bot stood stuck
     22 units off it, head on: solid things are now gone round in steering (3.4). Finished in
     80.3 s, as before. Watched one by one, two things to improve (not fixed yet): the lift
     beside the skull rock is lowered at the start long before it is any use (follow-up 12),
     and a one-way door that closes before the bot gets back to it costs a switch (follow-up
     13).
   - MAP17: the barrels on the staircase are planned round now, and the bot gets all three keys
     (blue at 63 s, yellow at 84 s), working seven switches and five lift rides. Then the way to
     the exit (floor 600) is a step 112 high (line 935) into a room that is also reached
     through a block 24 wide (sector 80, tag 30) that a switch far off lowers (line 1101). The
     route's first blocked crossing is the step, which nothing fixes, so the brain never looks
     further along the route; not a solid-things problem. Watched one by one (the user: a nice
     slalom between the barrels), two more things:
     - **The ditch.** Switch 491 (tag 19) sits in a 48-unit alcove at the end of a ditch 64
       wide and 288 deep (sector 40). The bot drops in to press it, and the switch raises the
       ditch floor too, at half a unit a tic, so it rides 15 s back up. With jumps (follow-up 7)
       the bot could keep out of the ditch (the user's note, optional). Also possible without
       jumps: in vanilla, use ignores height (`PTR_UseTraverse` only needs an opening on the way,
       here 168 and 48 units), so the switch can be pressed from the ditch's edge, 288 above.
       The line goal spot (3.4) only looks straight out from the switch, which is all ditch; a
       spot on higher floor within use range would save the drop and the ride. An idea, for the
       user to decide.
     - **South staircase, then north** after the blue door: not a detour. The yellow key lies on
       lift 164 (floor 224), ringed by a walkway at the same height (sector 165) that everything
       around is 80-160 below, so the only way on is to lower the lift and step on from the west
       (floor 64). That floor is reached by dropping past walk-over line 722 from the area
       behind door 153, and door 153 opens only from switch 677, at the top of the south
       staircase, a dead end. So: up the south stairs for the switch, back north through the
       door, down past line 722 (which lowers the lift), onto the lift. The brain chose the door
       route because no route avoided the door (one with the lift alone would have cost less).
   - MAP19: the rows of blue torches along the river are walls (3.2.1), so the old route is
     closed, and now every route left passes a gate of three key bars (sectors 12-14, 8 and 16
     wide with 8-unit gaps; their door faces look north, the side the player comes from) that no
     single door opens wide enough. GO TO EXIT fails at once. The map's real way round is still
     to be found.
   - MAP23: the start hall holds 208 barrels: a fence north of the start (12-unit gaps), rows
     with 44-unit gaps down the hall (passable). The planner prefers the route east over a
     128-unit step at line 9, since every route out crosses something unfixable further on,
     presumably a barrel fence to blow up (it was not: a gap link, see "MAP23 played through by
     hand" below). Earlier
     the brain took the door switch at line 9 as the fix for that step, which it is not (it
     raises the strip's ceiling and leaves its floor 128 up); since the fixes go by direction
     (3.5), it no longer does.
   Regression set unchanged in time (MAP26 49.1 s, MAP28 72.7 s, MAP12 77.8 s), MAP21 ends as
   before. Now 12 maps finish (MAP03 added to the regression set).

   **Bars that must be opened together** (3.2.1, 3.5): MAP19's gate, the way to the exit, is a
   passage 64 wide (x 192-256) holding a blue bar (8 wide), a red one (16) and a yellow one (8),
   with 8-unit gaps. One bar open leaves 24 or 32 units; the red one and either other leave 48.
   A debug dump of the gate showed the crossings between the bars as open (both bars "near all
   along", so counted open) while the chords there saw them closed, so the route ran into an
   8-unit gap and stopped at a pinch, which has no fix. Now free-standing doors count as closed,
   and a door that narrows a gap is opened from its face. With the user, no working out of door
   combinations at level load: the bot responds as it goes, one key, one bar, still too narrow,
   the next key.
   - MAP19: **finished in 100.9 s**. The blue bar was the cheapest first (the trip to its key and
     on to the gate), but GET BLUE KEY found no way (a 40-unit step at line 666 that nothing
     lowers) and failed for the level. The red key took three switches (lines 1205, 419 and gun
     switch 618); back at the gate the bot opened the red bar (51.6 s), the gap was still too
     narrow, so it fetched the yellow key (walk-over lines 443 and 504) and opened the yellow bar
     (100.1 s), then walked through to the exit. The worst case the user described, as designed.
     Two things seen, not fixed: the blue key may have been reachable after the red key's
     switches (it lies 570 units from the red one), but it was given up for good (follow-up 11);
     and the red key in hand made the red bar cost nothing, so the bot carried it straight back to
     the gate, though the red bar alone could never be enough (seeing that would take the
     combinations the user ruled out).
   - Regression set unchanged in time; MAP19 added to it. MAP21 ends at the same point, with a
     different reason: the red door at line 210, whose key it cannot reach, instead of a gap next
     to it.

   **MAP23 played through by hand** (the player trace, 3.10; 171 s, with some detours). Route
   experiments first showed that barrels were not what stopped the planner: with barrel crossings
   counted as fixable, every route to the exit still climbed the 128-unit step at line 9, and the
   teleporter behind the south door only led back into the hall. The way, from the trace:
   - Off the start square (line 0 opens the shotgun closet), south through the slalom, door 5,
     line 52 (opens the east strip, sector 35), the teleporter (tag 3) to a landing among barrels
     (448,-576), north between two rows of barrels to sector 16, a teleporter to the west room
     (floor 192). There line 113 opens the strip door into the hall (sector 53) and switch 111
     raises the hall's south triangle (sector 46) to 128, making it a lift (switches 108 and 109).
   - From the triangle at 128 into the east strip (sector 35, 128) at its corner: the only line
     between them (114) is 16 units long, beside a wall, and the player's box crosses a sliver of
     hall floor (0) with its centre (lines 108 and 9 crossed in one tic) without ever leaving the
     128 floors. For the point model it is a drop, and line 114 too narrow: **a gap link**
     (follow-up 7). The user confirms there is no other way into the east part.
   - East stairs, sector 4, the pit (floor -128, damaging) and its teleporters to the west part
     and to sector 5; from there, the yellow skull key (walk-over lines 386, 230 and 289 work the
     doors on the way); back by teleporters to sector 13, door 350, the yellow door (line 359),
     and the exit room.
   - Barrels: the start hall's need no shooting (the slalom is passable), nor the landing's (gaps
     of 44-76 units; the user shot them from the pad and lost 73 health to four blasts, a
     berserk pack nearby put it back to 100). One must go: barrel (1184,-320) stands in the
     middle of the 64-unit way into the exit room, 22 units from each wall, and it chains to
     three barrels on a ledge beside the corridor, the last 64 units from the yellow door: from
     the corridor that is about 80 health. The user shot the ledge barrel from outside the yellow
     door (966,-356); the others went off out of sight and did no damage. A blast only hurts what
     the barrel can see, and the bot must know that (follow-up 3, second part).
   So MAP23 waits for gap links first, then barrels.

   **Jumps** (3.2.2, follow-up 7). MAP02 played through by hand first (the trace): the red key's
   area is reached by a running jump west over a pit 96 wide (sector 73, floor -40, 8 below the
   takeoff floor) into a window 96 wide (line 248) with a ceiling 72 high; flown 64 units, at
   least 9.1 a tic, where running gives 16.7. Agreed with the user: one model, straight jumps
   only, a margin, monsters cleared first (later); measured: no bloat (3.2.2).
   - MAP02: the jump worked at once (off the edge at (1453,251) at 13.0 a tic, landed at
     (1388,251)). Two older gaps showed up behind it, on ground the bot had never reached: it
     waited 2 s at each switch that lowers a bar (sectors 15 and 16), since a floor going down was
     no crossing to wait at, and the bars' far faces made the crossing look too narrow until they
     were down (3.2.1, floors that come down); and the fast door between the switches shut while
     it waited, so the blue key was given up for good (3.5, a way that shut on the way).
     **Finished in 42.2 s.**
   - MAP24 (in the regression set, 60.4 s) found a shortcut by jump: from a teleporter's arrival
     platform over the nukage onto a walkway 16 wide, and fell off it, again and again. Three
     things: the check stopped at touchdown (now it brakes and must stop on the landing); the
     run-up crossed an earlier route cell, which sent the bot back from the edge; and flying over
     a later route cell cut the corner after the landing, straight over the nukage (3.4, jumps on
     the route). **Finished in 62.1 s**, by the jump.
   - Regressions: everything finishes. New: MAP02 42.2 s, MAP21 62.5 s, MAP17 152.3 s (it now
     gets past the step that stopped it). MAP12 74.1 s (77.8). The rest unchanged. Seen by the
     user, not looked into yet: on MAP21 the bot lowered a lift in a nukage pit, did not step on,
     called it again, and only then rode it.

   **The maps waiting for jumps, one by one** (watched by the user; findings noted, fixed after
   the run-through):
   - MAP04: stalled before the jumps, at a walk-over switch in a corner. The red key's lift
     (sector 38, tag 15) is lowered by crossing lines 408/409, two sides of a box 16 square and 16
     high in room 50's north-west corner, 8 units from each wall. Crossing them puts the player's
     centre in an 8x8 spot on the box, exactly its width from both walls; with Karp's 17 units of
     clearance that is no way at all ("a gap narrower than the player at (-632,1884)"). A human
     runs into the corner and the walls slide it onto the box: the last step across a walk-over
     line could aim a few units past it and drive straight in, without the clearance margin.
     Also a loop: with no way, the brain waits while sectors move, and the only one moving was
     the fast door it had just opened itself (sector 77, special 114, shut after 4 s), so it
     pressed door switch 388 every 5 s. A door the bot opened and that will only close again
     should not count as a reason to wait. (The spot was enough, the goal on it was not: see
     "MAP04 finishes" below.)
   - MAP06: **finished in 79.3 s**, after two fixes. The blue key is reached by a long jump from a
     lift (sector 122) south over a pit, 304 units, which was not found: a run straight south
     touches down on a pillar's top 16 square level with the lift and could not stop there, so
     the check threw it away, where the engine carries the player on (now it runs on, 3.2.2). And
     the walk-over line onto the lift (183, WR) lowers it, so the jump works only once it is back
     up (the user): now a ride, waited at the run-up's start. The red key takes a second jump.
   - MAP08 ("Tricks and Traps"): **finished in 26.9 s**, after five runs and a hand-played trace
     (the user). Four jumps were not found or not taken: the hop off the strip behind the yellow
     door needs no run-up to speak of (22 units of floor: no minimum run-up now); walk-over line
     599, crossed on the way to the red key, lowers the island the way back starts from (sector
     80) into the slime, so that jump did not work, with no fix sought (now a blocked crossing,
     fixed by switch 572, which raises it); the jump back from the yellow key's ledge only exists
     once its door is open (a search again when a sector stops moving); and the corridor of
     sinking pillars to the exit starts from a strip 8 wide behind the red door, closed at level
     load (doors count open on the takeoff floor). Then the pillars: each sinks a unit a tic once
     stepped on, and the bot backed up along each for its run-up and braked after each landing;
     now it hops on with a run-up of 8 and does not stop on a sinking floor, turning north at the
     end as the user described (3.2.2, 3.4).
     Seen, not fixed: the bot pressed switch 572 while the island was still finishing its way
     down from line 599. The engine ignores a switch on a busy sector and leaves it usable (S1
     changes its texture only if it starts the mover), but the brain counted the old mover as the
     press working, found the jump still not working, and walked back to press again (about 2.3 s
     lost). A switch whose sector is still moving should be waited for before it is pressed, or a
     press that did not take (the line kept its special) should not count as done.
   - MAP09 ("The Pit"): **finished in 105.2 s**, after a hand-played trace (the user). At first the
     bot gave up at once: the yellow key is reached over three jumps that only exist once a
     sector has moved (floor 6 raised by switch 126, door 33 opened by switch 157, the stairs from
     line 380 with door 101 opened by switch 455), so it saw no way at all. Now they are found as
     if those sectors had moved, and their fixes are the switches (3.2.2, 3.5). Two more things
     were needed: rays at 45 degrees (the jump out of door 33 runs along its wall onto a ledge off
     to the side), and wall sliding in the run (floors 6 and 4 touch only at a corner of door 8,
     closed until a line past floor 4 opens it; the player slides along the door's face to get
     across). Stairs were already known (k_specials follows the chain). The bot built the stairs,
     raised floor 6, rode lift 12 and jumped onto it, crossed at the corner, opened doors 8, 3 and
     33, jumped out of 33, got the blue key, opened door 101 from switch 455 and went up the
     stairs onto its sill, as the user did.
     Seen, not fixed: the jump out of door 33 left the edge at 3.8 units a tic, where the check
     has 13.5, 38 units off its line along the door's north wall, and fell short; the bot walked
     round (6 s) and made it the second time at 4.4, landing on the corner of the ledge. And on
     the way to the exit it worked switches 442, 403 and 428 twice each (the user): lift 123,
     lowered first, was back up by the time door 130 further on was open again. Fixed by "lifts
     last" (3.5): **94.6 s**, every switch once.
   - MAP11 ("Circle of Death"): **finished in 91.2 s**, after a hand-played trace (the user). At
     first the bot gave up at once: switch 615, which lowers the way to the exit, stands on a
     ledge 136 above the nukage, reached up spiral stairs behind the red door and a fence of
     pillars 16 apart (sectors 23-27, 34, 35). Walk-over line 516 opens the four pillar doors
     (blazing, they shut again) and line 539 lowers the three pillar lifts; either leaves gaps 56
     wide. The gap between two pillars is a crossing narrowed by them, which the brain could fix,
     but each gap is 24 long and pinched inside its own cell too, and a pinch cost as a wall, so
     the cheapest route was the unfixable step from the nukage. Now a pinch is fixed like a narrow
     crossing (3.2.1, 3.5). The bot got the blue key, worked switches 433 and 462, jumped onto the
     red key's ledge, crossed line 516, went through the fence and up the stairs to switch 615,
     and on to the exit over two more jumps.
     Seen: after switch 615, USE TRIGGER for sector 4 failed ("no line moves the sector any
     more"): the switch had moved it too, though not low enough to step onto. Harmless, the exit
     goal found its way by jumps.
   - MAP13 ("Downtown"): **finished in 98.3 s** as it was, then **93.2 s**. The red key lies on a
     platform (sector 165, 392) in a pit (164, 200) ringed by a ledge (163, 448), which walk-over
     line 902 lowers for good. Right after crossing it, the bot planned to teleport into the pit,
     wait for the ring to come down within a step of the pit, get on it and jump onto the
     platform: but the jump needs the ring high. Now a jump off a floor sinking away is a gap
     (3.2.2), and the bot goes for the blue key and the teleporter onto the platform at once.
     Not a fault: the yellow key is reached over a perpetual lift (sector 20, lines 918 and 952
     start and stop it) that runs 384 units at a unit a tic, about 25 s down and up.
     Seen, not fixed: after the jump towards the blue door (line 1077), the bot stood at the door
     for a second, stuck, before a replan opened it; perhaps a waypoint in front of the door that
     the box cannot quite reach after the landing.
   - MAP14 ("The Inmost Dens"): **finished in 59.9 s**, after a hand-played trace (the user; 2:02
     with some wandering). The bot looped among self-closing doors (WR blazing doors opened by
     the walk-over lines in front of them) and switches. Three things: the red key is reached by
     a jump out of a window (sill 118, 16 deep, 64 wide) over a courtyard, a fence and water,
     with the run-up in the corridor a step below, and the margin's side starts always hit the
     low sills beside the opening (3.2.2: run-ups from a step below, starts moved to the middle);
     once switch 416 had opened the door on the way, the goal on top was still a switch for
     another way round (3.5: goals below with an open way drop those above); and a sub-goal
     whose switch was behind the red door being fetched kept opening the doors in front (3.5:
     no fix past a dead end, lifts last only without a missing key). Now: switch 416, the red
     key over the window, switches 533 and 1500, the blue key, doors 155 and 71, the exit.
     The jump search on MAP14 takes 160 ms (152 jumps, 21 more as if a sector had moved).
   - **Regression after MAP06-MAP14** (22 maps in parallel: the 16 of the set and MAP06, 08, 09,
     11, 13, 14). 19 finished: MAP01 8.2 s, MAP02 42.2, MAP03 33.5, MAP05 31.1, MAP06 80.4, MAP08
     27.0, MAP09 97.6, MAP10 45.9, MAP12 80.1, MAP13 93.3, MAP14 59.9, MAP16 42.0, MAP17 152.7,
     MAP18 32.9, MAP19 101.9, MAP21 56.1, MAP26 49.2, MAP27 79.8, MAP31 41.4. Three did not (the
     user watched and quit them), all from jumps found as if a sector had moved, starting from a
     cell far below where they take off (3.2.2), which the routes had not met before:
     - MAP11 (91.2 s before): after both keys, a loop of "running up for the jump to (504,2429)",
       "missed the jump: down at (589,2339), 112 below", until the player died in the nukage.
       The jump off strip 20 (lowered by switch 615) started from the nukage's cell, its centre
       over the nukage; dropping needless goals now has the bot at switch 615 30 s earlier, from
       where the way on went through the nukage to that cell.
     - MAP24 (62.1 s before): the same after the red key, the jump to (-1060,1385) missed 128
       below. The user saw the bot leave the last teleporter and wade through the nukage pool
       instead of keeping to the ledge: the jump found as if line 332 had raised the pool starts
       from the pool's cell, its box standing on the strip beside it, so it worked as things
       were.
     - MAP28: stuck at (-2,-857) by lift 82, which line 402 had raised; GET RED KEY failed after
       three stucks, then GO TO EXIT. The jump found as if line 402 had raised the lift, a step
       onto its edge, started from the floor round it, 64 below, and the bot pushed against the
       lift instead of calling it.
     Fixed both ways (3.2.2): the jump's cells are those the player stands on by the floors of the
     search, and a jump from a cell more than a step below its takeoff is no jump yet (a ride off
     a lift coming back up, else a gap). Either alone fixes MAP28. Again all 22: all finish, MAP11
     in 64.9 s, MAP12 74.2 s (was 80.1), MAP24 59.4 s, MAP28 72.7 s, the rest within 0.1 s. That
     also covers the "missed the jump" loop: a route no longer starts with a jump whose start the
     player is below. Jotted: a run that never leaves the ground still counts as a jump when its
     centre passes over a floor more than a step below while its box stands on the takeoff
     (MAP28: from the lift's edge onto the lift, cell 432 to 429, both at floor 0). Harmless with
     the right cells (a walk does it too), but it is no jump.
   - MAP15: **finished in 48.6 s** with no changes, through the secret exit (lines 1666-1669, a
     W1 box to MAP31): the yellow key, switch 634 (the stairs), switch 1135 reached by a jump,
     walk-over line 1199, and a second jump on the way to the exit. At the end 4 of 8 exit lines
     were reachable, presumably the secret box's four, so the normal exit (lines 1686-1689) looked
     out of reach; not looked into, either exit is fine for now (follow-up 14).
   - MAP20: GO TO EXIT failed at once (line 557, which lowers the way to the exit, behind a step of
     264). The user played it through (the trace, 3.10, 82.9 s): line 271 opens door 187 to a
     teleporter pad inside walk-over lines 300-303, which lower block 30 from 399 to the walkway
     (176) as the pad teleports the player onto it; the block rides the player down, and line 557
     is a few steps away. On the way to line 271, the jump the user remembered: off the foot of a
     walkway stepping down to 160, over 240 units of blood, onto a ledge at 72. It needed long
     run-ups down steps with a smaller margin (3.2.2, the user's choice over strafe-running or
     skipping the map). Then the bot walked to it over the blood floor and died: floors that hurt
     now cost what they take (3.3). **Finished in 67.2 s**, by the user's way: switch 73 for door
     120, the teleporter, the jump (off at 16.7 a tic), line 271, the pad onto block 30, line
     557, lines 513 and 512, the teleporter to the exit. Seen, not fixed: after line 557 the bot
     dropped into the blood strip in front of sector 26 and waited there 10.4 s for it to come
     down (the user waited on the walkway); the wait is only possible from the blood side, so
     where to wait is for nav to choose. And the route to line 380 on MAP09 reads "about 63 health
     off floors that hurt" (no dry way, so taken anyway): to be checked whether that is real or
     an overestimate of a lift wait. (It was the lift: see the MAP29 fixes below.)
   - Regression with MAP15 and MAP20 (24 maps; the long run-ups and floors that hurt): all finish.
     Unchanged within 0.1 s: MAP01-05, 06, 08, 10, 11 (64.9), 12, 13, 14, 16, 17, 18, 19, 27, 31.
     Slower: MAP09 101.9 s (97.6; the missed hop out of door 33 in 3.4), MAP21 56.9 (56.1), MAP24
     60.2 (59.4), MAP26 50.7 (49.2), MAP28 73.9 (72.7). MAP15 48.4 s, MAP20 67.2 s. Both added
     to the script's default list.
   - MAP22: **finished in 42.7 s** with no changes: switch 535, switch 14 (twice), the blue key
     past a lift ride and two teleporters, switches 188, 269 and 289, then lift 25 ridden back up
     and the 16-unit hop onto the red key's ledge that stopped the first pass (a jump across a gap
     narrower than the player, 3.2.2), and the exit switch past two more teleporters and a lift.
     Seen: the routes to the blue key and to the exit were planned with about 38 and 26 health
     off floors that hurt, each under half the health then, so taken; together about 70 with no
     monsters about. Radiation suits (follow-up 15) and keeping count of health would help.
   - MAP25: GO TO EXIT failed at once, as in the first pass (the niche at line 523). Drops past a
     cell with no room to stand in (3.2.2) fixed it: **finished in 59.9 s**. The drop off the
     channel's end into the pit, switch 410 and lift 111, a teleporter, the blue key; the long way
     to walk-over line 622 goes back up by the pad's teleporter and down the drop again, then
     lift 111, door 528, lift 105, line 622, switch 645 and its lift to the exit.
   - Regression, 26 maps (MAP22 and MAP25 added to the script's default list): all finish, every
     time as in the run before drop links.
   - MAP25 again, the user: the drop looks awkward, planned through a solid wall and down only by
     sliding along it, where the geometry is a plain step off the ledge (the niche's floor runs on
     into the pad's). Drop links removed; cells too thin to stand in (3.2) instead: the bot walks
     off the channel's end and drops 344 at line 523, **finished in 54.3 s** (5.6 s faster), 28
     jumps again. On the way, two fixes from the regression maps: lifts left out (MAP12's
     perpetual lifts, MAP05's strip in front of a raised lift), and the crossings judged by the
     same floor (MAP22: on a strip round a crate that had sunk 8 below it, the bot was told of an
     80 step). MAP03, 05, 06, 11, 12 and 22 as before. MAP09 96.2 s (101.9): after the same jump
     that does not work, the new plan finds another jump and gets the blue key 6 s sooner. MAP14
     68.9 s (59.9): the way to switch 393 now walks along a raised strip and works the first time;
     before, a jump on it failed, and on the replan the bot crossed walk-over lines 1366 and 1383,
     whose doors happened to open a shorter way to the blue key. With a way already there, the
     brain does not look for shortcuts behind triggers (follow-up 16).
   - Regression, 26 maps, with cells too thin to stand in: all finish. As before but MAP09 96.2
     s, MAP14 68.9 s, MAP19 101.1 s, MAP22 42.8 s, MAP25 54.3 s, MAP27 79.4 s.
   - MAP29: **finished in 162.7 s** at the first try with jumps, through the normal exit (it
     failed in the first pass for want of the running jump onto the island). Line 10, a long
     walk with two teleporters to line 350, switch 395, a jump onto a ledge at 600 and switch
     508, then lift 116 (13 s) and switches 686, 696, 688, 758, 583, 655 and 666 among lifts;
     then the three pillar switches 975, 825 and 837, with six jumps between them, and the
     running jump from the ledge at 528 onto the island at 480 (2:27.8), its teleporter and the
     staircase to the exit hall. Seen: (1) dropping 476 off the ledge at 600 into sector 116, a
     blood channel about 100 across that switch 508 raises a unit a tic, the bot flew on out of
     it into the blood pit beside it (sector 0, level with the channel then), and the only way
     back was the long way round by both teleporters and the jump again: about 11 s; a long fall
     carries the speed of the run-in. (2) The route to the exit was planned with about 117
     health off floors that hurt, more than the player has; the steep search found no drier
     way. The bot lived (no monsters, the guess counts waits it did not make), but with
     monsters, or less health, it would not (follow-up 15). Added to the script's default list
     (cap 200 s).
   - MAP29's two issues, fixed. (1) Drops that the fall could carry off the floor the route
     lands on are left slowly (3.4): the bot lands in the channel and rides it up 24 s (no time
     saved, since the channel rises no faster; the detour had fitted into the wait). Slowing
     every drop over 48 was tried first and cost the regression maps 25 s; slowing only those
     with a further edge below the landing missed MAP29's, since the pit was level with the
     channel when judged (the user saw the bot fly into the pit again). (2) A lift lowered by
     its switch had its whole round trip put down as waiting in the cell it is called from;
     its wait at the bottom and the ride up are spent aboard (3.3): the route to the exit reads
     about 43 health. That was also MAP09's "about 63 health" to line 380, now 23 by a way 89
     units shorter. The ride up the blood channel itself (24 s, some 130 health) was planned
     as about 15; the route is right (the user: the other way, by the lifts in the south-west,
     is far longer), so it was let be. Regression, 27 maps: all finish, as before but MAP09
     96.7 s, MAP17 152.4 s, MAP26 51.1 s (its 216 drop beside a teleporter is now slowed),
     MAP29 162.6 s.
   - MAP04 finishes (35.4 s): of the maps that did not, only those needing monsters or barrels
     shot are left (MAP07, 23, 30, 32). The box in the corner
     (lines 408 and 409) could be crossed all along; the goal for crossing was 24 units past a
     line's middle, which was on a wall, and the way to it went through the 8-unit gap between
     the box and the wall. Now a spot past a walk-over line is one the player can stand on, as
     near as 4 units past it, and arriving there means having crossed (3.4, line goals). The
     stepping stones to the red key needed nothing new: the jump finder hops them (9 units off
     each edge), after the bot rides lift 38 back up. Then the user saw the bot open door 32 (a
     fast door, shut after 4 s), go back past the start for switch 244, and press 32 again on
     its way to the blue key: a door that closes again is now fixed last, like a lift, and a
     door further on opened only by switches needing a key counts as a locked door, its key
     first (3.5); the bot goes door 64, switch 244, door 32, blue key (10.4 s, was 12.6), and
     takes the yellow key before the exit's lift. Regression, 28 maps (MAP04 added): all
     finish; as before but MAP02 41.9 s (the red key a step earlier), MAP17 152.9 s (walk-over
     line 1200 instead of 1201, for the new spots), MAP19 101.7 s (the same route).
   - Waiting dry (follow-up 15.3, 3.4): the bot no longer idles on a floor that hurts in front of
     a crossing that opens by itself when a dry floor lies on the way before it. MAP20 waits 8.2 s
     on the walkway instead of 10.4 s in the blood for floor 26. The level's end now logs the
     health left, and the regression prints it. Regression, 28 maps, against the same build with
     the change off: the same in all but MAP20 (67.4 s, 95 health; was 67.2 s, 50) and MAP21
     (88 health, was 93: the same route, a tic earlier from the first seconds, from the build
     alone, as without the change; a hit of nukage falls differently). Tried and left out (3.3):
     counting such waits as less in the route's damage and time (MAP12, MAP27 and MAP29 went
     other ways; MAP29's south leg, whose tricky jump failed, the user).

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
      are not part of the geometry. In the engine a solid thing is a box of its radius, of any
      height (no heights are checked), and the player does not slide along it as along a wall:
      it stops dead. Planned with the user in three parts:
      1. **Things that stay put go into the map.** Done (3.2, 3.2.1, 3.4): every solid thing
         that is not a monster is a block, its square cut out of the cells like a pillar, the
         cells under it judged live (a barrel blown up opens the way); steering goes round
         them. Doom II MAP03 finishes; MAP17 gets all its keys; MAP19 finished once its key bars
         could be opened together; MAP23 needs more (see the playtests above).
      2. **Shooting barrels**: a wall of things that can be destroyed. Barrels block like
         torches, but the brain knows the wall can go. Designed with the user (the first combat
         code); the user's points, with what the engine says:
         - **Avoid it unless it pays.** A crossing into a barrel's cell, and a gap or pinch a
           barrel narrows (MAP23's exit: the barrel stands mid-way, not in the way's cell), is
           no longer unfixable: it costs time like a door, the aiming and shooting, the wait for
           the chain, a reluctance penalty (about 10 s, tunable) and the predicted damage turned
           into time. So a 30 s detour loses to blowing the barrels, a 5 s one wins.
         - **The blast** (`A_Explode`, `P_RadiusAttack`): a barrel (20 health, radius 10)
           explodes 15 tics after it dies, damaging every shootable thing it can see whose box is
           within 128: 128 less the larger of the x and y distances, less the thing's radius.
           The player (radius 16) is hurt within 144; a barrel takes 20 or more within 118, so
           it dies too and goes off 15 tics later: the chain, and its timing, follow from that.
           Armour takes a third (green) or half (blue). Karp may not call `P_CheckSight` (2.4),
           so it needs a sight check of its own on its map data.
         - **Mind the distance; the chain.** Shoot from where the predicted damage is least: out
           of the chain's sight (round a corner, through a doorway, as the user did on MAP23),
           else far enough. Keep away while the chain runs, without spending long on it: chains
           usually run away from the player, and it outruns them (16.7 units a tic against one
           barrel every 15 tics).
         - **If the way needs it, do it**, even with barrels close: accept the damage while
           health stays above a margin; else it is not fixable yet. Later: fetch health first
           when low.
         - **Weapon**: the pistol or the chaingun (the first shot of a burst is accurate), the
           shotgun up close; never the fist or the chainsaw.
         - Done as a nav action at the crossing, like a door: walk to the firing spot (a clear
           shot, the least predicted damage), aim, fire until the barrel is gone, wait or keep
           away while the chain runs, replan.
         - **For later, with monsters:** shoot a barrel when monsters are near it (and not in
           the line of fire) for its blast; keep a bit away from barrels while monsters are
           about; monsters can set barrels off too.
         - First test before MAP23 can be finished: a click into the shotgun closet north of the
           start, behind a fence of barrels with 12-unit gaps, to be shot from 64-96 units away
           with the whole hall chaining.
         MAP23's start hall was thought to need a hardcoded route or a soulsphere (the user's
         first note); played through, it needs one barrel shot, from a spot out of sight.
      3. **Monsters** are never built into the map: they move. When one is in the way and is not
         to be killed, the steering that goes round blocks goes round it too, with some random
         strafing against the randomness of how monsters move (the user's note), then waiting
         for it to move, then a replan round it. Built with combat, since `-nomonsters` leaves
         nothing to test it on.
   4. **Downhill and drops.** Going down stairs the player keeps gaining speed: each step down
      leaves it briefly airborne, with no friction and no control, so it overshoots the target,
      and next to a pit that can mean falling in. Slow down earlier for crossings that drop
      (like a corner), and brake harder where it matters. Partly done (3.4): near an edge, the
      waypoints where the route turns or steps down are taken carefully. Not yet: braking that
      knows it cannot brake while airborne, on a long stairway down.
   5. **Narrow paths.** Some maps need narrow walkways and bridges, e.g. Doom II MAP24's long
      ledges only 4-8 units wide over the nukage. Entering a narrow portal or cell, slow down
      enough to stay on it. Doom II MAP03's raised strip to the teleporter is exactly the
      player's width: once the bot drifted a unit off it (and still stood on it), once it fell
      off and had to go round. Partly done (3.4): careful corners near edges, braked for early and
      taken drift first (MAP24's 8-unit ledges now hold), and wall avoidance no longer pushes the
      player off a ledge it hangs over (likely MAP03's fall too).
   6. **Stuck recovery.** Before replanning when stuck, block the crossing ahead for a while, so
      the new route really differs. If the player stays stuck too long, try semi-random moves in
      different directions, strafing included, to work free. Also loops that are not stuck:
      dropped back into an earlier route cell, the player follows the same route again
      ("back in route cell" in the log), so a fall that repeats repeats forever (MAP16 before
      the ledge fix in 3.3). Count them, and plan round the place after a few.
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
      So does MAP22: from a raised lift to a ledge at the same height, 16 units away. And
      MAP25: off the start platform, across a 16-unit niche, dropping 344 onto a teleporter pad.
      And MAP23: from the raised triangle (sector 46, a lift at 128) into the east strip (sector
      35, 128) at its corner, over a sliver of hall floor; the line between them is 16 units long
      beside a wall, so the player's centre passes over the sliver while its box stays on the
      128 floors (the user: the only way into the east part).
      By the user's choice, gaps and jumps were to come **last**, after the other follow-ups.
      But twelve of the first twenty-two Doom II maps need them (MAP02, MAP04, MAP06, MAP08,
      MAP09, MAP11, MAP13, MAP14, MAP15, MAP20, MAP21, MAP22; MAP14's over several gaps in a
      row), and so do MAP25 (a gap) and MAP29 (a running jump onto an island, about 180 units
      from a ledge 48 higher), so they will probably have to come sooner. Jumps would also save
      time where falling in is not needed: Doom II MAP17's ditch at switch 491 costs a 15 s ride
      back up (optional, the user's note; see the MAP17 playtest).
      **Done as one model** (3.2.2, 3.3, 3.4): gap links and running jumps are both straight runs
      checked by the engine's rules, found at level load and judged live. MAP02 and MAP21 finish
      (MAP24 takes a jump too). MAP06, MAP08, MAP09, MAP11, MAP13 and MAP14 finish too; jumps that
      only appear once a sector has moved are searched for again when it stops, and at level load
      as if it had moved (3.2.2). MAP15, MAP20, MAP22 and MAP25 finish too (MAP20 with long
      run-ups down steps, MAP25 walking over a cell too thin to stand in, 3.2), and so does
      MAP29 (its running jump onto the island), and MAP04 (its stepping stones, once the bot
      could get onto the box that lowers their lift; see the playtests).
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
      for the lift right away. Other floors on their way down (`T_MoveFloor`, not a lift) were
      not judged as crossings to wait at, but when no way is left while anything still moves,
      the brain waits and plans again (3.5), so the bot no longer gives up on a step that is
      shrinking (Doom II MAP13). Now they are (3.2.1, floors that come down): Doom II MAP02's
      bars lowered by switches had the bot standing 2 s at each switch.
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
   11. **Never stuck for good** (the user's rule). The bot never decides it is stuck for good:
       it keeps looking for a way, and when there is none it idles (later: fetches weapons and
       ammo meanwhile) and tries again. A map can look blocked and still open later through
       something out of the bot's hands: a monster in a distant room crossing a walk-over line,
       a very long moving floor, a lift a queue of monsters keeps turning back (3.4 already never
       gives up on a ride). Limits against thrashing stay, but they pause and retry instead of
       ending things. What still gives up for good today, all in the brain:
       - a goal whose candidates all fail is failed for the rest of the level, never pushed
         again; a failed GO TO EXIT is not added again, and the bot stands idle until a click
         (Doom II MAP19: GET BLUE KEY failed at the start, and the blue key was never tried
         again after the red key's switches, though it lies 570 units from the red one);
       - a candidate that did not work out (nav failed, stuck, presses that moved nothing) is
         never chosen again for that goal;
       - a line is worked at most 3 times a level;
       - waiting while sectors move ends after 15 seconds with a failure.
       To do: failures, tried candidates and line uses expire (after a while, or when the world
       changes: a sector moved, a door opened, the player is somewhere new), and an emptied
       stack idles and plans GO TO EXIT again from time to time.
   12. **Lasting fixes first, passing ones last.** The brain works the first blocked crossing on
       a route, and only then sees the next one. When that first fix does not last (a lift goes
       back up after 3 s, a door closes after 4) and a later crossing needs a long way round
       first, the first fix is wasted. Doom II MAP27: the route to the red key is blocked first by
       an 8-unit gap beside the raised lift 185 (tag 18), then by a 192-unit step whose switch
       (line 727) is behind the blue door, with the blue key behind the yellow one. The bot spent
       4.6 s going to the walk-over lines round the skull rock, lowered the lift, then turned
       away for the yellow key (switch 90), and the lift was long back up when it next came by.
       It lowered the lift again at 13 s, for the blue key, and that time went on through. The
       key-first rule (3.5) already covers a locked door further along the route. The same is
       wanted for any later blocked crossing: look at the whole route, and work a fix that does
       not last only when nothing after it needs work first. Seen in a one-by-one run. Agreed
       with the user, but for later: not worth risking a regression now.
   13. **Doors that close before the player gets there.** A door that is open now counts as
       open wherever the player is, but a normal door (DR) closes about 4 s after it opens, and
       from a side it cannot be opened from (a one-way door) the way is then shut. Doom II MAP27:
       going north for the blue key, the bot opened the yellow door at line 626 (sector 27; its
       north face, line 751, has no special) at 31.7 s. At 35.2 s it planned the way back south
       through it, to switch 610, and arrived at 38.1 s with the door closing (46 units of
       headroom), found no way and gave the switch up. The brain then found another way, so the
       map still finished, but the switch stays given up for the level (follow-up 11). **By the
       user's choice, closing times are not predicted:** if the door shuts first, so be it, the
       bot plans again as a human would. A door can also stay open longer than its timer says:
       a closing door goes back up when it meets a monster, so a prediction would give up on
       doors too early once monsters are in. What is left to do is only that a way shut like
       this is not given up on for good (follow-up 11). Partly done (3.5): when nav's way shuts
       on the way and a blocked route is still there, the goal is planned again, which pushes
       the switch (Doom II MAP02's fast door, its only blue key given up before). A one-way
       door like MAP27's has no switch to push from behind, so that case still falls to
       follow-up 11.
   14. **Which exit.** GO TO EXIT takes every exit line or switch as a candidate, normal or
       secret, and goes for the cheapest it can reach (Doom II MAP15 finished through the secret
       exit to MAP31, the normal one looking out of reach). Either is fine for now (the user).
       Later, perhaps a command line switch: prefer the secret exit, prefer the normal one, or
       let the bot choose (as now).
   15. **Floors that hurt, next steps** (the user's list, after the stand-in cost of 3.3; the
       user had not meant to do the nukage logic yet, but keeps it):
       1. A jump over a floor that hurts can still hurt (an engine quirk): marginal, since jumps
          are short, but when one hit from that floor (5, 10 or 20) could kill the player, such
          a jump should cost so much that it is taken only as a last resort.
       2. A radiation suit can leak (another engine quirk, far rarer): no code for it.
       3. Setting off right after a press (3.5) sends the bot to the lift or floor it moved, to
          wait there; where that is on a floor that hurts, it is better to wait by the switch, or
          anywhere dry, until the wait is nearly over. Seen in MAP11's starting pit, and at the
          slowly sinking floor by the blood lake on MAP20 (10 s in the blood for sector 26). The
          bot should not idle on a floor that hurts. Done for the way ahead (3.4, waiting dry):
          the user's point, that sometimes the only way out is a run through the nukage at once
          to catch a lift, is kept by judging it every tic. MAP11's pit no longer waits (its lift
          is fast, down in 0.3 s; the whole pit is nukage anyway). Going back to a dry spot from
          a floor that hurts was measured and left out: over the regression set the bot stands
          still on such a floor 6 times, for 0.3-1.5 s, at most a hit each, and the nearest dry
          floor is up a ledge it dropped from (MAP09's second visit to the pit) or further than
          the wait pays back (MAP09's start: the platform some 190 units off, the lift down 0.7 s
          after the switch). Still to do: the brain's waits for moving sectors, and the route's
          damage counting waits nav spends dry (3.3).
       4. Radiation suits as something to fetch when the way crosses a floor that hurts: MAP24's
          bot drops into the big nukage pit and runs across it to the teleporter, past a suit
          nearby that would save health. Suits often lie on the floor that hurts themselves, so
          not all damage can be avoided.
   16. **Shortcuts behind triggers.** The brain looks for a trigger only when a goal has no way;
       with a long way already open, it never asks whether a nearby walk-over line or switch
       would open a shorter one. Doom II MAP14: after switch 393 the blue key is 2077 units off,
       by two lifts; crossing lines 1366 and 1383 opens doors to a way of 1452 (found only by
       luck, on a replan after a failed jump: 9 s).
8. **M7 Priorities tab** - the goal stack shown in the console is done (3.5, 3.7); still to come:
   history, cancel, polish.

## 5. Verification

- Build: VS Code task **build crispy-doom** (Release x64, output in `build/src/Release/`).
- Run (Doom II):
  `build\src\Release\crispy-doom.exe -iwad $env:DOOM_IWAD -warp 01 -nomonsters -autoplay -karpconsole`
  (Doom 1: `-warp 1 1`).
- Map regressions: `karp\tools\regress.ps1 -Iwad <DOOM2.WAD>` (or set `DOOM2_IWAD`) runs MAP01,
  02, 03, 04, 05, 06, 08, 09, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 24, 25, 26, 27,
  28, 29, 31 (all finish, in about 165 s) in parallel, with
  `-quitonexit -nosound` added, and prints per map the finish time and health left, keys and
  first failure; the
  logs land in `build\karp-regress\logs`. Each instance runs from its own folder holding a copy
  of the exe, its DLLs and cfgs: crispy keeps `karp.log`, `karp.cfg` and `crispy-doom.cfg` in the
  exe's folder, which instances sharing it would overwrite. Seven maps take 77 s instead of over
  five minutes, with the same results to the tenth of a second as one at a time. `-Maps '24'
  -Seconds 150` runs other maps. A build that only computes differently (the same decisions)
  can still shift a map by a tic, a tenth of a second or a hit of nukage (MAP14, MAP21).
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

Monsters and combat (the next stage, section 7); remote doors and lifts, switches, walk-over triggers and collecting keys
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

## 7. Combat (the next stage)

> Status: C0, C1 and the first of C2 done (7.9): Doom II MAP01 and MAP02 finish with monsters.
> Monsters are back (no `-nomonsters`), tested at `-skill 3`.

Four Doom II maps do not finish with `-nomonsters`: MAP07 and MAP32 open their way only when
monsters die (follow-up 10), MAP30 ends when the boss brain dies, and MAP23 needs a barrel shot
(follow-up 3.2). And once monsters are in, every map needs fighting. Combat is more than shooting
on sight: the bot has to manage ammo and weapons, choose what to fight and what to leave, and stay
alive. None of it can be computed exactly at runtime, and it need not be: heuristics, survivable
rather than perfect (the user). Perfect missile avoidance while dancing on a narrow ledge is not
the aim. The design below is the user's, with the engine facts that shape it.

Two new parts of the brain, on top of the goal stack (3.5) and nav (3.4):
- the **condition** (7.1): how the player is doing (ammo, health, armour), which steers how bold
  the bot is and which detours it takes;
- **combat** (7.2-7.4): what the bot sees, what it fights and with what, and how it moves in a
  fight.

Files: `k_sight.c` (sight checks, 7.2), `k_combat.c` (perception, targets, weapons, battle mode)
and `k_condition.c`, with plain C headers for the console like the others. Their decisions go to
the log under `COMBAT` (3.8; `ITEMS` to come with pickups), the condition's changes under `BRAIN`.

### 7.1 The condition

Three needs, each judged on its own, each with an urgency:
- **ammo**: OK or LOW, for the weapons the player holds;
- **health**: OK, LOW or CRITICAL;
- **armour**: OK or LOW (none, or worn down).

The **condition** is the worst of them, in the user's order from the lowest priority to the top:

| Condition | When | What the bot does |
|---|---|---|
| COMFY | every need OK | runs, fights, solves the puzzles, goes to the exit; takes pickups on the way (7.5) |
| LOW AMMO | ammo LOW | carries on, but detours for ammo and weapons; thinks more about switching weapons, and melee where it works |
| LOW HEALTH | health LOW | as LOW AMMO, and detours for health and armour |
| CRITICAL | health CRITICAL | survival: health is the top goal, fights are escaped |

Armour LOW alone does not change the condition: it makes armour worth a detour once health is low,
and a pickup on the way always. A single ladder would hide needs: low on health and on ammo at
once, LOW HEALTH would walk past the shells it badly needs, so the needs stay separate and each is
weighed where it matters. The console shows every need side by side next to the condition (the
user), e.g. `COMFY - ammo OK, health OK, armour LOW`, and the log says when one changes.

- **Hysteresis**: a need goes LOW below one threshold and back to OK only above a higher one
  (say below 30 health, above 45), so a stimpack does not flip it back and forth.
- **CRITICAL with no health to get** (none left, or none reachable) must not mean running about
  for good, against the never-give-up rule (follow-up 11): careful play instead, fighting from
  cover, starting no new fights, carrying on with the goals.
- **Invulnerability**: while it lasts nothing hurts the player (`P_DamageMobj` ignores damage
  under 1000, floors that hurt included), so the health need waits until it runs out.
- **The health budget**: the cost of floors that hurt (3.3) is a stand-in until the bot knows its
  health, armour and suits. The condition gives that budget: how much health the route can spend.

For a start the condition is always COMFY (the user): the needs are worked out and shown, and
acted on only from C4 (7.7).

### 7.2 What the bot sees

- **Monsters**: the bot knows every one, as most players know where the monsters stand on the
  classic maps (the user): no exploring to uncover them. Each tic, a list from the thinkers (reading
  mobjs is allowed, 2.4): type, position, velocity, health, awake or asleep (it has a target), what
  it is doing (walking, winding up an attack, in pain, dead), and whether the player can see it.
  The bot plans with all of them, but aims and fires only at the ones it can see.
- **Projectiles** (`MF_MISSILE`): position, velocity, who fired them, the damage they do.
- **Sight checks of our own**: `P_CheckSight` bumps `validcount`, so Karp may not call it (2.4).
  Done as a copy of it: the REJECT table first, then the trace through the BSP, in the engine's
  fixed-point maths, reading the level read-only and marking lines with marks of its own. Barrels
  need the same (follow-up 3.2).
- **Seeing is not hitting**: a monster is a target only if the player's shots would find it -
  from the gun's height (8 above the player's middle, not the eyes), within the autoaim's reach up
  and down (`P_AimLineAttack`: a slope of 100/160), through the openings on the way. Seen only by
  the eyes, a row of a monster's pixels over a ledge, below a window or through a gap in a floor
  drew shot after shot into the wall (the user, Doom II MAP02).
- **Console**: monsters on the map (awake, asleep, seen), projectiles, the current target and the
  line of fire.

### 7.3 Fighting

The user's points, with what the engine says:

1. **Ammo supply.** Counted per type. The bot keeps a shot or two for gun switches still to work,
   but only for those the fist cannot reach: the fist sets them off too (`P_LineAttack` at
   `MELEERANGE`, 3.4), so only gun switches high up or across a pit need bullets or shells. The
   brain counts them from the lines still ahead.
2. **Weapons.** A table per weapon: damage, hitscan or projectile, splash, the range it is good
   at, ammo per shot.
   - A switch costs about 0.9 s (`A_Lower` and `A_Raise` move the weapon 96 units at 6 a tic, 16
     tics down and 16 up), so weapons are chosen with hysteresis, or fights are spent switching.
   - The pistol's and the chaingun's first shot after the trigger was let go is dead accurate
     (`P_GunShot` with `!player->refire`): single taps far off, bursts up close.
   - Vertical aim is automatic (vanilla autoaim, `P_AimLineAttack`): only the angle matters.
   - Melee (the chainsaw, the berserk fist) against weak monsters when ammo runs low.
   - **No rockets at anything closer than about 160 units**, monsters and walls alike (the blast
     reaches 128 from where it hits), nor near barrels by the player. The exception: while
     invulnerable, rockets everywhere (the user), as long as enough of it is left for the rocket
     to land.
3. **Fight or avoid.** A Cyberdemon with only the pistol in hand is not fought: avoiding means not
   starting the fight, keeping moving, getting out of its sight. **Far monsters are ignored** (the
   user): chasing a far hitscanner wastes time and ammo. If a far one keeps hitting the bot, the
   bot gets out of its sight rather than going after it. Firing wakes monsters wherever the sound
   reaches (`P_NoiseAlert` in `P_FireWeapon`, the fist too); that matters for sneaking past, later.
4. **Target priority.** Hitscanners first: the zombieman, shotgunner, chaingunner and Spider
   Mastermind, whose bullets cannot be dodged. Then arch-viles (they raise the dead, and their
   flame), pain elementals (they spawn lost souls), then by threat: the damage a monster can do,
   how close it is, how soon it would die. A monster within melee reach goes before that.
5. **Dodging, projectiles only.** A hitscan hit is decided the moment the monster fires. Against
   hitscanners: kill them first, keep the distance (their spread is an angle, so fewer hits far
   off), break line of sight (6). Projectiles (imps, cacodemons, barons and knights, mancubi,
   arachnotrons, revenants, the Cyberdemon) can be seen coming: for each one heading at the
   player, its closest approach and how soon; if it would hit soon, step to the free side. Never
   off a ledge or into a floor that hurts (`K_Graph_Clearance`, `K_Graph_DropDistance`): on a
   narrow ledge, take the hit. A revenant's homing missile: break sight, or step aside late.
6. **Breaking line of sight**, against hitscanners and arch-viles.
   - A zombieman winds up for 10 tics before it fires, so only cover right beside the player
     works then; between fights, keep cover near.
   - The arch-vile's flame does nothing if it loses sight of the player before it lands, 66 tics
     after the attack starts (`A_VileAttack` checks sight).
   - Cover spots: nearby floor out of the threat's sight, sampled and sight-checked a few times a
     second. Peek, shoot, step back.
7. **Never stand still** in the sight of awake monsters, the more so when there are more than the
   bot can manage: running about semi-randomly is a good way to survive in Doom (the user).
8. **A bullet or two for gun switches**: point 1.

### 7.4 Battle mode

Who controls the player in a fight (the user's model):
- **On the way**, nav follows the route as now. Combat may aim and fire at a monster in sight, but
  only where nav does not need the facing: never during running jumps, presses (use, gun
  switches) or careful legs along a ledge. The facing also changes the top speed: 16.7 units a
  tic forward, 13.3 strafing alone, about 21 both at once. Nav's speed model must use the real top
  speed for the current facing, or jump run-ups, waits on a dry floor and lift timing go wrong.
- **Battle mode**: when awake monsters close enough to matter can see the bot, or monsters stand
  in the way, the bot switches to battle mode. It leaves the route and clears the area: combat
  controls movement and facing, target by target, strafing, dodging, using cover. A leash keeps
  it from wandering far after monsters. Once the area is clear (or what is left is far, asleep,
  or to be avoided), the bot goes back to its goals: the brain plans the top goal again from where
  the player stands, since the chase may have taken it well off the route (the user).
- **Strafe movement** (the user): circling a target, side to side, backing off, all while facing
  it. The controller already moves the player in any direction whatever the facing (3.4: the
  thrust is split into forward and side), so these are new movement patterns on top of it, not a
  new controller: battle mode hands it a velocity and a facing.
- **Monsters in the way** when not fighting: follow-up 3.3 (steer round them with some random
  strafing, wait for them to move, plan round them).
- **Fleeing** (CRITICAL): a goal pushed for health, or for a spot out of sight.

### 7.5 Goals combat adds

- **Pickups on the way**, in every condition: items within a small detour that are useful now.
  Weapons and backpacks always, ammo when not full, armour when better than what is worn, health
  only when not wasted (no medikit at 95). Cheap, natural, and the LOW conditions come less often.
- **Detours** for the needs (the temporary goals in front of the chain, 3.5): GET <item>, its
  candidates every item of the kind in the level, scored by route and by danger (a guarded
  medikit loses to a lonely one further away).
- **KILL ALL <kind>** (follow-up 10): Doom II MAP07 and MAP32, Doom's E1M8, E4M6, E4M8.
- **Barrels** that block the way are removed by shooting them (follow-up 3.2), with the same
  sight, aiming and firing. MAP23 needs nothing of its own (the user).

### 7.6 Testing

- **`-skill 3`** (Hurt Me Plenty, the game's default), the neutral level (the user): `-skill 1`
  halves the damage and doubles ammo pickups, `-skill 5` doubles them too and brings fast,
  respawning monsters; skills 1-2 place the easy set of monsters, 3 the medium, 4-5 the hard.
- **On death, stop**, and log the cause: what hit the player, from where, the health over the last
  seconds. Later, once the bot plays fairly well, it restarts a few times by itself and the logs
  are compared for improvements (the user).
- **Runs with monsters cannot be compared exactly across builds**: a tic's difference changes how
  every monster moves (they draw from the same random numbers as everything else). Compare rates
  over several runs instead: finished, died, health, time.
- **The `-nomonsters` regression stays**, and guards: with nothing to fight, combat code must leave
  it identical, until pickups on the way (C4) change routes on purpose.

### 7.7 Order

1. **C0 What the bot sees**: monsters, projectiles, the sight check, the console overlay. No change
   in behaviour. Done.
2. **C1 Shooting on the way**: pick a target, aim where nav allows, fire, the weapon table, the
   rocket rule. Death handling, and runs with monsters on a few early maps. Done.
3. **C2 Battle mode**: clearing the area, strafing, never standing still in sight, monsters in the
   way, back to the goals. Done but for monsters in the way.
4. **C4 The condition**, next (the user, after MAP02: health and dodging will take the bot much
   further, health first): the needs, pickups on the way, detours, CRITICAL.
5. **C3 Dodging and cover**: projectiles (on MAP02 the bot stood in slow imp fireballs); breaking
   line of sight against hitscanners and arch-viles.
6. **C5 The rest**: fight or avoid; barrels that block the way (MAP23); KILL ALL goals (MAP07,
   MAP32); MAP30.

### 7.8 Later, or left out

- **EXPLORER** (the user's idea, jotted): when nothing is left to do but leave, the bot may choose
  to explore: secret areas, far bonuses, to finish the map with full health and armour. A goal in
  front of GO TO EXIT, taken when the condition and the time allow.
- Left out for now: exact missile simulation (closest approach and a free side are enough), sound
  (what a shot wakes, for sneaking past), infighting, BFG tricks, predicting where monsters walk
  beyond a few tics.

### 7.9 Done so far

- **The condition** (7.1) is worked out every tic and shown on a line of its own under the status
  line, every need side by side, ammo as b(ullets), s(hells), r(ockets), c(ells) (the user: the
  long form did not fit); changes are logged. Nothing acts on it yet. Marks: health LOW below 50
  until above 60, CRITICAL below 25 until above 35; armour LOW below 25 until above 40; ammo LOW
  when no ammo the player has a weapon for is at its low mark (bullets 20, shells 5, rockets 3,
  cells 40), OK again above 30, 8, 5, 60.
- **What the bot sees** (7.2): every monster and projectile each tic, which monsters the player
  can see and which its shots would find; on the console's map (Monsters overlay) and in a Combat
  section of the Bot tab. Hits taken, monsters dying and the player's death are logged (`COMBAT`).
- **Shooting on the way** (7.3, `k_combat.c`): the target scores by the kind's threat
  (hitscanners first), awake, attacking, near, little turning to it, a little for being the
  target already and for being wounded (the user: a zombieman came up a corridor unshot while
  the bot kept to a wounded monster further off). None further than 1024, asleep ones only within
  512. Aimed at where it will be when the command runs; fired once the facing after the
  command's turn is on it, and the shot along that facing would not meet a wall (it can graze a
  corner the centre is clear of, the user). The weapon is the one doing most damage a tic,
  counting no more than kills the target; the shotgun's rating falls off beyond 400, the super
  shotgun's beyond 300; another weapon is brought up only at twice the rating and 3 s after the
  last switch, unless the one in hand cannot hurt the target (with a gain of 1.3, 24 switches in
  a minute on MAP02). Five shots without a hit and the target is left alone for 2 s (the user).
- **Facing** (7.4): nav takes combat's aim wherever it does not need the facing: not running a
  jump (and coming up to one, it faces the jump: on MAP02 a run-up begun facing a shotgun guy
  left the edge at 13.2 units a tic and fell short), not pressing at a switch, door or lift; on
  a lift ride it aims, unless the ride ends in a jump.
- **Battle mode** (7.4): a target in sight for 8 tics in a row, awake within 512 (or asleep and
  near), stops the route (`K_Nav_Pause`); the player strafes side to side within 128 of where the
  fight began, keeping the target where its shots find it and open floor before a passage (the
  user saw it peek in and out of a corridor), backing away from awake monsters within 192, never
  over a drop or onto a floor that hurts. A second with nothing worth shooting and nav plans its
  route again from where the player is. With a reach of 768 and no steady-sight rule, monsters
  420 units off showing through a window for a tic or two stopped the bot 58 times on MAP02.
  A target not hurt for 5 s in a fight is left alone for 10 s.
- **On death** the run stops with `-quitonexit` (the user); the regression script's `-Monsters`
  plays at `-skill 3` and reports kills and deaths.
- **Results**, `-skill 3`: MAP01 in 31.7 s, 95 health, 11 of 19 killed (shooting on the way
  alone: 9.2 s, 77 health, 3 killed, stuck once on a zombieman in the way). MAP02 in 97.3 s, 16
  health, 60 of 70 killed, CRITICAL for its last minute (without battle mode it died at 15 s,
  walking into a group of shotgun guys). MAP03-MAP05 died within 13 s with shooting on the way
  alone, not tried since. The `-nomonsters` regression is unchanged.
- Seen, not fixed: a run-up was still slow once while aiming (13.0 units a tic at the edge on
  MAP02, though it landed).
