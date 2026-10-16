Sandover: a lighting remaster of Jak and Daxter: The Precursor Legacy on OpenGOAL
==================================================================================

You need your own copy of the game as a disc image (.iso) made from your own disc.
This zip includes five sun and moon sprite images taken from Jak 3; nothing else from any disc
is included, and your own Jak 1 disc image is still required.

Install
-------
The OpenGOAL launcher is the only supported way to install Sandover:
1. In the launcher, open Settings > Mods, paste the mod source URL from the project's GitHub
   page into Mod Source URL and press Add.
2. Open Jak and Daxter, then Features > Mods. Pick Sandover (beta) under Available Mods and
   install it. The launcher downloads this zip, builds the game from your disc data (a few
   minutes) and gives the mod its own Play button.
Updates arrive through the same mod source.

The remaster look is on by default. Pause > Options > Sandover turns each part on or off.

Requirements
------------
- Windows, 64-bit. Tested on Windows 11; Windows 10 should work but is untested. A CPU with AVX
  and an OpenGL 4.3 graphics card with current drivers.
- HDR output (optional): turn on "Use HDR" for the game's display in Windows Settings > System >
  Display before starting the game. Without it the game runs in SDR.

Settings and saves
------------------
In the launcher's install folder, under features/jak1/mods/Sandover/_settings/sandover,
separate from the stock game.

Reporting a bug
---------------
1. Where you see the problem, press F7 (or Pause > Options > Sandover > SAVE BUG REPORT (F7)).
   "BUG REPORT SAVED" shows at the top left.
2. The report goes to the bug-reports folder inside the settings folder above:
   features/jak1/mods/Sandover/_settings/sandover/bug-reports. Each report is a PNG of the
   frame, a JSON sidecar and a zip of both.
3. Open a new issue on the project's GitHub page with the "Sandover bug report" form, say what
   you saw and what you expected, and attach the zip.
The report holds the frame, where you were (level, camera, time of day), your Sandover settings,
display and HDR state, the build, Windows version, graphics card and driver, and the end of the
game log. File paths and your user name are taken out before it is written.

Licenses
--------
Source code: ISC License (LICENSE). OpenAL Soft (LGPL) ships as OpenAL32.dll with its notices;
its source is in third-party/openal-soft in the project's repository at the release tag.
