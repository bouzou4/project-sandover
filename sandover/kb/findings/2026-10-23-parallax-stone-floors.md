---
title: Stone ground got no parallax because the height map was gated on masonry names
date: 2026-10-23
issue: play capture runs/20261023-1042-play/0018 (village3-farside, 12 h)
status: verified
sha: 6d6ef3974 (base), fix/parallax-jungle-vil3
---
## Answer (≤5 lines)
Parallax runs only where a texture has a packed height map, and `derive-maps` wrote one only for
class stone textures whose name holds a masonry cue (brick, stone, block, wall, tile, cobble). The
village3 cave floor is `vil3-cave-ground` (class stone, the strongest relief in the stone class), so
it never got a height and the shader skipped it. Fix: a stone texture also qualifies when the pat
census says Jak walks on stone there (stone pat over at least 95 % of at least 10 m2 of matched
floor). 30 textures gain a height (cave floors, sulfur rock, snow rock); sand, dirt, mud and grass
stay flat.

## Symptom
Play snapshot 0018 (village3 at 12 h): "The ground here isn't getting parallax displacement for some
reason". The cobbled cave floor reads flat while the cave walls around it shift with the view.

## Mechanism
- The shader side is a per-draw gate: `pbr_px_on()` (pbr_bg_frag.glsl) needs `pbr_draw.y > 1.5`,
  which the engine sets only when the texture's map was packed with a height
  (`decode_map` in Materials.cpp reads `<name>.parallax.png` when the index entry has
  `parallax: true`). No height, no ray march: the albedo, normal and roughness are sampled at the
  unshifted UV. Not a tfrag/tie, prepass, hi-Z or LOD question: the floor is LOD 0
  tfrag and its draw simply carries no height.
- The height is written by `derive-maps --parallax` through `parallax_qualifies`
  (pbr_maps.py): class in `[parallax] classes` and a masonry cue in the name. The index had
  `village3-vis-tfrag/vil3-cave-ground` and `vil3-ground-01` as class stone, coverage 1.0,
  `parallax: false` (slope p95 24.5 and 8.1 degrees; the masonry set spans 3.9 to 31).
- Names are the wrong signal for floors: the rule is stone and
  masonry, cave rock included, never narrowed to masonry. Class stone alone is too wide, because
  the table folds sand, dirt and mud into stone (161 stone rows have no cue; `bch-beach-01`,
  `swp-mud-floor`, `mis-ground` among them). The collision pat under a floor is the ground truth
  for what it is (`pat_census.py`): `vil3-cave-ground` is 100 % stone pat, the beaches are sand,
  the swamp floors dirt. Walls keep the name rule because walls carry the default stone pat.
- The rule: class stone (or ice, below) and (a masonry cue, or walkable pat stone over >= 95 % of
  the matched area and at least `floor_min_area_m2` = 10 m2 of it, the census row of the level with
  the most matched area). 95 % keeps out mixed beach and
  mud floors that read partly stone through the default pat (`jng-beach-01` 77 %, `swp-mud-top`
  94 %). The opt-in/opt-out column still wins. Normal and roughness maps are unchanged (byte
  identical on re-derive); only the height is added.
- Minimum area: stone-pat matches in the census run from 0.4 m2 to 44 000 m2. Under 10 m2 they are
  props matched on a handful of collision cells, not ground: the threshold excludes
  `vil3-miner-camprock` (0.4 m2), `robocave cv-stal-01-merge` (5.0) and `bch-idolrubble` (9.6, flat
  anyway); the smallest floor that qualifies is `ogr-cave-stalag-mid` (18.6), the cave floors are
  1 000 to 44 000 m2.
- Count: the pat test without the area floor adds 35 textures; the area threshold removes the
  three above and `min_coverage` (mostly cut-out) drops `maincave cav-pebble-all-01` and
  `vil3-dmg-plate-02`, so 30 get a height. Three of the 30 are shrub pebbles, which take the height but draw no parallax (shrub has no
  parallax path).
- Class ice joins `classes`: the snow ice walls were reclassified stone -> ice after their
  heights were derived, so the live index still flags them, but the old rule no longer selects
  them. With `--parallax` now clearing heights that no longer qualify (below), they would have been
  dropped; with ice in `classes` they qualify by the wall cue, as confirmed for ice walls.
  Their maps are kept as derived; a fresh re-derive would use the ice recipe.
- Dark Cave floors (`cv-cavefloor-01-2x2`, `cv-floorfringe-01/02/03-2x2`) are in the set. Parallax
  adds no light: it moves the UV at which the albedo, normal and roughness are read, and the
  self-shadow only removes part of the baked direct sun, which is none in the Dark Cave. The cave
  stays as dark as before; a hit crystal's light shows the floor relief through the mapped normal.

## Tooling
- The census is a pinned input, `materials/pat-census.csv` (a copy of the census run, not the
  `candidates/` output); `derive-maps` fails without it, so a full re-derive cannot silently drop
  the floor heights.
- `derive-maps --parallax` clears `parallax` in the index and deletes `<name>.parallax.png` for every
  texture that no longer qualifies (`prune_parallax`), so a rule change cannot leave stale heights.

## Proof
Rig re-shoot of snapshot 0018 (`from-snapshot --play-look`, 3840x2160), master maps against the
candidate maps (`SANDOVER_PBR_MAPS`), same engine and shaders. Debug view 7 (`(pc-set-pbr-param 4
7000000)`: red = the draw has a packed height, green = parallax offset), floor band (rows 1300-2160):

| frame | packed-height share | offset > 1/1024 UV share | mean offset (green) |
|---|---|---|---|
| master maps | 0.000 | 0.000 | 0.000 |
| this branch | 0.970 | 0.765 | 0.752 |

The test (packed share > 0.5 on the floor band) fails on the master frame and passes on the new one.
In the shaded frames the cobbles take relief and self-shadow (`look/floor-crop-ab.png`); the walls,
props and sky are unchanged (mean abs difference 0.41 of 255 in the top 500 rows, the floor band
4.8). Frame cost not measured: the rig was shared with other captures under memory pressure; the
parallax and self-shadow marches together measured 0.1-1.6 ms where they run
(`2026-10-23-pbr-tie-cost.md`), and this adds them on the floor of the levels above.

## Data
- Candidate maps: `$SANDOVER_DATA/materials/candidates/parallax-floor-pbr-maps/` (a copy of
  `pbr-maps` plus the 30 heights, index updated). Applying the fix = re-run
  `derive-maps --parallax` (or copy that folder's 30 entries and its index) into
  `materials/pbr-maps` after the merge.
- Captures: `$SANDOVER_DATA/shots/parallax-jungle-vil3/look/` (`before*`, `after*`, `view7-ab.png`,
  `floor-crop-ab.png`); the frames one level up were shot without `--play-look` (vanilla look, void).
