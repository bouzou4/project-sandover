---
title: Flat screen and VR graphics settings as two profiles that follow the OpenXR session
date: 2026-10-27
issue: ""
status: verified
sha: feat/xr-settings-profile
---
## Answer (≤5 lines)
The game keeps two complete sets of graphics settings: the flat screen one (`sandover.gc`) and
the VR one (`sandover-vr.gc`). While an OpenXR session runs, the VR set is the one the menu edits
and the engine runs; when the session ends the flat set comes back unchanged. The stock MSAA row
is part of the set, because MSAA is the antialiasing in stereo. In XR the render scale sizes each
eye's frame, 67% by default. A flat-screen boot with a VR edit made from the REPL left `sandover.gc`
byte-identical and `pc-settings.gc` at the flat MSAA.

## Evidence
### How it works
- One active set at `*pc-settings* sandover`, the other stored beside it in `sandover-other`
  (`pc/pckernel-impl.gc`). Every menu row, the REPL and the rig keep writing the same place, so
  none of them needed to learn about profiles. `*sandover-profile*` says which set is active.
- `sandover-settings-update` (once per frame) asks `pc-get-xr-state 0`, which the render thread
  sets from `xr::state() == RUNNING` right after `xr::update` (`pipelines/opengl.cpp`). When the
  wanted profile differs, `sandover-profile-switch!` swaps the two sets (three `mem-copy!`s) and
  the stock `gfx-msaa` with the stored one. Nothing else happens: the same frame's diff against
  `*sandover-applied*` sends exactly the fields that differ between the two sets, through the same
  `pc-set-*` forms a menu change uses. A switch is a menu change of several rows at once.
- A row open for editing holds the switch until it closes (`*sandover-menu-editing-func*`, the
  progress process's `selected-option`). The row's cancel restores a backup taken from the profile
  it was opened in; swapping under it would write that backup into the other profile.
- Saving: `commit-to-file` writes both files. `pc-settings.gc` always gets the flat MSAA: while
  the VR set is active the method puts the flat value in `gfx-msaa` for the parent's write and
  restores the VR one after. The VR file adds `(msaa N)`; the flat file has no MSAA key, since
  `pc-settings.gc` already holds it and stays readable by an unmodified OpenGOAL.
- Loading: the flat file first (unchanged path, including the first-boot MSAA rule), then the VR
  file; a missing one is written with the VR defaults, an unreadable one falls back to them. An
  install from before the split therefore gets VR defaults, not a copy of its flat values. A VR
  file without `msaa` takes the flat screen's (that is also what the rig's seed relies on).

### Render scale in XR
Before this change the row did nothing in stereo: `sandover-effective!` forced it to 100% whenever
temporal AA was not running, and `pipelines/opengl.cpp` applied `render_scale` only on the temporal
path. Now, while a session runs, the frame size the eyes are drawn at (`xr_targets->render_w/h`) is
multiplied by the scale before the renderer is called (one helper, `set_xr_eye_size`, used by the
frame and by a screenshot's restore, so an XR screenshot is drawn at the session's scaled size and
no targets are re-created for it), and `display_w/h` carry the unscaled size so
the bloom radius scales the way it does on the flat screen. The existing eye copy
(`copy_eye_to_xr_cropped`) already computes its crop from fractions of the drawn frame and blits
with `GL_LINEAR` when sizes differ, so the smaller frame is cropped and scaled up into the
runtime's swapchain image without touching the stereo dispatch or the OpenXR code. The swapchain
stays at the runtime's recommended size. Debug side-by-side stereo is not a session and keeps the
old rule (100%).

Default 67%: on the PSVR2 at SteamVR's default resolution each eye is drawn at 4491x3468 (one
symmetric frame covering both eyes' fields of view, `findings/2026-10-27-vr-bringup.md`), 15.6 Mpx.
At 77% that is 3458x2670 (9.2 Mpx), above a 4K frame's 8.3 Mpx; at 67% it is 3009x2324 (7.0 Mpx),
0.45 of the pixels. The rows' text shows that size per eye once the headset has drawn a frame
(`67% (3009X2324 PER EYE)`), from `pc-get-xr-state 1/2`. If the eyes later get their own asymmetric
frusta (3400 columns instead of 4491), the same 67% gives 2278x2324 and the text follows.

### No hidden coupling
Every row runs through the same `sandover-effective!` rules in both profiles. The only differences
come from the engine state, and they are the same ones stereo already had: no temporal AA per eye,
so the upscaler row is greyed (its value reads NOT AVAILABLE IN VR rather than a choice that would
not run) and contact shadows are greyed. Tiers are unchanged and stay monotonic.

### Tests (`sandover/tools/tests/test_settings_forms.py`, `test_settings.py`, about 16 new)
The GOAL evaluator now also runs `sandover-settings-load`, `sandover-profile-switch!` and the
`commit-to-file` method with the file reader and writer replaced by an in-memory folder:
first boot writes both files; an older install gets VR defaults and its flat MSAA; an unreadable
VR file falls back; a session start pushes the VR render scale on that frame and nothing on the next;
a VR edit (render scale, a switch, MSAA) never reaches `sandover.gc` or `pc-settings.gc` and a flat
edit never reaches `sandover-vr.gc`; an open row holds the switch; the REPL force; equal values give
equal effective settings in either profile; vanilla rendering in VR keeps the eyes at 100%; debug
stereo keeps 100%. The Python mirror reads and writes the VR file, `seed-off` writes both files.

### Launch (flat screen, no headset)
Rig slot 0, village1-hut, this branch's gk and GOAL, both files seeded all-off. Over the REPL:
`*sandover-profile-force*` to `'vr`, then vanilla rendering off, render scale 50%, MSAA 8, commit;
then back to `'flat`, commit. Log:

```
RIG:PROOF boot profile flat flat-msaa 2 vr-msaa 2
[PC] Sandover settings profile: vr
RIG:PROOF vr-edit profile vr vanilla #f rs 4 bloom #f msaa 8
[PC] Sandover settings profile: flat
RIG:PROOF back profile flat vanilla #t rs 0 bloom #f msaa 2
RIG:PROOF stored-vr rs 4 bloom #f msaa 8
```

After the run `sandover.gc` is byte-identical to the seeded file (`diff` empty), `pc-settings.gc`
holds `(msaa 2)`, and `sandover-vr.gc` differs from its seed in exactly `vanilla-rendering? #f`,
`render-scale 50` and `msaa 8`. No headset session was run: the switch on a real session start is
covered by the tests (`pc-get-xr-state` mocked) and by the code path above.

### Known behaviour
- The first frames of a session are drawn at the flat profile's scale: GOAL sees the session one
  frame after the render thread (`pc-get-xr-state` is set after `xr::update`), and the switch pushes
  the VR set on its next update. A few eye frames at the wrong size, then steady.
- SteamVR may end and restart the session (`findings/2026-10-27-vr-bringup.md`); each bounce swaps
  to flat and back. The swap only re-sends differing rows, so this costs a frame or two each time.
- The per-eye text and the VR upscaler list follow the session (`sandover-xr-session?`), not the
  profile: a REPL force to `'vr` on the flat screen edits the VR set with the flat-screen texts.
  The eye size is cleared when the session ends, so a later session never shows a stale size.
- Bug reports record the active profile (`settings.sandover.profile`) and the MSAA in effect
  (`settings.sandover.values.msaa`, the VR one in a session; `settings.pc` keeps the flat one).
- The rig's `--play-look` holds the VR profile for XR shots (`howto/vr-synthetic.md`).

## Implications for the design
- New graphics rows land in both profiles for free (one struct). Only a row whose VR default must
  differ needs a line in `sandover-settings-reset-vr!`.
- The stock rows other than MSAA (resolution, display mode, vsync, frame rate, the PS2 options
  page with its LOD distances) are not split: the first four describe the window, which the
  headset does not use; the LOD rows apply to both.

## Follow-ups
- A headset session to confirm the switch and the per-eye text on the PSVR2 (one look, no tuning).
- Temporal AA per eye would make the upscaler row live in VR; the row is ready (same field).
