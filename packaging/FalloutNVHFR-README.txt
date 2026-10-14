FalloutNVHFR 1.0.1
High-frame-rate patch for Fallout: New Vegas
==================================================

What it does
------------
Fallout: New Vegas ties its game logic to the frame rate. Above 60 frames a
second the unmodified game runs faster than real time (about 40 % fast at
105 frames a second), limits itself to roughly 100 to 140 frames a second,
and moves everything in uneven steps. This patch keeps the game simulating
at a fixed 60 ticks a second, at the correct speed, and draws the picture at
whatever frame rate your system reaches, with the motion between ticks
interpolated. The result is smooth motion at 120, 240 or more frames a
second without running the game logic faster.

Interpolated at the display frame rate:
  - player movement and the first- and third-person cameras, including
    crouching, falling, aiming (field of view) and reload view animations
  - the first-person arms and weapon, including aiming down the sights and
    crouching or standing up
  - the player's body in third person
  - other characters and creatures, including ragdolls
  - objects that move: carried, dropped and knocked-over items, doors and
    other animated objects, in the loaded area around you
  - the sky
  - the VATS and kill cameras, including their slow motion
Mouse look runs at the display frame rate.

It also:
  - removes the sleeps the game's timers take when a frame is shorter than
    10 ms, which is what limits the unmodified game's frame rate
  - reads key and mouse-button presses at the 60 Hz tick, so a press between
    ticks is not lost
  - removes the acceleration of the mouse cursor in menus (can be switched
    off)


Requirements
------------
  - Fallout: New Vegas version 1.4.0.525. Developed and tested on the GOG
    release only. Other releases are accepted if their executable has the
    same code layout; none has been tried. See "If it does not load" below.
  - A 32-bit ASI loader, for example Ultimate ASI Loader. The download
    comes in two forms: the plugin alone, for those who already have a
    loader, and a bundle that includes Ultimate ASI Loader as dinput8.dll.
  - A frame rate limit set outside the game (graphics driver or a limiter
    tool such as RTSS). Tested with a limit of 240.
  - Tested with the game's own settings as the launcher writes them
    (iFPSClamp=0, V-Sync off). The script extender (NVSE) is not needed; the
    patch has not been tried together with it.


Install
-------
Copy into the folder that contains FalloutNV.exe:

    FalloutNVHFR.asi
    FalloutNVHFR.ini
    dinput8.dll                  (the ASI loader; bundle only)

Plugin-only download: you need a 32-bit ASI loader already installed in the
game folder.

Bundle: if you already use an ASI loader, do not copy dinput8.dll; keep
yours. If you already have a different dinput8.dll from another mod, do not
overwrite it: rename the included loader to another name it supports and the
game loads, for example d3d9.dll or winmm.dll.

FalloutNVHFR.asi may instead be put in the loader's "plugins", "scripts" or
"update" folder. FalloutNVHFR.ini must be in the same folder as the .asi.


Uninstall
---------
Delete FalloutNVHFR.asi, FalloutNVHFR.ini and FalloutNVHFR.log. Delete
dinput8.dll too if nothing else needs the ASI loader. The patch changes
nothing on disk besides its own log file; saves and the game's own settings
files are not affected.

To switch the patch off without removing it, set bEnabled=0 in
FalloutNVHFR.ini.


Settings (FalloutNVHFR.ini)
---------------------------
  bEnabled                  1 = patch active, 0 = game unmodified.
  bMenuRealTime             1 = menus, conversations, loading and the 30
                            frames after them run on the real frame time.
                            Set to 0 if you get a crash or a physics glitch
                            right after closing a menu; those moments then
                            look stepped instead.
  bMenuMouseNoAcceleration  1 = plain, unaccelerated mouse cursor in menus.
                            0 = the game's own cursor behaviour.
  bTelemetry                1 = write diagnostics to FalloutNVHFR.log
                            (several megabytes an hour). Leave at 0 unless
                            you are reporting a problem.
  fSimulationHz             Leave at 60. Values above 60 are not accepted.

Leave the settings marked "Internals" as they are.


Known limitations
-----------------
  - In normal play the game is simulated at 60 ticks a second. In menus and
    conversations, while loading, and for 30 frames after they end, it is
    stepped at your frame rate (see bMenuRealTime).
  - The picture runs up to one tick (1/60 s) behind the simulation. That is
    what makes interpolation possible. Mouse look is not delayed.
  - For about a quarter of a second after a menu or conversation closes,
    motion is not yet interpolated.
  - Below 60 frames a second the game runs slower than real time.
  - An object in a quiet neighbouring area starts being interpolated a
    fraction of a second after it first moves.
  - Parts of some weapon animations can look uneven. Noticed on the 9mm
    pistol: for about a third of a second at the start of aiming down the
    sights the weapon shakes. The cause was not found.
  - The "spider hands" bug (fingers stretching at high frame rates) is not
    corrected by this patch.
  - New Vegas Tick Fix (NVTF) and New Vegas Stutter Remover change the same
    timer. If either is found in Data\NVSE\Plugins, this patch does not
    install. If another mod changes the same code after the patch has
    installed, the patch switches itself off on the first frame.


If it does not load
-------------------
Start the game once and open FalloutNVHFR.log next to the .asi.

  - No log file: the .asi was not loaded. Check that the ASI loader is in the
    game folder and that the .asi is next to FalloutNV.exe or in "plugins",
    "scripts" or "update".
  - "hooks disabled: executable layout not recognised", with a line starting
    "layout:" above it: your FalloutNV.exe does not have the code layout the
    patch was made for (a different release, an executable that is still
    packed when the patch loads, or code already changed by another mod).
    Nothing was changed; the game runs unmodified.
  - "hooks disabled: ... is installed and patches the same timer": remove
    the named mod or this patch.
  - "hooks rolled back": something prevented the patch from installing
    completely. Nothing was left changed.
  - "first frame: hook sites were patched by something else": another mod
    changed the same code after this patch. The patch is inactive.
  - "installed: frame hook=1 timer hook=1 ..." followed by "first frame: hook
    sites intact": the patch is installed and running.


Changes
-------
1.0.1
  - First person: the arms and weapon no longer step and trail the view
    while aiming down the sights or crouching and standing up. The arms
    are now drawn from the interpolated camera, and the whole arm and
    weapon skeleton is interpolated.
  - VATS and kill cameras are interpolated.
1.0.0
  - First release.


Credits
-------
Ultimate ASI Loader by ThirteenAG, MIT licence,
https://github.com/ThirteenAG/Ultimate-ASI-Loader
The bundle includes it unmodified, with its licence in
Ultimate-ASI-Loader-LICENSE.txt.


Misc
-------
For more game fixes, visit https://slop-blog.enkhayzomachines.net/fixes :)
