---
title: Far particles lit a nearer occluder because a tile's light was clamped to the occluder
date: 2026-10-22
issue: play capture runs/20261022-1234-play/0005 (misty, 23.0 h)
status: verified
sha: cbcdc5373 (base), fix/particle-light-depth
---
## Answer (≤5 lines)
The light was in the wrong place, not too wide and not unoccluded. Two blue eco clusters (eco-39 at 67 m,
eco-40 at 100 m) share their 60 px screen tiles with the underside of the bone bridge, 30 m nearer.
`emissive_tiles.frag` caps a tile's light depth at the tile's nearest surface + 1 m. That cap ignores the
sprite depth the eco already wrote, so the eco's tile lights landed on the bone and lit its underside blue.
Fix: a tile with an emissive sprite never pulls its light in front of its nearest sprite. All 17 candidates are now within 2.4 m of an emitter. The underside drops to near the no-light floor.

## Symptom
Play snapshot 0005 (misty at night, crop `crops/0005-a1.png`): the underside of the spine bridge and the
pillar top over two blue eco sprites read lit blue, although the sprites float far behind them.

## Mechanism
- Ruled out by the burst log (`pc-set-emissive-lights 17`, rig `from-snapshot` with a 4-frame
  burst):
  - **Radius:** the eco world rows (1310-1312) sit at their census positions with the 5 m eco radius.
  - **Group origin:** the eco's own tiles (luma 0.82 and 0.46) are 0.6 m from eco-39 and are
    taken over by its row.
- The misplaced lights are three more blue tiles, drawn at (-12.2, 24.0, 856.1), (-11.6, 24.0, 856.3)
  and (-14.4, 24.4, 853.1). The background mesh z-buffered at the rig camera puts the lit underside
  pixels (`mis-bone1`) at (-11.3, 24.4, 856.2) and (-13.9, 24.6, 853.1): the lights sit on the bone,
  28.9-30.5 m from eco-39. The bone is 36 m from the camera; eco-39 is 67 m.
- The line responsible: `float z = min(a.zw > 0.0 ? a.zsum / a.zw : a.zmin, a.zmin + near_slack);`.
  The E-weighted depth is the sprite's own (sprite depth), but `zmin` is the nearest surface
  anywhere in the tile. That is the bone edge, so the clamp wins.
- The launch-light takeover does not catch it. `toward_emitter` drops a tile only between 10%
  nearer than the emitter and its radius behind it. A tile 30 m in front is outside that window, so it
  stays a light of its own.
- The same clamp moved the misty torch tiles 10-24 m toward the camera onto nearer rock (master: 8
  of 16 candidates more than 10 m from any census row).

## Fix
`emissive_tiles.frag`: track the tile's nearest emissive sprite depth (`szmin`, carried in the free
`part2.w` of the split path). The clamp's anchor becomes `max(zmin, szmin)` when a sprite is
present. Tiles without sprites, and tiles whose nearest surface is the sprite, are unchanged. Emission
with no depth of its own (a lamp texture) still sits on its surface; a tile mixing such a surface and a
farther sprite now anchors at the sprite (not seen in the frames checked).

## Proof
Captures: `$SANDOVER_DATA/shots/particle-light-depth/{A,A2,B,OFF}`. A and A2 are master, B is the
fix (same binary, shader from the branch), OFF is master with tile lights off. Side-by-side crop:
`crop-A-B.png`. Overhang = `mis-bone1` pixels in the reported crop, mean 8-bit RGB over 5 frames.

| region | A | A2 | B | OFF (floor) |
|---|---|---|---|---|
| underside over eco-39 (2589 px) | 25.3/42.2/71.3 | 24.2/40.1/70.7 | 17.8/27.1/46.5 | 15.9/24.2/43.6 |
| underside over eco-40 (2838 px) | 23.6/40.8/70.2 | 30.0/45.3/77.6 | 24.5/37.1/63.6 | 21.0/34.1/60.5 |
| bone away from the ecos (9096 px) | 28.3/31.3/45.6 | 28.3/31.3/45.6 | 28.2/31.0/45.1 | 27.5/30.1/44.0 |

- Light position error: A, the three bone tiles are 28.9-30.5 m from eco-39. B, every blue candidate
  is 0.6 m from eco-39 or 2.4-3.6 m from eco-40 at 100 m, inside its 5 m row. Over all 17
  candidates the largest distance to a census row is 2.4 m; nothing is drawn as a separate tile light,
  because the rows take over.
- Controls (eco-reference `blue-vent-night` jungle and `blue-cluster-night` village1, play toggles):
  on pixels stable between two of the three runs, B differs from the master pair on 3.6% / 2.5% of
  pixels (> 2 levels). Master A differs from A2 and B on 3.1% / 3.4% by the same test. Mean shifts
  are at most 0.5 levels: unchanged within particle noise.

## Follow-ups
- The underside keeps 2-3 levels over the floor in B. Within the eco's 5 m reach nothing is lit; the
  remainder is not attributed (bloom from the sprites is a candidate).
