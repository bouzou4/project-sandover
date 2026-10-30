---
title: Why AO (and the tile lights) die mid-session until the vanilla switch is toggled
date: 2026-10-22
issue: none (play reports 2026-10-21, two machines)
status: verified
sha: 594e76f8a (proof build: play base ec4a22c8f + the fix)
---
## Answer (≤5 lines)
Not a resize. The play launcher sent `(pc-set-ssao-preset 4)` after every warp and on `a`. Since the
GTAO tiers, the preset table has four entries (0-3), and `pc_set_ssao_preset` turned AO off for
any index it could not apply. GOAL's `*sandover-applied*` still said AO was on, so the diff push never
re-sent it; the tile lights (which need SSAO) went with it. The vanilla switch forces a full push, so it
cured it. Fix: an unknown index logs an error and leaves AO alone; only a negative index turns it off.

## Symptom
Play, 2026-10-21 (sessions launched with `local/play.py`; reported on two machines): AO and sometimes other effects disappear mid-session
and come back only after Options > Sandover "Vanilla rendering" off and on.

## Mechanism
- Session log of 2026-10-21 20:03: every `PLAY:APPLIED` is followed by
  `glq: settings changed at byte 106` (byte 106 of `GfxGlobalSettings` is `ssao_enable`; byte 112,
  `ssao_view`, is what the harness's AO view changes) and `SSAO: emissive mask target ..., 1 samples`
  without "highlight material" (`light_material` needs the tile lights, which need SSAO). AO comes back
  only after a menu change (normal target re-allocated).
- The launcher's FORMS then held `(pc-set-ssao-preset 4)` (the `deep` row of `play.md`); master's table
  has seven presets, where 4 is `deep`. The GTAO tiers cut it to `deep-low, deep, deep-high, deep-ultra`.
- `kmachine.cpp` `pc_set_ssao_preset`: `if (!SsaoApplyPreset(index)) ssao_enable = false;`.
- `sandover-settings-update` pushes only differences from `*sandover-applied*`, which a direct form
  does not touch: nothing re-enables AO until a setting it compares changes (the vanilla switch, or the
  AO row).
- The launcher was edited locally on 2026-10-21 21:09 (after that session) to drop the AO form and add
  `(sandover-settings-reapply)`, which re-sends the menu after the forms; that covers the launcher. The
  engine fix covers any other stale caller (old snapshot toggles, notes, manifests).

## Proof
`sandover/tools/scripts/effects_after_resize.py` (one rig gk, play look with ultra AO, events: fullscreen,
windowed, x2, render scale 59 % and back, HDR output off and back, FP16 off and on, window resize and
back, the launcher's current apply, the old launcher's AO form, fullscreen; then vanilla off/on):
- play bin 20261019-161748-08ce3291ec (ec4a22c8f): FAIL `old-launcher-ao fullscreen`. AO-view share above
  0.9 drops from 0.853 to 0.177 (the lit frame shows: AO off), still 0.182 after the next fullscreen
  event, back to 0.848 after the vanilla cycle. Frame difference 11.1 against 3-4 for every other event.
- proof build 594e76f8a (ec4a22c8f + the fix): PASS, every probe 0.847-0.900; the log has
  `pc-set-ssao-preset: no preset 4 (4 presets), AO left as it was` once.
- Every other event passes on both builds: resize, fullscreen, render scale, HDR output, FP16, play apply
  and the render-target rebuild of each bug report (the F9 path) leave AO intact.

## Ruled out on the way
- GTAO history NaN after a target rebuild: no event leaves the AO view damaged (white share and
  occluded share within 0.01 of the baseline at each size).
- `glq::SavedState`: with `SANDOVER_GL_STATE_CHECK=1`, after each render-target rebuild bloom and
  emissive-lights log `read-fbo 96->71` once (the snapshot taken in the rebuild frame holds a read FBO
  bound only in that frame). Without the check the stale snapshot is restored for up to 255 frames.
  Nothing reads through it in the passes measured; transient, not this bug. The check mode refreshes
  the snapshot every frame, so it cannot reproduce a stale-snapshot bug: run without it to test one.
- `ssao_normal_target` sticky off (SsaoRenderer.cpp): never logged in the harness or the play session.

## Follow-ups
- The 2026-10-21 play log also has `glDrawBuffers failed ... 0x8ce0` on tfrag/tie/merc at boot (draw
  buffers set while the window framebuffer is bound) and "finish() not called on ssao" (fixed with the GTAO change).
- `snapshot-run.md` sidecar example and `play.md` no longer name a preset index.
