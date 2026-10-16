---
title: How two loaded levels share mood, fog, sky and the frame
date: 2026-09-23
issue: #19
status: verified
sha: 64d72d103
---
## Answer (≤5 lines)
Load-boundary polygons request levels into two slots (level0/level1). Both levels' mood
functions run every frame; each level's background uses its **own** palette weights, while
fog, sun colour, shadow and character lights are one global blend weighted by camera distance
to each level's bsphere, ramped over ≈5 s. Both levels draw into one depth buffer; all opaque
geometry of both levels is done by the end of bucket 30.

## Evidence (path:line@sha / URL, short excerpts)
- Slots: `goal_src/jak1/engine/level/level-h.gc:13,202-219@64d72d103`; slot allocation
  `level.gc:218-311`; per-frame load driver `level.gc:1083-1186`.
- Boundaries: `load-boundary.gc:1351-1380` dispatches `load`/`display`/`vis`/`force-vis`/`checkpt`.
  Examples in `load-boundary-data.gc`: `(load ogre village3)` :361, `(load village3 ogre)` :1162,
  `(load village3 snow)` :497, `(load maincave darkcave)` :1192.
- Vis: 8 `vis-info` slots per level (`level-h.gc:73-90`); the authoritative vis is chosen by
  which level's boxes contain the camera (`level.gc:1232-1312`, `point-in-boxes?` :738-757).
- Mood: `time-of-day.gc:213-228` runs `((-> s2-0 mood-func) (-> s2-0 mood) ...)` per slot;
  weight `(/ f0-6 (+ f0-6 f1-0))` of `level-distance` (`cam-update.gc:245`, distance to the
  bsphere surface) at `time-of-day.gc:230-252`; ramp ±0.0033/frame (±0.00396 PAL) at :253-280,
  snap on teleport; global lerp of fog/prt/sun/shadow/light-groups at :290-340. village2 mood
  forced near sunken when camera y > 0 (:242-249). Sky on if any active level has `:sky` (:198-202).
- Background palette weights per level: `goal_src/jak1/engine/gfx/tfrag/tfrag.gc:275-278`
  `(-> lev mood itimes 0 quad)`.
- Renderer: `game/graphics/opengl_renderer/buckets.h:6-78` (level0 opaque 5-11, level1 opaque
  12-18, shrubs 19-30, alpha from 31 `ALPHA_TEX_LEVEL0`, level1 alpha from 38); one clear before
  the bucket loop `OpenGLRenderer.cpp:1244-1263`; no depth clear inside `dispatch_buckets_jak1`
  :1291-1353; fog colour global :1316; per-bucket `TfragPcPortData` `TFragment.cpp:229-232`,
  `Tie3.cpp:317-320`.
- Dark Cave: `*palette-fade-controls*` is global and reset once per frame
  (`time-of-day.gc:438`); darkcave crystals write controls 1-7 (`levels/maincave/cavecrystal-light.gc:121`);
  maincave reads 1-2 (`mood.gc:1233-1234`), so crystals light maincave geometry across the
  border. `darkcave` run-packages include `"maincave"` (`level-info.gc:1688-1738`).

## Implications for the design
- SSAO: insert after bucket 30; the depth buffer then holds both levels' opaque geometry
  (verified). No per-level depth separation is needed or available.
- Unbaked sun: during an overlap two moods are live. Each level's background must be lit
  with its own level's sun/effect state (the palette path already does this); characters get
  the global blend. A single global sun uniform would be wrong at borders such as
  village3 (fixed crater light) ↔ snow (hour-driven), or ogre (3 contexts) ↔ village3.
- Fog colour is global but fog distances are per bucket; a new lighting pass should take fog
  per level to match.
- Crystal lights must be registered per crystal, not per level, so maincave stays
  consistent when darkcave crystals fire. The one-frame reset/read latency is harmless.
- SSAO in Dark Cave: slot 0 is near-black; apply AO only to the crystal/point-light
  contribution, never to the base.

## Follow-ups
- Runtime capture of `*time-of-day-context* current-interp` across village3↔snow and
  ogre↔village3 to see the ramp in practice (needs the rig).
- Check whether the mood-func of a loaded-but-inactive level writes shared globals
  (e.g. `set-target-light-index`, `*teleport*` set by darkcave at `mood.gc:1285`).
