---
title: Landmarks revision 2 - windmill draw fix, temple rings for loader-only jungle, rock village and the basin building for real
date: 2026-09-24
status: verified (windmills, rings, village2/rolling, distant tower id) / inferred (cost within noise)
sha: master after the landmark-actors change (follow-up branch)
---
## Answer (≤5 lines)
The beach windmill copies now draw: their draw connection moves from beach's foreground engine
(run only for active levels) to `*vista-landmark-engines*`, executed every frame from the
foreground phase. The forbidden jungle temple rings (`towertop`) draw from village1, where
jungle is loader-only, as a merc model from jungle's FR3 driven by GOAL, plus copied tower
spewer particles. Rock village and Precursor Basin load for real behind option 12, which also
answers the distant tower: it is the Precursor Basin building, not the citadel.

## 1. Windmills
Cause (found in review): `initialize-skeleton` -> `make-nodes-from-jg` connects the draw-control
to `(-> entity extra level) foreground-draw-engine <page>`, beach's. `bsp.gc` runs those engines
only from beach's bsp draw, which joins `*background-draw-engine*` only when beach is `'active`.
Fix (`vista-landmarks.gc`): after `initialize-skeleton`, `remove-from-process` on beach's engine
and `add-connection` to `*vista-landmark-engines* [3 * slot + page]` (six small engines, 16
connections each). `vista-landmarks-draw`, called from `drawable.gc` after the level-default
foreground engines, runs `foreground-engine-execute` on them with the slot index, so the draws
land in the undisplayed slot's own merc buckets and sink groups exactly as its bsp would put
them. The isolation guards of the landmark-actors change are unchanged (no entity link, no collision, rider, sound,
events or perm writes; killed before birth/unload). `add-tex-to-dma!` is not needed on PC: merc
textures come from the FR3.
Evidence: `$SANDOVER_DATA/shots/landmarks2/{lm,nb}/windmill-noon.png` (120 m from
`windmill-one-2`): the gear house and belt of the windmill are present with landmarks on and
absent off. hut-roof: `lm` vs revision 1's `lm`, the pixel diff now includes the windmill at
x 1107-1213 y 1085-1146 (1661 pixels) besides the waterfall (it was zero there in revision 1).

## 2. Temple rings (towertop) and tower spewers
Actors (`decompiler_out/jak1/entities/jungle-actors.json`): `towertop-1` (x 362.3, y 146.6,
z -241.1 m; skeleton `*towertop-sg*`, idle at rate 0.4) and four `jungle-part` spawners with
`group-jungle-tower-spewer` (y 70-100 m around the temple). Both are whitelisted for the
GOAL-loaded path (`*vista-landmark-types*`), which applies wherever GOAL holds jungle
undisplayed.
From village1 jungle is loader-only, so `*vista-static-landmarks*` lists them:
- the model: `vista-static-merc` builds a `pc-merc` DMA request by name (`towertop-lod0`, found
  by the C++ loader in jungle's FR3 because the loader holds jungle as a neighbour), with 9 bone
  matrices computed in GOAL (entity trans, joint spin, camera-rot), lights from the viewer's
  light group 0, into the level-default merc bucket (`merc-alpha-tex`). The idle clip is a pure
  rotation per joint: `innersphere` 36 deg/key about Y, `outerR` Z 18 + Y -9, `planet`/`planetR`
  20 deg Z tilt with +/-9 Y; those match the clip to 5e-5. `innerR` is a two-axis fit (Z -19, Y 18
  deg/key), approximate (it tumbles about a moving axis in the clip). 2 keys/s.
- the particles: `group-vista-jungle-tower-spewer` (id 1000, parts 3500-3503) is a copy of
  `group-jungle-tower-spewer` with fade distances raised to 2 km (the original fades at 50-400 m,
  so it would never show from a neighbour); one `vista-landmark` process per spawner in
  `*active-pool*`.
An entry applies while `pc-vista-neighbour?` (new kernel function: the loader keeps the level
resident as a neighbour) and GOAL has no slot for the level; when GOAL loads jungle the real
actors take over. Evidence: `hut-temple-{noon,dusk}` A/B/C: the old stand-in shows the flat ring
sprite, landmarks off show nothing above the temple, landmarks on show the ring model spinning.
From the temple top (jungle displayed) the rings are the game's own actor in all three sets.

## 3. Rock village huts (and Precursor Basin)
Rock village (village2) is not adjacent to village1 in the vista adjacency table, so the neighbour mode
never loaded it; from village1 it was the `v1-lores-vil2-canvas` / `v1-lores-vil2-wall`
stand-ins (village1 tfrag lowres), also seen from the jungle temple top. New option 12
(`vista_standin_neighbours`, default off) adds the levels depicted by the viewer's stand-in
rows (`kStandins`) to the loader's resident set, per the neighbour-load policy (only a level
among the current level's stand-ins is loaded). Rows added: village1 -> village2 (canvas, wall)
and village1 -> rolling (`v1-lores-rolling-precursor`). The real huts, pontoons and the basin
building then draw at full fidelity and the stand-ins hide. The huts are background (tfrag/tie),
so no landmark actor is needed.

## 4. The distant tower from the temple top
Projection of candidate positions into the `temple-north` view (camera on the temple top,
facing -Z): the tall brown tower with green lights at screen x 754 is the Precursor Basin
building (`v1-lores-rolling-precursor-1..3`, x -180, y 100-196, z -1536 m, 1.4 km away). The
citadel (x 2825, z -4783 m, 5.2 km) projects to x 2250, behind the temple pole, and draws
nothing: the finalboss/citadel levels are not resident and village1 has no stand-in for it
(`v1-lores-mountains` is the horizon). Rock village (x 1330) and the swamp blimp rock (x 1504)
are the other shapes on that horizon. Scope: the basin building needs no new mechanism; option
12 loads rolling (FR3 11.8 MB, 782k triangles) and draws it for real. Cost: part of the +116 MiB
VRAM of option 12 (village2 + rolling, row "village2" below); a dedicated merc or background
mesh of just the building would be cheaper but does not exist as a separate asset.

## Cost (indicative; GPU `buckets` 240-frame average, 2560x1440 MSAA 4)
`$SANDOVER_DATA/shots/landmarks2/cost/cost.json`. The hut-temple rows were solo except "off";
hut-roof ran next to another instance, so its absolute numbers are noisy.

| hut-temple-noon mode | buckets ms | VRAM MiB (nvidia-smi, whole GPU) |
|---|---|---|
| windmills only | 5.37 | 4501 |
| waterfall only | 5.46 | 4511 |
| towertop only | 5.42 | 4511 |
| spewers only | 5.32 | 4507 |
| all landmarks | 5.44 | 4507 |
| option 12 (village2 + rolling), no landmarks | 5.66 | 4623 |
| all landmarks + option 12 | 5.69 | 4623 |

Per landmark the GPU difference is inside the window noise (about 0.1 ms). GOAL memory per
process (heap used): windmill copy 2.6 KB (x3), waterfall spawner 0.7 KB, spewer 0.6 KB (x4);
the towertop has no process and writes about 1.7 KB of DMA per frame. Option 12 costs about
+116 MiB VRAM and +0.2-0.3 ms.

## Shadow sampler warning flood (not in this PR)
With actor shadows off, the `sampler2DShadow` units had no depth texture bound and the GL driver
logged `0x20096` on every draw (gigabytes per run; rig shots timed out with `no RIG:CAMERA`).
This branch first carried a 1x1 depth-texture bind in `ActorShadows.cpp`; the neighbour-fog change landed the same
fix on master (with NEAREST filters, so the dummy is texture-complete), and the rebase keeps
master's version only.

## Captures
`$SANDOVER_DATA/shots/landmarks2/{off,nb,lm}/` (manifest `sandover/shots/landmarks2.yaml`,
particles on, unified fog 150 m / 0.15 per km / H 800 m, own mood; `lm` adds option 12).
Compared as A old stand-ins, B real neighbours without landmarks, C with landmarks.

## Follow-ups
- innerR ring motion is approximate; an exact version would sample the clip (the FR3 has no
  animation data, so the keys would be copied into GOAL as a small table).
- Option 12 and the stand-in rows cover village1 only; the other hosts' rows follow the
  policy's data-driven table.
- Towertop lighting uses the viewer's light group 0, not jungle's mood.
