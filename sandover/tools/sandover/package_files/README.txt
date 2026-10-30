Sandover: a lighting remaster of Jak and Daxter: The Precursor Legacy on OpenGOAL
==================================================================================

You need your own copy of the game as a disc image (.iso) made from your own disc.

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
- Windows, 64-bit. Development and testing used AMD Radeon and NVIDIA GPUs on Windows 11;
  Windows 10 should work but is untested. A CPU with AVX and an OpenGL 4.3 graphics card with current
  drivers. Every effect runs on any vendor; only the optional DLSS and FSR 4 modes are vendor-specific.
- HDR output (optional): turn on "Use HDR" for the game's display in Windows Settings > System >
  Display before starting the game. Without it the game runs in SDR.
- DLSS (optional, NVIDIA graphics cards only): nvngx_dlss.dll is not included. A copy placed in
  the mod folder next to gk.exe is used first (usually taken from the DLSS SDK on NVIDIA's GitHub
  page, github.com/NVIDIA/DLSS, or from an installed game that uses DLSS); otherwise a copy the
  NVIDIA App has stored on the system is used when there is one. Without either the option is not
  offered.
- FSR 4 (optional): AMD's FSR 4 runtime is included (amd_fidelityfx_*_dx12.dll, licence in
  AMD-FSR-LICENSE.txt). It needs a graphics card and driver that FSR 4 supports (RDNA 4, or
  RDNA 3 with a recent driver) and DirectX 12; elsewhere the option is not offered.
- VR (optional): an OpenXR runtime (SteamVR) and a headset; start gk.exe in the mod folder with --xr plus
  the launcher's boot arguments (the launcher's Play button starts the flat-screen game). Tested on a
  PSVR2.
- Audio: Pause > Options > Sandover > Audio sets the output (Auto, Stereo, 5.1, 7.1, Headphones,
  Spatial for Dolby Atmos, Windows Sonic or DTS:X) and has a speaker test.

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
