# Building Crispy Doom on Windows (MSVC + CMake + vcpkg)

Build notes for the **Karp** project. Versioned in the repo so the setup does not
get lost; lives under `karp/` to keep it out of upstream's tree.

The VS Code tasks that automate this are in `.vscode/tasks.json`, which *is*
gitignored (upstream's `.gitignore` excludes `/.vscode/`) because it contains
machine-specific absolute paths. If you set up a second machine, recreate it from
the commands in this file.

## Just build it

Ctrl+Shift+B, or Terminal > Run Task > **build crispy-doom**.

That configures (if needed) and builds all targets as Release x64. Output lands in
`build/src/Release/`:

| Executable | What it is |
|---|---|
| `crispy-doom.exe` | Doom / Doom II / Final Doom |
| `crispy-heretic.exe` | Heretic |
| `crispy-hexen.exe` | Hexen |
| `crispy-strife.exe` | Strife |
| `crispy-setup.exe` | Config tool (controls, sound, display) |
| `crispy-server.exe` | Dedicated netplay server |
| `mus2mid.exe`, `midiread.exe` | MUS/MIDI helper tools |

vcpkg copies the needed SDL2 DLLs next to the executables automatically, so they
run in place.

## Other tasks

- **build crispy-doom (Debug)** - Debug build with symbols, in `build/src/Debug/`
- **configure crispy-doom** - re-run CMake by hand (safe any time)
- **rebuild crispy-doom (clean)** - delete `build/`, then run a build task
- **run crispy-doom** - build, then launch Doom with the `DOOM_IWAD` IWAD and the
  Karp Console open (`-karpconsole`)
- **run crispy-setup** - build, then launch the config tool

The two build tasks and **run crispy-doom** are all in the build group, so
Ctrl+Shift+B lists them to pick from (no task is marked default).

### Why the run tasks launch through `cmd /c start`

Pointing a task straight at `crispy-doom.exe` looks correct but leaves VS Code
stuck on `Executing task: ...` forever after you quit the game. The game is not
at fault - it exits cleanly with code 0 and leaves no stray process - VS Code
simply never registers that the GUI process ended. The visible cost is that
relaunching demands you terminate the task first, and `"close": true` never helps
because VS Code still believes the task is running.

So the run tasks launch the game **detached**:

```jsonc
"command": "cmd.exe",
"args": [ "/c", "start", "Crispy Doom", "...\\crispy-doom.exe", "-iwad", "${env:DOOM_IWAD}", "-karpconsole" ]
```

`cmd` returns in milliseconds, so the task genuinely finishes, the terminal
closes, and you can relaunch as often as you like. The `"Crispy Doom"` argument
is `start`'s window title - it also keeps `start` from mistaking the quoted exe
path for one.

Consequences worth knowing: the game is no longer a child of VS Code, so you
cannot stop it from the task list, and nothing prevents launching several copies.
`"reveal": "silent"` keeps the launcher terminal from flashing up.

The build tasks keep the normal attached behavior with `close` off, so compiler
output stays on screen after a build.

You can also open `build/Crispy Doom.sln` in Visual Studio 2022 directly; the
CMake configure generates it.

## You need an IWAD to actually play

The executables build and link fine without one, but the game needs a commercial
IWAD (`doom.wad`, `doom2.wad`, `heretic.wad`, `hexen.wad`, `strife1.wad`, ...).

On this machine the Doom IWAD path lives in the **`DOOM_IWAD`** environment
variable (set machine-wide), and the **run crispy-doom** task passes it through:

```jsonc
"args": [ "-iwad", "${env:DOOM_IWAD}" ]
```

So Terminal > Run Task > **run crispy-doom** builds and launches straight into
the game. If the task starts and immediately errors about a missing IWAD, the
variable did not reach VS Code - restart VS Code, since a machine-level variable
is only picked up by processes started after it was set. To check what it holds:

```powershell
$env:DOOM_IWAD
[Environment]::GetEnvironmentVariable('DOOM_IWAD','Machine')
```

To run a different game or IWAD by hand:

```
build\src\Release\crispy-doom.exe -iwad D:\path\to\doom2.wad
build\src\Release\crispy-heretic.exe -iwad D:\path\to\heretic.wad
```

Crispy Doom also finds IWADs on its own via the `DOOMWADDIR` / `DOOMWADPATH`
environment variables and from Steam/GOG install paths in the registry - note
those are Crispy's own built-in variables, whereas `DOOM_IWAD` is ours, read only
by the VS Code task. For a free option, `freedoom2.wad` works.

## Toolchain this was set up against

- Visual Studio 2022 Professional 17.14, MSVC 14.44 (x64)
- CMake 3.31.6 and Ninja, both bundled with VS (not on PATH - tasks.json uses
  the full path to VS's `cmake.exe`)
- vcpkg at `E:\Git\vcpkg`

## Dependencies

Installed lean - the libraries needed to actually play, skipping the heavy ones:

```powershell
cd E:\Git\vcpkg
.\vcpkg install sdl2 sdl2-mixer sdl2-net --triplet x64-windows
```

Run that from the vcpkg directory, **not** from the repo. This repo ships its own
`vcpkg.json`, and vcpkg switches to manifest mode whenever that file is in the
working directory - which rejects package names on the command line and instead
tries to build the full manifest, including fluidsynth and its glib dependency.

For the same reason the configure task passes `-DVCPKG_MANIFEST_MODE=OFF`.
Without it, CMake would kick off a from-source build of libpng, libsamplerate
and fluidsynth.

### What the lean set gives up

CMake reports these as not found, which is expected and harmless:

| Missing | Consequence |
|---|---|
| `libpng` (+ zlib) | screenshots save as PCX instead of PNG |
| `libsamplerate` | lower-quality audio resampling |
| `fluidsynth` | no SoundFont MIDI playback (OPL emulation and native MIDI still work) |

To add them later, install the ports and re-run the configure task:

```powershell
cd E:\Git\vcpkg
.\vcpkg install libpng libsamplerate fluidsynth --triplet x64-windows
```

Expect a long one-time build for fluidsynth/glib.

## Known-harmless build noise

- A CMake deprecation warning about `cmake_minimum_required(VERSION 3.7.2)` -
  upstream's, not ours.
- MSVC C4244/C4267 conversion warnings in the Dehacked and Strife code - also
  upstream. The build completes with zero errors.
- `strcasecmp`, `strncasecmp` and `dirent.h` reported as not found. Correct for
  MSVC; the tree has Win32 replacements in `win32/`.

## Branches

- `master` - clean mirror of upstream `fabiangreffrath/crispy-doom`, fast-forward
  only. The `upstream` remote points at it. Don't commit Karp work here.
- `karp-master` - the Karp integration branch. Feature branches come off this.

## The autotools route, for reference

Upstream CI builds Windows via MSYS2 + autotools (`./autogen.sh && make`), not
MSVC. See `.github/workflows/main.yml`. The CMake path used here is the
MSVC-friendly alternative; if you ever need to match CI output exactly, that's
the one to reach for.
