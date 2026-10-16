---
title: Temple rings seen from village1 read bright and see-through; the stand-in's merc request lacked the actor's envmap, alpha and light state
date: 2026-10-15
issue: user report 2026-10-15 ("weirdly bright and transparent", seen from Sandover Village)
status: verified
sha: b59bd4c22 (base); fix on branch fix/temple-rings
---
## Answer (≤5 lines)
The village1 stand-in (`vista-static-merc`) sent envmap fade `#x80808080`, ignore-alpha 0 and world-frame lights for an
actor whose one effect is envmapped: its base pass alpha-tested the envmap mask (holes) and added the envmap white at
full strength (bright, worst at dusk). It now sends what `draw-bones` sends: ignore-alpha 1, fade env-color x tint
`#x80334c80` / 128 with alpha 0, camera-frame lights, one effect, bucket `merc-tfrag-tex0`. Proof: 70 m dusk, actor
vs fixed stand-in in one scene and clip frame, ring luma 49.9 vs 49.1 (old request 118.9); hut-temple dusk 154.3 to 94.1.

## Evidence (path:line@sha / URL, short excerpts)
- `goal_src/jak1/engine/gfx/foreground/bones.gc:1288-1291@b59bd4c22`: per-effect fade zeroed before the envmap test.
- `bones.gc:1299-1328@b59bd4c22`: envmap branch, `(* 0.0078125 strength env-color tint)`, byte 3 = 0; `*emerc-hack*`
  sets `ignore-alpha 1` (1325-1327).
- `bones.gc:965-966@b59bd4c22`: ignore-alpha OR'd into the mask; `:990` effect-count from the merc-ctrl;
  `:1005-1008` fades written per effect.
- `bones.gc:1144-1165@b59bd4c22`: lights multiplied by `inv-camera-rot` rows before the request copies them.
- `goal_src/jak1/pc/features/vista-landmarks.gc:357-368, 386-412, 437` (branch): the same values in the stand-in.
- `goal_src/jak1/engine/draw/drawable.gc:510@b59bd4c22`: the actor takes `moods <lev> light-group <light-index>`;
  `goal_src/jak1/engine/gfx/mood/mood-tables.gc:3270-3278@b59bd4c22` builds `*jungle-mood*` from village1's lights,
  sun and palette tables, so the viewer's light group 0 equals the actor's at every hour.
- `game/graphics/opengl_renderer/shaders/merc2.frag:416@b59bd4c22`: `ignore_alpha == 0 && color.w < 0.128` discards;
  `shaders/emerc.vert:184@b59bd4c22`: `vtx_color = fade.xyz`.
- Probe of `towertop-1` on the temple (rig stage hook): lod0/1/2 one effect, `envmap-usage 1`, tint `#x80334c80`,
  fade0 0, fade1 3.5; light-index 0, color-mult 1, texture page 0; `force-envmap? #t`, `*emerc-hack* #t`;
  env-color noon (128 128 128), dusk (128 98.4 32.1).
- Captures: `sandover/shots/temple-rings.yaml`, 3840x2160, MSAA 2 + SMAA, `--play-look --wait-vista`, in
  `$SANDOVER_DATA/shots/temple-rings/{before,after,off,off2,swap-real,swap-standin}`. Ring pixels differ by more than
  12 levels from a rings-off frame of the same camera (stand-in off: vista option 11 = 0; actor: draw-status hidden).

Stand-in from village1, before -> after (same pose; white = all channels over 170 within 40 of each other):

| view | mean RGB before | mean RGB after | luma | white px | ring px |
|---|---|---|---|---|---|
| hut-temple dusk (645 m) | 192 145 135 | 123 86 87 | 154.3 -> 94.1 | 12.8% -> 0 | 16355 -> 16587 |
| hut-temple noon | 153 149 159 | 135 117 121 | 150.6 -> 120.8 | 20.1% -> 2.6% | 15481 -> 15655 |
| 70 m dusk | 141 114 104 | 98 57 40 | 118.9 -> 64.7 | 9.6% -> 0 | |
| 70 m noon | 130 116 108 | 117 82 66 | 118.3 -> 88.4 | 5.8% -> 0.6% | |

Actor and fixed stand-in in one jungle-start scene at the actor's clip frame (0.9667; actor hidden, stand-in drawn),
same pixels (both over 40 off the hidden frame, eroded 3 px):

| view | actor mean RGB / luma | stand-in mean RGB / luma |
|---|---|---|
| 70 m dusk | 81.1 42.6 30.6 / 49.9 | 79.1 42.1 31.1 / 49.1 |
| 130 m dusk | 86.3 47.4 36.8 / 54.9 | 84.8 47.1 36.8 / 54.4 |
| 130 m noon | 128.3 81.2 60.4 / 89.7 | 128.0 79.6 58.7 / 88.3 |
| 70 m noon | 128.6 79.5 54.4 / 88.0 | 119.0 70.7 49.3 / 79.4 |

70 m noon: the stand-in sits about 130 px (4K) right of the actor with a different inner-ring yaw, so the shared
pixels cover different faces; colour and finish of matching faces agree in the crops.

The `near130-standin-*` village1 shots do not show the stand-in: before and after are equal (luma 91.1 / 91.1 noon,
61.5 / 61.0 dusk) and the before frame has no holes or streaks, though the same old request shows both at 645 m and
70 m. Most likely jungle is GOAL-resident at that camera, so `vista-static-want?` is false and the rings there are the
game's art; they stay in the manifest, annotated, and are no regression shot.

## Implications for the design
- A hand-built `pc-merc` request must carry the per-effect state `draw-bones` computes (envmap fade, ignore-alpha,
  camera-frame lights); an envmapped model is where a neutral default reads wrong.
- Tint, fades and spin are towertop-only tables applied to the one merc landmark; a second merc landmark needs
  per-landmark values (no mechanism yet).
- The bucket follows the actor's texture page (`merc-tfrag-tex0`: SSAO receiver, tile-light prop), not the
  translucent `merc-alpha-tex`.

## Follow-ups
- The actor's highlights read brighter than the stand-in's in every matched pair while the means agree (95th
  percentile luma 214.5 vs 187.8 at 130 m noon, 114.4 vs 100.7 at 130 m dusk, 231.0 vs 182.4 at 70 m noon); likely
  lod (actor lod2 past 65 m, stand-in lod0) or pose; not visible from the village.
- Stand-in placement and spin against the clip (offset visible at 70 m) left as is.
