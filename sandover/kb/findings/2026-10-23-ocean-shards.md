---
title: Ocean black seams and bright shards (play run 2026-10-23)
date: 2026-10-23
status: fixed (seams, shards); tiling and the two eco reflections are separate, not fixed here
sha: master 6d6ef3974 (base)
---
## Answer (5 lines)
Two mechanisms. (1) Since the water-transparency change the near ocean re-copies the depth buffer just before it draws, so a
swimmer is fogged by his own depth. That copy also holds the mid ocean's displaced surface, and
the near ocean's wave shoaling read it: next to the mid ocean the "seabed" was the water itself, the
near rim lost its waves while the mid kept them, and a black crack opened along every crest. (2) The
shoal weight was taken per tessellated vertex and steps from 0 to 1 within one 0.75 m segment at a
silhouette, so crests stood up as bright, steep sheets beside rocks, banks and reeds.

## Symptoms (run 20261023-1042-play)
- 0007 (jungle 19 h), 0011 (village1 12 h): black jagged lines across the sea, along the crests.
- 0003, 0004 (jungle river at the fisherman, 12 h): large bright cyan triangles on the water near
  the banks and reeds. 0007: blue sheets along the waterline of the rock Jak stands on.
- The same shoreline sheets are the "polygons moving at the shoreline" reported since 2026-10-22
  (`findings/2026-10-22-shore-temporal-jitter.md` on the draft branch): with the wave clock
  running, the sheets slide with the crests.

## Mechanism 1: the shoaling read the mid ocean (regression from the water-transparency change)
- The mid ocean draws after bucket 18 with depth writes on (`flush_mid_deferred`). The shoal fade
  (`water_shoal_at`, water_waves.glsl) reads the scene depth behind each rest point and a ring of
  `shoal_m` (2.5 m) around it, and turns the waves off where the water is shallow or where
  something stands within 3 m in front of the point (`water_shoal_occ`, -4 per sample).
- Before that change both oceans read the copy taken before the mid ocean: seabed only. That change re-captured
  `m_water_depth` at `OCEAN_NEAR` into the same texture. From then on, the near ocean's ring samples
  that land on the mid ocean see a surface 0 to 1 m away: shallow, or "in front", so the weight drops
  to 0. The near's rim vertices sit exactly where the mid's do (`near_rim_on_plane`), but one
  mesh now kept full-height crests and the other went flat, so a black gap opens between them along the crests. Anything else drawn between the two copies
  (actors, transparent sheets; which ones at 0003 was not isolated) scattered the same zero weights
  over the open river, and crests next to them stood up as shards.
- Fix: the near ocean keeps its re-copy for what it was added for (the per-pixel absorption and
  tint read `water_depth`), and the vertex shoaling reads the pre-mid copy through its own sampler
  (`water_shoal_depth`, unit 12, `SharedRenderState::water_shoal_tex`). The re-copy goes to a second
  single-sample target (`m_water_depth_near`), so the first copy is no longer overwritten. One more
  4K depth texture (about 33 MB); the number of copies per frame is unchanged.

## Mechanism 2: a hard step in the wave weight
- The shoal weight is the mean of 9 samples, and an occluded sample counts -4, so two such samples
  flatten the point (the shoal-ring rule for floats and decks). As a vertex moves away from a rock, its ring
  samples leave the silhouette one by one, and the weight goes from 0 to 1 over one or two samples.
  The tessellator cuts edges into 0.75 m segments, so the surface rose by a full crest (up to about
  1.4 m) within one or two segments. Those near-vertical triangles catch the sky reflection and the
  glint as bright sheets. Water view 18 shows it: the zero-weight band beside the bank has a hard,
  jagged edge.
- Isolation at 0007 (shoal settings reset in every shot, clock pinned): sheets with the shipped
  settings; none with displacement off (`pc-set-water 7 0`) or shoaling off (`53 0`); `53 5` moves
  them further out; hex tiling off and glint off keep them.
- Fix: the tessellation control stage computes the shoal weight once per patch corner (the game's
  near triangles, a few metres across) and the evaluation stage interpolates it across the patch
  (`water_shoal_fixed`). The weight now ramps over a whole patch instead of stepping between
  neighbouring tessellated vertices. Corners are shared between patches, so the weight is
  continuous across edges. It is also cheaper: 3 ring evaluations per patch instead of one per
  tessellated vertex. Floats and decks still flatten the water under them (village2 planks, below).
- New behaviour, the T-junction mismatch: the near ocean's rim vertices take their own shoal weight
  from the ring at their own position, while a longer mid edge they T-join interpolates between the
  mid's two corners. In shallow water the two weights differ, so the rim can step against the mid
  edge. At the bundle cameras this leaves 35 (0007, 3 after the master merge) and 0 (0011)
  uncovered seam pixels. Follow-up: snap the near rim's weight to the mid's interpolated value
  (evaluate the rim vertex's weight from the mid edge's end points), as `near_rim_on_plane` already
  does for the rest position.
- Coarser sampling: the weight is sampled at the patch corners (6 to 12 m apart) with a 2.5 m ring,
  so an obstacle under about 7 m standing alone in shallow open water can fall between the corners
  and the rings, and a crest could rise through it. At the bundle cameras it did not: the village1
  dock posts (0011) and the jungle reed clumps (0003) sit in flat water with their foam rings in the
  fix, and master showed shards there. A post far from any bank was not shot.
- The corner evaluation runs only with displacement on (`water_disp_on`), so the ring taps are not
  paid when the waves are off.

## Proof (rig, bundle cameras, wave clock pinned at 5 s, play look; A = master bin 6d6ef3974, B = fix)
Frames: `$SANDOVER_DATA/shots/ocean-shards/{before,after-final,v2-before,v2-after}`;
metric: `proof.json` next to them.
- Seam (water view 20 at the near/mid boundary: pixels within 8 px of both meshes that neither covers
  and are black): 0007 3523 to 35, 0011 1214 to 0.
- Shards (bright blue-cyan pixels 30 levels over their 31 px median, foam excluded, in the shard
  region): 0003 open river 2734 to 350, 0007 rock waterline 6062 to 168, 0004 left of Jak 3254 to 0.
- Village2 (run 20260926-1011 0014, 0015, the shoal-ring cameras): the black crest seams in A are gone in
  B; no crest rises between the plank floats.

## Not fixed here
- 0009 "tiling is brutal" (temple top, 19 h): a regular lattice of lit crests on the sea below.
  It is the Gerstner chop itself, not the texture: it stays with hex tiling off, texture ripple off,
  sparkle off and glint off (the glint only changes its brightness), and goes with displacement off.
  Vanilla's sea is flat there. Reducing it changes the wave spectrum (more incommensurate lengths,
  stronger crest warp, or more waves), a look change needing its own comparison, not this fix.
- 0008 "polygons in the eco reflection" (jungle, blue eco): the purple light on the canyon wall
  ends exactly at a triangle edge. Not water: the eco light lands on one face and not its neighbour.
  Hypothesis, not diagnosed: a per-face term in the eco point light or the highlight lobe.
- 0016 "the eco shot glow does not read like muddy water" (swamp): inland water reflects the eco
  shot as a clean streak. Hypothesis: the swamp water's roughness or mud lobe is too smooth for an
  emissive source; inland water (merc2.frag), separate from the ocean.
