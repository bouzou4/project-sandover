---
title: Can neighbour landmark actors (beach waterfall, windmills) draw for real in vista neighbour mode?
date: 2026-09-24
issue: "#156 (item 5, landmark actors)"
status: verified (beach waterfall) / open (windmills spawn but are not seen) / refuted (loader-only neighbours such as jungle)
sha: da4107cab (branch p4/landmark-actors)
---
## Answer (≤5 lines)
Yes for a neighbour that GOAL has loaded but does not display: at `village1-hut` beach is loaded
(`:disp1 #f`), so its entity table, code and art are in memory. With neighbour mode on, the
whitelisted beach actors (`windmill-one` x3, the `beach-part` running `group-beach-waterfall`) are
spawned as display-only `vista-landmark` processes. The waterfall draws for real; the three
windmill copies are alive and animating but produce no pixels in the captures (open).
No for a loader-only neighbour (jungle, misty, firecanyon, training at village1-hut): GOAL has no
entity table, types or art for them, so nothing can be spawned there (see Implications).

## Which waterfall
The waterfall seen from the village1 hut roof and from the start is the beach one, not jungle's:
`beach-part-48` (`group-beach-waterfall`, x -120 m, z -488 m) projects to the centre of the
hut-roof view; the jungle `group-jungle-waterfall-*` spawners sit at x 207-399 m, out of frame.
`jungle-water` is the jungle river surface (`water-anim` look 34, `*water-anim-jungle-river-sg*`),
not a waterfall. Positions from `decompiler_out/jak1/entities/{beach,jungle}-actors.json`.

## What runs and what is skipped
`goal_src/jak1/pc/features/vista-landmarks.gc`. A landmark is not the actor's own process: it is
a `vista-landmark` (a `process-drawable`) that reads the entity for position, rotation, lumps and
level, and reuses the actor's skeleton group or particle group.

| actor type | runs | skipped (vs the real actor) |
|---|---|---|
| `windmill-one` | `initialize-skeleton` with `*windmill-one-sg*`, the idle animation looped at the actor's rate (0.5), `ja-post` | `collide-shape-moving` and its prims (never created), `rider-trans`/`rider-post`, the `gears-rumble` sound, `platform` process mask |
| `beach-part` (`group-beach-waterfall` only) | `create-launch-control` on the level's group, `spawn` every frame at the entity position (as `part-spawner-active`) | `ambient-sound` (the `'waterfall` effect), the `stop`/`start`/`trans` events, the perm `complete` bit |

Guards (what keeps landmarks out of game state):
1. No entity link: the process gets the entity only during its init and clears `entity` after,
   and `(-> ent extra process)` is never set, so `birth!`, `kill!`, `entity-deactivate-handler`,
   perm status and `reset-actors` bookkeeping of the level never see it. An unbirthed level's
   entities have `extra` = 0 (`add-to-level!` runs at birth), so the init lends a private
   `entity-links` (inline in the process) and restores `extra` after.
2. Lifetime: `vista-landmarks-update` (called from `level-update` after `update! *load-state*`)
   keeps landmarks exactly for level slots that are `'loaded`, not `display?`, with a bsp, while
   `pc-vista-landmarks?` (neighbour mode and option 11) is true. `birth` and `unload!` of a level
   kill its landmarks first (hooks in `level.gc`), so they never coexist with the real actors or
   outlive the art.
3. A probe handle notices when something else emptied `*entity-pool*` (`reset-actors`, which the
   rig calls every shot) and respawns on the next frame.
4. No collision, no rider, no sound, no events: the process has none of them, so there is nothing
   to guard in the actor code itself. `actor-pause` is cleared (the level is far from the player).
5. Draw state: the draw goes to the loaded level's sink group and uses `tod moods <slot>` for
   lights (see mood), writes `closest-object`/`texture-mask` of the undisplayed level only, and
   sets `was-drawn` on its own `draw-control`. Nothing of the active levels is touched.

Verified in a live instance (probe via nREPL): spawn 4 at village1-hut; toggle option 11 off: 0;
on again: 4; `(pc-set-vista "none")`: 0; back to neighbours: 4; `reset-actors 'life`: 4 again the
next frame. Each spawn logs `vista-landmarks: beach spawned 4`.

## Mood, fog, shadows
- Lighting: merc lights come from `*time-of-day-context* moods <level slot>` (`drawable.gc`
  `dma-add-process-drawable`), and `update-time-of-day` runs `mood-func` for every non-inactive
  slot, so a loaded beach gets beach's own mood, not village1's. Particles use the blended
  `current-prt-color` (viewer mood), as for any particle in the game.
- Fog: merc, emerc and 3D sprites take the unified vista fog when the frame's policy is unified
  (`vista_atmo_foreground_uniforms`, `atmo` in `merc2.vert`, `emerc.vert`, `sprite3_3d.vert/frag`).
  Additive sprites fade to black instead of tinting. Off (any other policy) the shaders run the
  vanilla path; parity unaffected.
- Shadows: landmark merc draws land in the level's merc buckets, which the #152 pre-pass already
  walks, so they cast with the shadow mode on (not captured here).

## Evidence
Captures (`$SANDOVER_DATA/shots/neighbours/lm2/`, manifest `sandover/shots/landmarks.yaml`,
particles on, unified fog 150 m / 0.15 per km / H 800 m, binaries pinned `--bin p4lm-1`):
- `lm/hut-roof-noon.png`: the real falls-particle sheet and base spray on the beach cliff where
  the stand-in's baked ribbon was; `nb/hut-roof-noon.png` (landmarks off) has none. Pixel diff
  between the two is confined to the waterfall (x 1120-1200, y 680-1000).
- Windmills: from the hut roof they project at 380-630 m, 83-89 m below the camera. The lm/nb
  diff is zero at all three projected positions (hut-roof noon and dusk), so the copies do not
  draw there although their processes run `vista-landmark-anim`. Open. What was tried: a live
  probe (nREPL) listed 3 `vista-landmark` processes in `vista-landmark-anim` at the entity
  positions (x -120/-89/-282 m, z -226/-420/-499 m) with a skeleton and draw-control; the
  pixel diff lm vs nb at the three projected positions is 0 at noon and dusk and at the hut
  continue camera; a draw-status probe (draw status bits, cur-lod, level-index, sink group) did
  not complete because the slots were contended. Not tried yet: reading those draw-control
  fields, forcing lod 0 and `do-not-check-distance`, checking whether the merc buckets of an
  undisplayed level slot are flushed, and a capture close to a windmill. Particles do not depend
  on the level and are unaffected.
- `lm/hut-roof-dusk.png`: the waterfall at dusk.
- beach-v1: beach is the displayed level there, so the real actors run; landmark mode is inert
  (no undisplayed GOAL-loaded level).
Gallery: `$SANDOVER_DATA/shots/neighbours/gallery.html` (section "Landmark actors").

Parity (toggles off): `sandover-parity check --set auto-v0 --build p4lm-1` (engine da4107cab
on top of the #163 master) PASS 16/16, FLIP mean threshold 0.011399. Report:
`$SANDOVER_DATA/parity/runs/20260924-092910-check-auto-v0/`.

## Windmill cause (review, code read)
`initialize-skeleton` -> `make-nodes-from-jg` (`process-drawable.gc`) connects the draw-control to
`(-> entity extra level) foreground-draw-engine <page>`, here beach's. A level's foreground
engines are executed only from its `bsp-header` draw (`bsp.gc`), which runs through
`*background-draw-engine*`, and `level-status-set!` adds a bsp there only on `'active`. Beach is
`'loaded`, so `dma-add-process-drawable` is never called for the copies: no lod, distance or
frustum cull is involved. The waterfall draws because particles do not go through that engine.
A fix would execute the undisplayed slot's foreground engines (or re-home the connection to
`level-default`) and then check its merc buckets and textures; `add-tex-to-dma!` also skips
non-`'active` levels.

## Implications for the design
- The mechanism covers every neighbour GOAL loads without displaying. In Jak 1 that is the
  `:disp #f` half of some continue points and load boundaries (beach at village1-hut); most
  neighbours in neighbour mode are loader-only.
- Loader-only neighbours cannot run GOAL landmarks: jungle's waterfall particles
  (`group-jungle-waterfall-1..4`), its river (`jungle-water`), misty's actors and so on need the
  level's DGO (types, part groups, art, entity table) in a GOAL level heap, and Jak 1 has two.
  Options were (a) a third GOAL level slot, display-only; (b) C++ static draws of curated
  landmark models from the FR3 plus copied particle definitions; (c) keep the viewer's
  `medres-*` stand-ins. Decision (2026-09-24): (b) as landmarks revision 2 (issue #175), (c) for
  everything else, (a) rejected on memory.

## How to add a type
Append to `*vista-landmark-types*` in `vista-landmarks.gc`:
- a skeleton actor: `(list '<etype> 'skel '<skeleton-group symbol> <rate in percent>)`; the
  group's default animation plays in a loop at that rate;
- a particle spawner: `(list '<etype> 'part "<part group name>")`; only entities whose
  `art-name` lump names that group match.
The actor's level must be GOAL-loaded and undisplayed. Anything with behaviour beyond a looped
animation or a particle spawn needs its own state in `vista-landmark`.

## Follow-ups
- Windmill copies: find why the skeleton draw of an undisplayed level produces nothing.
- Loader-only landmarks: revision 2, C++ static draws (#175).
- Cost: not measured separately (4 processes; the waterfall is one part group).
