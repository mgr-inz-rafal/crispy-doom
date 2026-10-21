# Karp - Stage 1 Plan: "Go To Location"

> Status: approved, not yet implemented. Work proceeds milestone by milestone (section 4).
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
5. The bot plans a path over subsectors and drives the player there.

Stage 1 assumptions: no monsters (`-nomonsters`), no doors/lifts/switches on the route, no
strafe-jumps, no narrow-corridor/player-radius handling. Static geometry only.

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
  | G_BuildTiccmd end --------+--command-->| k_path.c    A* over subsector graph      |
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
    k_map.h / k_map.c         map reader + subsector polygons + nav graph
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
| H2 | `src/doom/d_main.c` `D_GrabMouseCallback` (:505) | `if (K_OwnsControls()) return false;` | While the bot drives there is nothing for the mouse to do, so a windowed game leaves the cursor free to reach the console. A fullscreen game still grabs (crispy decides that before asking the callback); Alt+Tab releases it. |
| H3 | `src/doom/d_main.c` `D_DoomLoop`, right after `I_InitGraphics()` (:611) | `K_PostVideoInit();` | With `-karpconsole`: create the console and install the event filter. Whenever Karp is on: register `I_AtExit(K_Shutdown, true)` |
| H4 | `src/doom/p_setup.c` end of `P_SetupLevel`, before the closing brace (:1402) | `K_LevelLoaded();` | Rebuild the map snapshot and nav graph, reset brain and nav, bump the level generation |
| H5 | `src/doom/d_net.c` `RunTic`, after `G_Ticker()` (:93) | `K_Ticker();` | Think once per game tic on fresh state (read-only) |
| H6 | `src/doom/g_game.c` end of `G_BuildTiccmd`, after the lowres block (between :981 and :982) | `K_BuildTiccmd(cmd);` | Write the bot's input into the command before it enters the network layer (section 3.6) |
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
- `subsectors[]`: sector index, closed convex polygon, area-weighted centroid, bbox, edge list.
- `portals[]` and a directed adjacency list (see below).
- Map bounds for the console view.

**Closed subsector polygons.** In vanilla, DeePBSP and XNOD nodes a subsector's segs only cover its
linedef edges; edges along partition lines are missing, and nothing in the engine rebuilds them.
One algorithm handles every node format:
1. Start with the map bbox, padded, as a polygon.
2. Recurse from the root node (`numnodes-1`). At each node, split the polygon by the partition line
   (`x, y, dx, dy`): the right side goes to `children[0]`, the left side to `children[1]`. This
   matches `R_PointOnSide`, which returns 0 for points right of `(dx,dy)`. Stop at
   `children & NF_SUBSECTOR` (`0x80000000`).
3. Clip the leaf polygon by each of the subsector's seg lines, keeping the right side of
   `v1 -> v2`. Every format keeps the subsector on the right of its segs: side-1 segs are already
   reversed, and XGL formats chain `v2` into a closed loop. This trims the void that partition
   clipping leaves behind one-sided walls. Two-sided segs lie on partitions, so clipping by them
   changes nothing. Skip zero-length segs.
4. Compute area and centroid (shoelace). Use a small epsilon in clipping (~1/256 unit).
5. A map with `numnodes == 0` (a single subsector) uses the bbox clipped by its segs.

**Leaves without floor.** Real nodebuilders leave BSP leaves that have segs but no usable floor.
A polygon has "no usable floor" when its area is below 0.01 or its average width (area over half
the perimeter) is under 1 map unit; nothing fits there, since the player is 32 units wide. Each
leaf is classified:
- **No area** (harmless, no polygon): the BSP region itself has no usable floor. These are leaves
  from splitting twice along the same line, and hairline slivers from vertex rounding.
- **Behind walls** (harmless, no polygon): the region has floor, but the leaf's one-sided segs
  alone remove all of it. The leaf is solid space inside a pillar or behind a thin wall.
- **Degenerate** (warning): the floor lies behind one of the leaf's *two-sided* segs (buggy
  nodes). The engine still assigns that floor to this subsector, so the polygon is the BSP region
  clipped by the one-sided segs only, which keeps the void out.

Measured on 100 maps (Doom, Doom II, Plutonia; ~54,000 subsectors): 2,129 leaves have no area,
229 are behind walls, and Plutonia MAP08 has 2 degenerate subsectors. All 100 maps have 0
unreached subsectors and 0 centroid mismatches. Builds take under 1 ms per map. Maps with
extended node formats (DeePBSP, XNOD, XGL) are not tested yet: no local WAD uses them.

**Portals and adjacency.** Two subsectors are neighbours where their polygon edges are collinear
(tolerance 0.5-1 unit, because vanilla split vertices are rounded to whole units), opposite in
direction, and overlap by at least 1 unit. Edges are bucketed in a uniform 128-unit grid so large
maps stay fast. Each overlap becomes a portal segment, which path smoothing will need later. If the
portal lies on one of the subsector's segs that has a linedef, the portal records that linedef;
otherwise it is a miniseg (same sector, always open). One-sided walls never produce portals,
because the void behind them was clipped away.

**Passability** of a directed edge A -> B is evaluated **live** at search time. It is cheap, and it
makes doors and lifts work naturally in later stages. It mirrors `P_TryMove`/`PIT_CheckLine`
(`src/doom/p_map.c:212-285, 564-631`):
- blocked if the crossed linedef has `ML_BLOCKING`;
- blocked if `floor(B) - floor(A) > 24` (step-up). This is directional: dropping down is always
  fine, because players have `MF_DROPOFF`;
- blocked if `min(ceil A, ceil B) - max(floor A, floor B) < 56` or `ceil(B) - floor(B) < 56`.

**Load-time sanity checks** (shown in the console's Map tab and logged): leaves without area,
leaves behind walls, degenerate subsectors, subsectors not reachable through the BSP tree,
`R_PointInSubsector(centroid) != ss` mismatches, build time in ms; from M4 also portal count and
isolated subsectors. The first few problems of each kind are logged individually with their
location.

`K_Map_SubsectorAt(x, y)` finds a point's subsector via `R_PointInSubsector` (a pure lookup),
then returns -1 if the point lies outside that subsector's polygon (more than 1 unit away), i.e.
in the void. The console's hover info and click validation use it.

### 3.3 `k_path.c` - pathfinder

- A* over subsectors. Start = the subsector containing the player; goal = the subsector containing
  the clicked point.
- Edge cost = `|centroid(A) - portal_mid| + |portal_mid - centroid(B)|`. The heuristic is the
  straight-line distance between centroids, which is admissible by the triangle inequality.
- Binary-heap open set; per-node arrays sized to `numsubsectors`, reset with a generation counter.
- Output waypoints: `portal_mid(1), centroid(2), portal_mid(2), ..., portal_mid(k), goal_point`.
  This keeps the barycenter-to-barycenter idea, but adds the portal midpoint between each pair.
  Every leg then stays inside one convex subsector, so no leg can cut through a wall corner.
  (Without the midpoints, the straight line between the centroids of two adjacent cells can leave
  both cells.)
- Also returns the list of path subsectors, used for progress tracking.
- An unreachable goal returns `K_PATH_NONE`; the priority is marked FAILED with a reason in the log.
- Later: funnel/string-pulling over the portal list, with portals shrunk by the player radius, for
  smooth runs.

### 3.4 `k_nav.c` - path following and steering

Physics facts the controller is built on (`p_user.c:151-216`, `p_mobj.c:160-302`):
- thrust per tic = `move * 2048 / 65536` units, so forwardmove 50 gives 1.5625;
- friction 0.90625; running top speed ~16.7 units/tic; coasting stop distance ~161 units;
- the turn in a ticcmd is applied **before** thrust in the same tic;
- there is no thrust while airborne.

**Think - `K_Nav_Tick()`, from `K_Ticker` (once per game tic):**
- **Progress:** find the player's current subsector in the path list and move the waypoint cursor
  there. This handles overshoot and corner cutting. Also advance when within 32 units of an
  intermediate waypoint.
- **Off-path:** if the current subsector is not on the path (fell off a ledge, got pushed), replan
  from the current position.
- **Stuck:** if the player moved < 8 units over the last 35 tics while a goal is active, replan.
  After 3 replans in a row without progress, mark the priority FAILED.
- **Arrival:** within 16 units of the goal and speed < 1 -> report DONE.

Every state change is logged (section 3.8).

**Act - `K_Nav_BuildCommand()`, from `K_BuildTiccmd`:**
1. **Predict.** In single player a command is built 1-3 tics before it runs (`d_loop.c:169`), and
   more in netgames. Karp keeps a small ring of the commands it emitted, tagged with `maketic`.
   The pending ones (tagged `>= gametic`) are replayed from the current player state with the
   physics model above (turn, thrust if on ground, move, friction). Walls are ignored, which is
   fine over 1-3 tics. The result is the predicted position, momentum and angle at the moment
   this command will execute.
2. **Velocity controller** on the predicted state:
   1. Desired velocity `v_des = dir_to_waypoint * speed`, where `speed = 16.7` toward
      intermediate waypoints and `speed = min(16.7, k * distance)` toward the final goal (k ~0.12,
      tunable).
   2. Required thrust `T = v_des - momentum`.
   3. Decompose `T` into the player frame using the **post-turn** angle `a'`:
      `forward = T . (cos a', sin a')`, `side = T . (sin a', -cos a')`.
   4. Scale to command units (`* 32`). If either axis exceeds its limit (forward +-50, side +-40),
      scale **both** axes down uniformly so the thrust direction is kept, then round.

   Because movement comes from strafe decomposition, it does not depend on facing, and
   counter-thrust brakes automatically.
3. **Heading:** turn toward the direction of travel, at most 2048 angleturn units per tic (about
   11 degrees; tunable in the console). This is cosmetic for now; later stages need it for aiming
   and using.
4. IDLE, DONE, FAILED or no path -> zero movement.

### 3.5 `k_brain.c` - priorities

- Stage 1 types: `IDLE`, `GOTO_LOCATION {x, y, ss}`.
- `k_priority_t { type, params, status (PENDING/ACTIVE/DONE/FAILED), score, created_tic, reason }`.
- One active priority. A console click replaces any current GOTO. DONE and FAILED fall back to
  IDLE, and the finished entry stays in a short history list.
- The interface is designed for later stages: each priority type has `evaluate()` (a score from the
  world state, e.g. low health -> medkit) and `update()` (tells nav where to go). The highest score
  wins.
- `K_Brain_RequestGoto(x, y)` is called from the console. It finds the subsector with
  `R_PointInSubsector`, then rejects void clicks with a point-in-polygon test (~1 unit tolerance).
  Rejections are logged. Without `-autoplay` (viewer mode) the GOTO is planned and drawn, but no
  command is ever written.
- `K_Brain_Status()` returns a one-line description of the current intent for the console status
  line, e.g. `GO TO (1024, -320) - 14 waypoints left, 812 units`.
- IDLE means zero movement (the bot owns controls).

### 3.6 `K_BuildTiccmd` - command injection (H6)

Called at the end of `G_BuildTiccmd`, after crispy's fliplevels and lowres handling, so Karp writes
final, world-space values into the command that is about to be queued and sent. If the gate fails,
the human command is left untouched (intermissions, menus and so on behave normally). Otherwise:
1. Compute the command (`K_Nav_BuildCommand`, prediction included) and record it in the pending
   ring with its `maketic`.
2. Always write `forwardmove`, `sidemove` and `angleturn`; set `lookfly`, `arti`, `lookdir` to 0.
3. If `cmd->buttons & BT_SPECIAL` (pause/unpause, save), keep `buttons`; otherwise clear them.
   Movement is still overwritten in that case, so no human movement leaks through.
4. If `lowres_turn` (demo recording without `-longtics`), round `angleturn` to a multiple of 256
   and carry the remainder into the next command, as crispy does for the keyboard.
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
  | GO TO (1024, -320) - 14 waypoints left, 812 units        |  status line (K_Brain_Status)
  +--------------------------------------+-------------------+
  |                                      | [Priorities][Bot] |
  |            map canvas                | [Map]             |
  |                                      |                   |
  +--------------------------------------+-------------------+
  | 00:41.2 BRAIN  new priority GOTO (1024,-320) - user click |  rolling log pane
  | 00:41.2 NAV    path found: 23 subsectors, 1840 units      |  (always visible,
  | 00:43.9 NAV    off path in ss 211 - replanning            |   resizable splitter)
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
  - Overlays (checkboxes): subsector polygons (hashed fill colors), centroids, nav graph (green
    open, red blocked), and the path polyline with the current waypoint highlighted.
  - Player arrow (live position and heading) and goal marker.
  - Hover tooltip: subsector #, sector #, floor/ceiling, neighbour count.
  - **Left click -> `K_Brain_RequestGoto(x, y)`.**
- **Side tabs:**
  - `Priorities`: active priority, status, history, Cancel button.
  - `Bot`: position, speed, current subsector, path length, waypoint index, controller tunables.
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
  - `BRAIN priority GOTO failed - no path (target ledge 64 units above)`
  - `NAV path found: 23 subsectors, 1840 units, 0.3 ms`
  - `NAV off path (in ss 211) - replanning`
  - `NAV stuck at (512,-96) for 35 tics - replanning (2/3)`
  - `MAP E1M1: 482 subsectors, 1210 portals, 0 degenerate, 0 centroid mismatches, 4.1 ms`
  - `SYS click at (3000,4000) rejected - outside the map`
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
6. **M5 Pathfinder** - A*; a click draws the planned path and logs it (no movement yet).
7. **M6 Driving** - H2, H5, H6, H7, prediction, controller, arrival, replanning, stuck detection.
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
path smoothing with player radius; narrow corridors; strafe-jumps; damaging-floor costs; lifting
the netgame guard; Heretic/Hexen.
