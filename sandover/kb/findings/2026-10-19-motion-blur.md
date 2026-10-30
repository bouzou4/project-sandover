---
title: Per-object motion blur from the motion-vector buffer
date: 2026-10-19
status: done (live contact sheet and parity in)
sha: branch feat/motion-blur (base master 7cc714ec9)
---
## Answer (≤5 lines)
Per-object motion blur (tile max, neighbour max, McGuire/Jimenez reconstruction) on the scene
colour before the HUD, rows Off / Weak / Medium / Strong and Camera motion blur Off / Vehicles /
Always. Actor pixels blur by v_full - v_cam clamped to |v_full|, the world by nothing unless
camera blur is active. Live sheet: blur reads at every strength on a spin, a still enemy stays as
sharp as the world under a pan, Jak and the zoomer stay sharp under the follow camera, HUD sharp.

## Rules and state
- Rules as of 2026-10-21 (code and tests; live verification below):
  - Rule: an object never blurs more than it moves on screen, and the world's camera motion never
    counts. Object pixels (`src == 1`, the merc override) take v_rel = v_full - v_cam clamped to
    |v_full|: `v_rel * min(1, |v_full| / max(|v_rel|, eps))`; the world is zero
    (`motion_vectors.frag`, `rel_on` 1). Jak tracked by the follow camera: v_full ~ 0, no streak
    (the old unclamped v_rel smeared him by his world speed). A still enemy during a pan: v_rel 0,
    sharp like the world (plain v_full would have blurred it). Spin attack, running enemy: both
    large, full blur. An enemy running while the camera pans with Jak: its own motion only.
  - Camera motion blur row Off / Vehicles / Always (default Vehicles, text ids 0x5a66-0x5a68):
    object pixels take v_full and the rest the camera-only velocity (`rel_on` 2), so everything
    is consistent under camera blur. Vehicles keys off the game's
    own mounted test, `(collide-action racer flut)` on `*target*`'s root prim (set in
    target-racing-start / target-flut-start, cleared in their exits; collectables.gc and
    logic-target.gc ask the same), as `sandover-riding-vehicle?` in pckernel.gc, sent as
    `pc-set-motion-blur 11` on change and latched with the chain. Mode is option 10. Shares the
    strength row, greyed while it is off.
- Numbers: shutter (fraction of the frame interval) weak 0.25, medium 0.5, strong 1.0; taps 16 / 16 / 24; radius cap 80 px at 2160 lines, tile = cap.
- Verified live: object-only velocity (static world zero under a pan, debug view), HUD sprites and merc HUD unblurred, cut frame skipped (log). Toggle-off parity: bit-identical to master except animated merc actors (rig noise, same as master vs master).
- Live contact sheet (2026-10-22, tip b70d68e4e, 3840x2160, MSAA 2, one instance):
  `$SANDOVER_DATA/shots/motion-blur-428-sheet/motion-blur-428-contact-sheet.png`, rows spin / run /
  jump / enemy moving / pan Vehicles / pan Always / pan debug view / zoomer camera Off / zoomer
  Vehicles, columns Off / Weak / Medium / Strong (one burst, frame N renders level N % 4). Tile
  readback per burst (`pc-set-motion-blur 8`, the action at Strong, 80 px tiles, 1296 tiles):
  | Case | moving tiles median (max) | longest px median (max) |
  |---|---|---|
  | spin (probe) | 59 (71) | 23.4 (52.6) |
  | run, follow camera | 43 (59) | 4.7 (7.6) |
  | jump | 50 (77) | 7.1 (25.6) |
  | enemy moving (bonelurker at Jak) | 526 (757) | 41.4 (80.0) |
  | pan, held enemy in view, Vehicles (on foot) | 7 (18) | 1.8 (2.6) |
  | same pan, Always | 1296 (1296) | 80.0 (80.0) |
  | zoomer, camera Off | 45 (59) | 54.5 (61.2) |
  | zoomer, Vehicles | 1296 (1296) | 58.5 (73.1) |
- Proof lines:
  - Symptom: a still enemy smeared during a camera pan with plain v_full. Mechanism: its on-screen
    motion is all camera motion; v_rel = v_full - v_cam is 0 on it. Proof: pan with the enemies
    held (sleep bit), Vehicles on foot: 7 moving tiles, 1.8 px (Jak's idle only); debug view frame,
    0 heat pixels in the enemy's box and in the world, 1083 on Jak. Always on the same pan: 1296/1296
    tiles at the 80 px cap, enemy and world blur together.
  - Symptom: Jak's body streaked by his world speed under the follow camera with unclamped v_rel.
    Mechanism: the follow camera tracks him, so v_full ~ 0 and the clamp to |v_full| zeroes v_rel.
    Proof: run burst 4.7 px median (limbs), Strong frame of Jak's body sharp; zoomer under camera
    Off 54.5 px median comes from the spinning fans, the bike body sharp.
  - HUD: sprites drawn after the blur, merc HUD models flagged (w negative): heart, orbs, the
    zoomer speedometer and subtitles sharp in every Strong frame.
- Toggle-off parity re-check (rig parity manifest, 7 continues at noon, 4K MSAA 2, master
  7cc714ec9 built and staged as its own root vs this branch, motion blur off = boot state, pixdiff
  threshold 0): branch vs master bit-identical at village1-hut, beach-start, misty-start; the other
  four differ by 0.02-0.35 % of pixels, inside the master vs master floor (0.18-0.78 %, animated
  actors). A first branch run, made while the machine was saturated, came out with every frame
  framed differently (same camera pose logged); its repeat matches master as above, so it is
  recorded as a rig flake, not reproduced.
- The sheet above ran on the vanilla (MSAA) path: the staged root had no FidelityFX headers, so FSR 3.1
  fell back. FSR 3.1 probe after the merge with master 0100caa20 (reactive mask built after the blur
  and the deferred HUD sprites): 67 % render scale (2560x1440 to 4K), Strong, pan with camera blur
  Always, `$SANDOVER_DATA/shots/motion-blur-428-sheet/fsr67-probe4/`. Heart HUD interior, Off vs
  Strong frame: mean |d| 1.35 / 1.30 (two pairs), gradient energy ratio 1.006 / 1.019 (sharp, no
  ghost); a world box on the same frames: mean |d| 11.5 / 10.9, gradient ratio 0.45 / 0.48.
- Remaining: cost, in the combined perf pass after merge.
- Unverified: frame-rate scaling (formula only); Strong tap-count cleanliness at 120 fps.
