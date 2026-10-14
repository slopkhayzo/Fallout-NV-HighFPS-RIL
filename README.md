# FalloutNVHFR: high-frame-rate patch for Fallout: New Vegas

FalloutNVHFR lets Fallout: New Vegas display at 120, 240, 360 FPS or more
while the game logic keeps running at a fixed 60 ticks a second. Everything
that moves is interpolated between the last two ticks, and mouse look updates
on every frame.

It is a port of the Fallout 3 patch (Fallout3HFR 1.0.0) and ships as an ASI
plugin.

**Current version: 1.0.1.** Download it from the
[Releases page](https://github.com/slopkhayzo/Fallout-NV-HighFPS-RIL/releases).

## Download

Each release has two zip files:

| File | Contents | Use it if |
| --- | --- | --- |
| `FalloutNVHFR-1.0.1.zip` | `FalloutNVHFR.asi`, `FalloutNVHFR.ini`, `FalloutNVHFR-README.txt` | You already have a 32-bit ASI loader |
| `FalloutNVHFR-1.0.1-with-ASI-Loader.zip` | The same files, plus Ultimate ASI Loader as `dinput8.dll` and its licence | You have no ASI loader yet |

## Why

When the unmodified game runs above 60 FPS:

- it runs faster than real time, about 40 % fast at 105 FPS;
- its timers sleep until 10 ms have passed, which caps the frame rate at
  roughly 100 to 140 FPS;
- it receives time in coarse, uneven steps (`GetTickCount` resolution), so
  motion looks like about 60 FPS even when the counter says more.

Tick fixes such as New Vegas Tick Fix (NVTF) make the engine step at the
frame rate instead. This patch does not. The engine never steps faster than
60 Hz in normal play, because the game is unstable when it does. Only the
picture runs faster.

## What it does

**Simulation**

- Fixed 60 Hz tick from a `QueryPerformanceCounter` accumulator, at the
  correct game speed.
- Frames between ticks are "frozen": the engine skips world, actor and
  physics updates, but still reads the mouse, moves the camera and draws.
- Havok's step is aligned with the tick.
- The 10 ms timer sleeps are removed.
- Key and button presses are latched to ticks, so none are lost on frozen
  frames.
- Mouse acceleration of the menu cursor is removed. This can be switched off.

**Interpolated at the display frame rate**

- The player's position and the first- and third-person cameras, including
  crouching, falls, view animation and aiming (field of view)
- First-person arms and weapon, including aiming down the sights and crouch
  transitions
- The player's body in third person
- Other characters and creatures, including ragdolls
- Moving objects in the loaded cells, including doors
- The sky
- The VATS and kill cameras, including slow motion

Every value written for a draw is put back after the draw, and only if the
engine has not changed it in the meantime. The engine's state is never left
modified by the interpolation.

## Requirements

- **Fallout: New Vegas 1.4.0.525.** Developed and tested on the GOG release
  only. The patch does not check a hash. It checks the code layout and accepts
  any executable that matches. If the layout does not match, the patch
  changes nothing and logs why. Steam and Epic executables have not been
  tried.
- **A 32-bit ASI loader**, for example
  [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
  as `dinput8.dll`.
- **A frame rate limit outside the game**, from the graphics driver or a tool
  such as RTSS. Tested with a limit of 240.
- NVSE is not needed.

**Not compatible with NVTF or New Vegas Stutter Remover.** Both patch the
same timer. If either is found in `Data\NVSE\Plugins`, FalloutNVHFR does not
install. If another mod patches the same call sites later, the patch switches
itself off on the first frame.

## Install

Extract the zip into the folder that contains `FalloutNV.exe`. The files sit
at the top level of the archive:

```text
FalloutNVHFR.asi
FalloutNVHFR.ini
FalloutNVHFR-README.txt
dinput8.dll                       (loader bundle only)
Ultimate-ASI-Loader-LICENSE.txt   (loader bundle only)
```

If you already have a `dinput8.dll` from another mod, do not overwrite it.
Keep your own ASI loader, or rename the bundled one to another name it
supports, such as `d3d9.dll` or `winmm.dll`.

The `.asi` may also go in the loader's `plugins`, `scripts` or `update`
folder. The `.ini` must be next to the `.asi`. To switch the patch off without
removing it, set `bEnabled=0`.

The player readme, [packaging/FalloutNVHFR-README.txt](packaging/FalloutNVHFR-README.txt),
covers uninstalling, troubleshooting from the log, and the known limitations
in full.

## Settings

The settings players may change, from [packaging/FalloutNVHFR.ini](packaging/FalloutNVHFR.ini):

| Setting | Default | Meaning |
| --- | --- | --- |
| `bEnabled` | 1 | Master switch. 0 runs the game unmodified. |
| `bTelemetry` | 0 | Writes a status block to `FalloutNVHFR.log` every 5 s (several MB an hour). Turn it on only to report a problem. |
| `fSimulationHz` | 60 | Leave at 60. Higher values are refused. |
| `bMenuRealTime` | 1 | Menus, conversations, loading and the 30 frames after them run on real frame time. Set to 0 if you get crashes or physics glitches right after closing a menu. |
| `bMenuMouseNoAcceleration` | 1 | Plain, unaccelerated menu cursor. |

Every other mechanism has its own switch under the "Internals" headings. They
exist so a faulty mechanism can be turned off without a new build. Leave them
at their defaults.

## Known limitations

- Menus, conversations, loading and the 30 frames after them are stepped at
  the frame rate (see `bMenuRealTime`). For about a quarter of a second after
  a menu closes, motion is not yet interpolated.
- The picture runs up to one tick (1/60 s) behind the simulation. Mouse look
  is not delayed.
- Below 60 FPS the game runs slower than real time.
- With the 9 mm pistol, the weapon shakes for about 300 ms at the start of
  aiming down the sights. The cause has not been found.
- The "spider hands" bug (stretched fingers at high frame rates) is not
  corrected.

## Changes

**1.0.1**

- First person: the arms and weapon no longer step and trail the view while
  aiming down the sights or crouching and standing up. The first-person pass
  is drawn from the interpolated camera bone; it sets its field of view
  through a function that recomputes the camera from its local transform, so
  the local transform is written too. The arm skeleton's world transforms are
  rebuilt from the blended local transforms, checked against each node's
  parent, because the engine adjusts `Bip01 Spine2` after computing it and
  rebuilds the skeleton during the render only on some frames.
- VATS playback and kill cameras: the camera's whole transform is
  interpolated between ticks.

**1.0.0**

- First release.

## Building

You need Visual Studio with the MSVC x86 toolchain and CMake 3.20 or later.
The plugin is 32-bit only, so configure from an x86 developer prompt
(`vcvarsall.bat x86`):

```bat
cmake -S . -B build-x86 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-x86
```

This produces:

- `FalloutNVHFR.asi`: the plugin, C++20 with a static runtime;
- `asi_smoke.exe`: a smoke test that loads the plugin outside the game and
  checks that it recognises the wrong executable and changes nothing.

```bat
build-x86\asi_smoke.exe build-x86\FalloutNVHFR.asi
```

## Repository layout

| Path | Contents |
| --- | --- |
| [src/FalloutNVHFR.cpp](src/FalloutNVHFR.cpp) | The whole plugin: layout checks, hooks, fixed tick, input latch, interpolation, logging |
| [tests/asi_smoke.cpp](tests/asi_smoke.cpp) | Smoke-test loader |
| [packaging/](packaging/) | Release INI and the plain-text readme that ship in the zip files |
| `research/` | Not tracked: the port plan, analysis scripts, captures, reference sources and tools |

## How it works

Each frame passes through two hooks on whole `call` instructions:

1. **Frame hook**, around the engine's per-frame function. It adds the wall
   time to an accumulator and decides whether this frame is a tick frame or a
   frozen frame. On a frozen frame it sets the engine's own freeze-time flag.
2. **Timer hook**, on the main timer update. It gives the engine exactly one
   tick or almost zero time.

Around the main world draw and the first-person render, the plugin writes
blended transforms using `alpha = accumulator / tick`. It saves the engine's
bytes and restores them after the draw. Each kind of object is blended in a
space where the per-frame mouse rotation cancels out: world translation for
the camera, parent-relative transforms for the player's body, local
transforms for first-person bones. The plugin measures which draw each kind of
value changes on, so it is blended with the right phase.

At installation the plugin checks fixed byte patterns ("layout anchors") and
the exact bytes of every call site it redirects. If any check fails, it
changes nothing. No check runs per frame.

## Credits

- Ultimate ASI Loader by ThirteenAG (MIT).
- The NVTF, xNVSE and JIP LN NVSE sources were used as references for the
  engine's layout.

## License

[MIT](LICENSE).

For more game fixes, visit https://slop-blog.enkhayzomachines.net/fixes :)
