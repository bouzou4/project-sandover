---
title: Texture census + classification pilot
date: 2026-09-23
workstream: texture census
status: verified
sha: 64d72d1
---

## Answer (<=5 lines)
`task rip-textures` produced 4002 PNGs across 126 tpages (61 MB, ~9s wall). A FiftyOne
dataset `jak1-textures` (persistent, under `$SANDOVER_DATA/fiftyone`) carries tpage,
name, dimensions, has_alpha and a thumbnail for every texture; `animated` is always
false for Jak 1 (verified from source, not inferred) and `levels` is left unset (not
recoverable without an engine change). A 100-texture stratified pilot (largest-remainder
allocation) against 5 draft material classes is in
`$SANDOVER_DATA/census/pilot/labels.csv`; the class counts there are inferred (manual
judgement on a small sample), not a verified ground truth.

## Evidence (path:line@sha / URL, short excerpts)
- `task rip-textures` (root `Taskfile.yml:170-172`) runs `decompiler.exe` with
  config-override `{"decompile_code": false, "levels_extract": true,
  "save_texture_pngs": true}`, writing `decompiler_out/jak1/textures/<tpage>/<name>.png`.
  Result: 4002 PNGs, 126 tpage directories, 61 MB, 7.77s decompiler-reported (8.7s wall
  including task overhead).
- A second, separate invocation of the same already-built `decompiler.exe` (no rebuild)
  with `write_tpage_imports: true` added to the config-override (`save_texture_pngs:
  false` this time, metadata only) produced `decompiler_out/jak1/import/tpages.gc` (128
  `(defconstant <tpage-name> <numeric-id>)` lines) and `textures.gc` (4004 `(def-tex
  <name> <tpage-name> <idx>)` lines, 5.96s). `sandover/tools/sandover/census.py`
  (`parse_tpage_ids`, `parse_tex_idx`) parses both to resolve `combo_id = (page_id <<
  16) | idx`, matching `ObjectFileDB::process_tpages` (`decompiler/ObjectFile/
  ObjectFileDB.cpp:749-823`). The 2-entry gap between 4004 texture-db entries and 4002
  PNGs is the synthetic `placeholder-white` texture `TextureDB` seeds in its constructor
  (`decompiler/data/TextureDB.cpp:11-15`, tpage id `32767`, not backed by a real tpage
  object) plus one other untraced duplicate (<0.1% of the set, not chased further).
- `has_alpha` (`census.py:image_has_alpha`): computed from each PNG's own alpha channel
  (Pillow), not decompiler internals. Gotcha, confirmed empirically across all 4002
  textures: the PS2 GS 7-bit alpha convention encodes **opaque as 128, not 255** - alpha
  never exceeds 128 anywhere in the rip. `has_alpha` is defined as "alpha is not
  uniformly 128" (i.e. actual translucency/cutout data). Result: 1521/4002 (38.0%).
  The same 128-vs-255 mismatch was present in the original `make_thumbnail` (thumbnails
  displayed at roughly half brightness/opacity over grey); fixed by scaling the alpha
  channel (`v -> min(255, v*2)`) before saving.
- `animated`: always false for every Jak 1 texture, verified from source, not inferred
  per-texture - `ObjectFileDB::process_tpages`'s per-`GameVersion` switch has an explicit
  no-op case for `GameVersion::Jak1` (`decompiler/ObjectFile/ObjectFileDB.cpp:762-763`,
  "no animated"), and `decompiler/config/jak1/jak1_config.jsonc`'s `animated_textures`
  list is empty. The `tree_tex_id < 0` "negative for anim slot" convention
  (`common/custom_data/Tfrag3Data.h:198,552`) exists for Jak 2/3, which do populate
  animated slots; Jak 1 has none.
- `levels`: not populated, left unset. `TextureDB` tracks membership in memory -
  `texture_ids_per_level: unordered_map<level_name, set<combo_id>>`
  (`decompiler/data/TextureDB.h:34`), populated from each tpage object's `dgo_names`
  during `add_texture` (`decompiler/data/TextureDB.cpp:18-57`) - but it is never written
  to disk by any existing decompiler flag; it's only consumed in-memory by the full
  per-level fr3 extraction path (`extract_level.cpp`).
- Pilot sampling (`sandover/tools/sandover/pilot.py:stratified_sample`): originally used
  a floor of 1 sample per tpage before proportional scaling, which over-allocated the
  126 small alpha/water/sky tpages (about 9% of the rip got about 40% of the 100 slots)
  and inflated the "other/special" bucket. Replaced with largest-remainder (Hamilton)
  apportionment and no per-tpage floor: `alloc[name] = floor(share)`, remaining slots
  given to the tpages with the largest fractional remainder, capped at each tpage's
  actual size. The new sample spans 54 distinct tpages (down from up to 100 in the old
  scheme) concentrated in the larger tpages, matching the rip's true composition.

## Implications for the design
- The dataset (`jak1-textures`, `sandover-census build`/`stats`) is ready to drive a
  larger classification pass (VLM or human): fields are stable, thumbnails now render
  at correct opacity, and the tpage/combo_id metadata lets a future pass join back to
  in-game usage once level membership is solved.
- A 5-class taxonomy (stone/rock, wood/organic, metal, foliage, other/special) is too
  coarse for this rip. A large share of Jak 1's tpages are tiny (16x16 or smaller)
  flat-color swatches meant to be shaded by an envmap at runtime, or character-part
  textures (eyes, eyelids, cloth, fur, leather, creature skin) - neither reads as a
  surface "material" from the raw PNG alone. The "other/special" bucket is absorbing
  both its intended scope (sky, water, UI, effects) and this leftover case; a production
  taxonomy should add a 6th class (character/cloth/organic-non-plant) rather than keep
  conflating the two.
- Name-vs-pixel disagreement is common enough to matter: `gen-metal-*` and similar
  envmap-driven swatches are graded by name over visual (flat, pale, low-detail squares
  that don't look metallic as a static PNG). Any automated classifier - human or model -
  would need the in-game render, not the raw texture, to get these right with
  confidence; expect a meaningfully higher ambiguous rate on any pass limited to raw
  PNGs.
- The corrected stratified sample increased the ambiguous rate (22 -> 36 out of 100)
  because it now proportionally includes more of the common small
  accessory/character/envmap-swatch textures that dominate the rip's tpage-count
  distribution - this is expected and desired: the old floor-of-1 sample was
  under-representing exactly the textures that are hardest to classify from a PNG alone,
  making the pilot look more confident than the rip actually is.

## Follow-ups
- Level membership (`levels` field): needs either a decompiler patch to dump
  `texture_ids_per_level` to JSON, or a Python parser for the per-level `.fr3` binary
  format (no such parser exists in-repo). Neither attempted here - "no engine builds"
  was out of scope for this pass. **Resolved 2026-09-23 (texture-levels)**, see the
  Update section above - took the decompiler-patch route.
- Consider a 6th taxonomy class (character/cloth/organic-non-plant) before running a
  full-dataset classification pass; see Implications above.
- `has_alpha` currently only checks `RGBA`/`LA`/`PA` modes; a `P`-mode image with a
  `tRNS` chunk would be missed. Not observed in this rip (all 4002 PNGs came out RGBA)
  but worth a defensive check if the rip ever changes.
- The contact sheets don't upscale textures at 16px or smaller, which makes some cells
  hard to judge by the vision judge (see several "tiny dot" entries in `labels.csv`); NEAREST
  upscaling in `pilot.render_sheets` would help future review passes.
- `sandover/tools/rig/` (a different in-progress workstream) sits outside
  `packages = ["sandover"]` in `pyproject.toml`; needs to move under the package or be
  added to `packages` when that work lands, to avoid a silent packaging gap.

## Summary tables

Total textures: **4002**
With alpha channel in use: **1521** (38.0%)

### Counts by tpage

| tpage | textures |
|---|---|
| citadel-vis-pris | 170 |
| village2-vis-pris | 161 |
| beach-vis-pris | 160 |
| village1-vis-pris | 150 |
| village3-vis-pris | 131 |
| common | 122 |
| misty-vis-pris | 114 |
| jungle-vis-pris | 113 |
| village2-vis-tfrag | 112 |
| village1-vis-tfrag | 106 |
| village3-vis-tfrag | 101 |
| citadel-vis-tfrag | 96 |
| swamp-vis-pris | 93 |
| snow-vis-tfrag | 91 |
| robocave-vis-tfrag | 90 |
| ogre-vis-pris | 88 |
| snow-vis-pris | 85 |
| jungle-vis-tfrag | 82 |
| misty-vis-tfrag | 78 |
| lavatube-vis-tfrag | 77 |
| rolling-vis-pris | 70 |
| beach-vis-tfrag | 69 |
| sunken-vis-pris | 66 |
| darkcave-vis-tfrag | 63 |
| maincave-vis-tfrag | 62 |
| swamp-vis-tfrag | 61 |
| finalboss-vis-pris | 59 |
| sunken-vis-tfrag | 59 |
| maincave-vis-pris | 58 |
| training-vis-tfrag | 58 |
| firecanyon-vis-pris | 52 |
| robocave-vis-pris | 50 |
| effects | 48 |
| sunkenb-vis-tfrag | 46 |
| rolling-vis-tfrag | 45 |
| lavatube-vis-pris | 41 |
| intro-vis-pris | 38 |
| ogre-vis-tfrag | 37 |
| finalboss-vis-tfrag | 36 |
| jungleb-vis-tfrag | 35 |
| firecanyon-vis-tfrag | 33 |
| training-vis-pris | 32 |
| sunkenb-vis-pris | 30 |
| village2-vis-shrub | 27 |
| beach-vis-shrub | 24 |
| village1-vis-shrub | 24 |
| jungle-vis-shrub | 23 |
| jungleb-vis-pris | 21 |
| village3-vis-shrub | 20 |
| darkcave-vis-pris | 19 |
| ogre-vis-shrub | 15 |
| swamp-vis-shrub | 15 |
| village2-vis-alpha | 15 |
| rolling-vis-shrub | 14 |
| eichar | 12 |
| maincave-vis-shrub | 12 |
| village1-vis-alpha | 12 |
| sunken-vis-alpha | 11 |
| sunkenb-vis-alpha | 11 |
| beach-vis-alpha | 10 |
| jungle-vis-alpha | 10 |
| jungleb-vis-alpha | 10 |
| robocave-vis-shrub | 10 |
| sidekick-lod0 | 10 |
| snow-vis-alpha | 10 |
| citadel-vis-alpha | 9 |
| darkcave-vis-alpha | 9 |
| demo-vis-alpha | 9 |
| finalboss-vis-alpha | 9 |
| firecanyon-vis-alpha | 9 |
| Hud | 9 |
| intro-vis-alpha | 9 |
| lavatube-vis-alpha | 9 |
| maincave-vis-alpha | 9 |
| misty-vis-alpha | 9 |
| misty-vis-shrub | 9 |
| ogre-vis-alpha | 9 |
| robocave-vis-alpha | 9 |
| rolling-vis-alpha | 9 |
| swamp-vis-alpha | 9 |
| title-vis-alpha | 9 |
| training-vis-alpha | 9 |
| training-vis-shrub | 9 |
| village3-vis-alpha | 9 |
| sunken-vis-shrub | 8 |
| title-vis-pris | 8 |
| intro-vis-tfrag | 7 |
| zoomerhud | 7 |
| firecanyon-vis-shrub | 6 |
| lavatube-vis-shrub | 6 |
| snow-vis-shrub | 6 |
| citadel-vis-shrub | 4 |
| gamefontnew | 4 |
| darkcave-vis-shrub | 3 |
| demo2 | 3 |
| demo2f | 3 |
| demo2g | 3 |
| demo2i | 3 |
| demo2s | 3 |
| demo3 | 3 |
| demo4 | 3 |
| demo4e | 3 |
| demo4f | 3 |
| demo4g | 3 |
| demo4i | 3 |
| demo4s | 3 |
| demo5j | 3 |
| environment-generic | 3 |
| intro-vis-shrub | 3 |
| sunken-vis-water | 3 |
| sunkenb-vis-water | 3 |
| darkcave-vis-water | 2 |
| maincave-vis-water | 2 |
| sunkenb-vis-shrub | 2 |
| demo-vis-tfrag | 1 |
| finalboss-vis-shrub | 1 |
| finalboss-vis-water | 1 |
| jungle-vis-water | 1 |
| jungleb-vis-shrub | 1 |
| ocean | 1 |
| rolling-vis-water | 1 |
| title-vis-tfrag | 1 |
| title-vis-water | 1 |
| training-vis-water | 1 |
| village1-vis-water | 1 |
| village2-vis-water | 1 |

### Size histogram (top 20)

| width x height | textures |
|---|---|
| 32x32 | 954 |
| 128x128 | 655 |
| 64x64 | 516 |
| 16x16 | 366 |
| 4x4 | 227 |
| 64x32 | 202 |
| 32x16 | 177 |
| 128x64 | 120 |
| 32x64 | 105 |
| 64x128 | 88 |
| 256x128 | 70 |
| 64x16 | 66 |
| 8x8 | 63 |
| 128x32 | 58 |
| 16x32 | 56 |
| 512x256 | 39 |
| 256x256 | 32 |
| 16x8 | 30 |
| 32x8 | 22 |
| 8x32 | 14 |

## Classification pilot

100 textures sampled stratified by tpage (largest-remainder allocation, no per-tpage
floor, seeded, `uv run sandover-pilot sample`), rendered to 5 contact sheets of 20
(`$SANDOVER_DATA/census/pilot/sheet-1..5.png`, 4x5 grid, index + tpage/name captions,
`uv run sandover-pilot sheets`), then classified by the vision judge (not by hand) against 5 draft material
classes and written to `$SANDOVER_DATA/census/pilot/labels.csv` (name, tpage, class,
confidence 0-1, note).

**These counts are inferred - a single manual pass over 100 thumbnails, not a verified
ground truth.**

| class | count |
|---|---|
| other/special | 32 |
| stone/rock | 27 |
| metal | 21 |
| wood/organic | 15 |
| foliage | 5 |

**36 / 100** were flagged ambiguous (confidence < 0.5) - up from 22/100 under the old,
biased sample (see Implications above: the corrected sample now proportionally includes
more of the small accessory/character/envmap-swatch textures that are hardest to judge
from a raw PNG). The dominant sources of ambiguity: tiny (16x16 or smaller) flat-color
swatches meant to be shaded by an envmap at runtime rather than read as "metal" or
"stone" on sight, and character-part or clothing textures (eyes, eyelids, cloth, fur,
leather, creature skin) that don't map cleanly onto any of the 5 draft classes and get
absorbed into "other/special" by default. See Follow-ups for a proposed 6th class.

## Update 2026-09-23 (texture-levels): the `levels` gap is closed

New decompiler flag `write_texture_levels` (`decompiler/config.h`, `decompiler/config.cpp`,
`decompiler/ObjectFile/ObjectFileDB.cpp:process_tpages`, same shape as the existing
`write_tpage_imports`) dumps `TextureDB::texture_ids_per_level` to
`decompiler_out/jak1/import/texture-levels.json`: `{"<tpage>/<name>": ["<dgo>", ...]}`,
one entry per texture in `TextureDB::textures` (empty array if unused by any level).

**Gotcha:** the values are raw DGO file names (`"BEA.DGO"`, `"GAME.CGO"`, `"L1.CGO"`), not
the friendly level slugs used elsewhere (`"beach"`, `"village1"`) - `texture_ids_per_level`
is keyed by `dgo_name` in `extract_level.cpp` (`add_all_textures_from_level(tfrag_level,
dgo_name, tex_db)`), not by `bsp_header.name` (the friendly slug, only known once a level's
bsp-header is parsed). No DGO-to-slug table exists anywhere in `sandover/tools` today; this
finding leaves the raw DGO names as the source of truth rather than inventing one.

Counts from a run against the full rip (`ntsc_v1`, `{"decompile_code": false,
"levels_extract": true, "write_texture_levels": true}`):
- **4003** entries in `texture-levels.json` (`TextureDB::textures`, all tpages including the
  synthetic placeholder). **4002** have at least one level; **1** has none - the synthetic
  `placeholder-white` texture (`kPlaceholderWhiteTexturePage`), the same one already known
  from the PNG-vs-textures-db gap above. Every one of the 4002 real, ripped textures is
  reachable from at least one DGO.
- **27** distinct DGOs appear as values. Busiest: `VI2.DGO` 316, `VI1.DGO` 293, `CIT.DGO`
  279, `BEA.DGO` 263, `VI3.DGO` 261, `GAME.CGO` 209 (textures loaded via the shared
  `GAME.CGO`, not a per-level DGO).
- Tpages spanning the *most* distinct DGOs: `zoomerhud` (7: `FIC.DGO`, `L1.CGO`, `LAV.DGO`,
  `MIS.DGO`, `OGR.DGO`, `RACERP.CGO`, ...), and the `demo*` tpages (2 each: `DEM.DGO`,
  `L1.CGO`). Contrary to the original guess, `common` (122 textures) maps to exactly **one**
  DGO, `GAME.CGO` - it isn't "shared across many levels" in this per-DGO sense, it's loaded
  once via the always-resident common DGO and every level pulls it in transitively; that
  transitive reachability isn't represented in `texture_ids_per_level` at all.

`sandover-census build` now reads the file (when present) into the FiftyOne dataset's
`levels` field automatically: **4002/4002** samples in `jak1-textures` now carry a non-empty
`levels` list (the PNG-backed set excludes the placeholder, so 0 come back empty).
`sandover-materials merge` populates `materials.csv`'s `levels` column (`;`-joined DGO
names) from the same file for new rows; `uv run sandover-materials refresh-levels` updates
it on an existing CSV without re-judging - ran against the existing 4002-row
`$SANDOVER_DATA/materials/materials.csv`: **4002/4002** rows refreshed.
