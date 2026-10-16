# Materials how-to (texture classification, Jak 1)

The material taxonomy is data-driven: `sandover/presets/materials.toml`
is the single source of truth for the classes and tags below, the judge prompt's class bullets,
the reviewer's buttons/legend/keyboard shortcuts, CSV validation and the FiftyOne
`material_class`/`material_tags` fields (`sandover.materials.load_taxonomy`). **Adding a class or
tag is a TOML edit** -- add a `[[classes]]` or `[[tags]]` entry with a `name`, a spare `key` (one
letter or digit, matched case-insensitively, unique across every class and tag) and a one-line
`description` (used verbatim as the prompt bullet and the reviewer legend's tooltip); nothing else
needs to change. Loading the file rejects a duplicate name or key, or a key that collides with the
reviewer's reserved keys (`s`/`b`/`v`).

Every ripped texture gets one of ten material classes: `stone`, `wood`, `metal`, `foliage`,
`organic` (character parts: skin, eyes, fur, leather, creature hide), `special` (sky, water,
effects, UI; no shading), `glass` (specular, transparent: windows, bottles, lenses), `cloth`
(fabric with a sheen: clothing, banners, curtains), `snow` (snow), `lava` (molten
rock, emissive) and `ice` (clear or blue ice, below). Results of the first full pass (eight-class taxonomy):
`kb/findings/2026-09-23-texture-classification.md`.

Orthogonal to class, four tags flag a shader/material nuance without changing the class:
`fur` (the fur shell shader: Daxter, lurkers, anything hairy), `precursor` (bronze-like
Precursor metal; class stays `metal`) and `baked-tint` (a metal whose reflection keeps the
baked light's colour instead of its luminance; set on the 33 metal `*ecovalve*` rows so the eco
vent dishes keep vanilla's eco-coloured glow, #275; with metal hue on, below, every metal does
this and the tag only matters with `(pc-set-pbr-param 25 0)`) and `shaft` (a fake light shaft:
tfrag skips its draws in the remaster look, `lighting.md` section 9; below). Jak's two
nail-strip textures are corrected to `organic` by hand (#264, below).

Fur shell size follows the texture's UV scale when `(pc-set-fur 8 <exp>)` is above 0: Merc2 takes,
once per (level load, model, texture), the median metres covered by one UV unit over the model's
draws of a fur-tagged texture, and scales the shell length by `s = clamp((m_per_uv / ref)^exp, 1, 16)`
with `ref` from `(pc-set-fur 9 <m>)` (default 0.3: every lurker and Daxter, at most 0.29, keep their
length; only coarser fur such as the ogre boss, 0.47-1.26, grows). The same `s` divides the root
darkening and the strand coverage where the surface faces the camera (full coverage below n.v 0.15,
1/s above 0.6), so long shells keep their fringe at the silhouette while the base texture stays
readable on the body. Measured medians: Daxter 0.09-0.14, lurker puppy 0.10-0.27, babak 0.20-0.29,
ogre boss 0.47-1.26. Default exponent 1.75 (the chosen default; shells themselves stay off
at boot and on in the play look); `(pc-set-fur 8 n)` sets the exponent (0: the old rule).

On the 100-texture spot-check, the maintainer's read is that the judges' class was outright
correct on roughly 70% of textures, and that the texture name was a real help in making
that call (not just the pixels) -- corroborates the judge prompt's "pixels first, name as a
hint" instruction, and is the kind of signal `rejudge --with-notes` is for (below).

Everything below lives under `$SANDOVER_DATA/materials/` and never enters git. The only
committed rows are the 20-row schema example `sandover/presets/materials.sample.csv`.

```
materials/
  manifest.csv            # index -> combo_id, tpage, name, size, has_alpha, envmap_swatch
  sheets/sheet-NNN.png    # 201 numbered contact sheets, 20 textures each
  prompts/<pass>-NN.txt   # the vision judge prompt per batch
  judgements/<pass>/batch-NN.json   # judge output: [{index, class, confidence, note}]
  judgements/rejudge/<timestamp>/   # rejudge batches: manifest.csv, sheets, prompt.txt,
                                     # judgement.json, merged.json (see `rejudge` below)
  materials.csv           # merged result (the class schema plus judge_agreement and tags)
  spotcheck/              # 100-texture human set: spotcheck.csv + 5 sheets
  pbr-maps/               # derived normal/roughness maps, see "PBR maps"
```

## The `ice` class and the ice rule

Class `ice` (engine class 12, reviewer key `C`) is for clear or blue ice: frozen lakes, ice
walls, icicles, ice crystals. Background draws (tfrag, tie, shrub) of an `ice` texture get the
ice response in `pbr_bg_frag.glsl` (`pbr_ice_shade`): a smooth fresnel coat reflecting the mood
environment (capped by the sky share like the swamp surround) with a sharp sun lobe; an inner
layer, the texture sampled again below the surface along the refracted view ray (n = 1.31) at
`depth_m`, blurred about two mips, tinted by the texture's own normalized colour over the path
length (Beer-Lambert, one application per `absorb_m`); and sun transmission on faces turned
from the sun and edges seen toward it, in the squared ice colour. Blue-shifted texels take the
full response and near-white ones (snow at the foot of an ice wall) about a third. Merc draws
(characters: the ice-cube enemy) get the same rule in `merc2.frag` (`pbr_ice_merc`) in camera
space, lit by the actor's key light instead of the sun; generic draws get only the class
parameters (`[class.ice]`).

Toggle: `(pc-set-pbr-param 40 n)` (millionths), `[global] ice` in `pbr.toml` (1.0). The preset
is read at renderer start and on every look change (`pc-set-pbr-param 11` reloads it, which
resets an earlier param 40), so the value is 1 from start-up; boot stays vanilla through the
material shading toggle (param 0), as with `metal_hue`. Send param 40 after any look change.
At 0 an `ice` row takes stone's response (class parameters, procedural detail layer, wetness
weight), not its own previous class: 11 of the 17 rows were stone and match; `snow-ice` and
`snow-fringe-01` were `special` (unshaded), the three `icecube-*` rows `organic`, and
`snow-ice-trans-01` `snow` (with sun glints), so those six do not return to their old look at 0.
Ice takes no snow glints by design (the coat has its own sun highlight). With the rule on, ice
takes no wetness. The per-texture maps match stone only while they were derived with the row
as stone: `derive-maps` on a table with `ice` rows uses the `[class.ice]` recipe. Shape: `[ice] depth_m, absorb_m, roughness, transmit`.
Chosen 2026-10-08 (the moderate strength 1.0): the 17 `ice` rows of
`$SANDOVER_DATA/materials/candidates/ice-corrections.csv` are applied to the live table
(`sandover-materials apply --csv`, backups `materials/backup/*.before-ice-<stamp>.*`): `snow-ice`
(tfrag and alpha), `snow-ice-trans-01`, `snow-icewall`, `-01` (tfrag and shrub), `-01-trans`,
`-02..05`, `-trans-02`, `-trans-03`, `snow-fringe-01`, and the ice-cube enemy's `icecube-ice-01`,
`-ice-corner`, `-nails`. `snow-crystals` stays `special` (a light source). The PBR maps are the
ones the round used (derived as stone); `derive-maps` on these rows would use `[class.ice]`.

## Prerequisites

- `task rip-textures` has run and `uv run sandover-census build` built the FiftyOne
  dataset `jak1-textures`.
- `SANDOVER_ROOT` and `SANDOVER_DATA` are set when running from a worktree.

## Re-run from scratch

```sh
cd sandover/tools
uv run sandover-materials sheets            # manifest + 201 sheets (about a minute)
uv run sandover-materials prompt --pass a --batch 0   # print one judge prompt
```

The vision judge runs as 40 independent sessions: 20 batches of about 10 sheets for pass
`a`, and 20 for pass `b`, whose batches are shifted by half a batch so every sheet is seen
by two different judges. Give each session the text of its prompt file; it opens the sheets
as images, classifies each cell and writes its `batch-NN.json`. Run at most six at a time.
Sheets show alpha at twice the PS2 value (128 = opaque) over a checkerboard, with an opaque
colour inset for translucent textures, and upscale tiny textures with nearest-neighbour.

```sh
uv run sandover-materials status    # which batches still have gaps; re-run those
uv run sandover-materials merge     # writes materials.csv
uv run sandover-materials load      # FiftyOne fields + the "needs-review" saved view
uv run sandover-materials summary --markdown summary.md
uv run sandover-materials spotcheck # 100 textures, stratified by class and confidence
```

`merge` rule: both judges agree, so class kept and confidence averaged; they disagree, so
the more confident class is kept at half its confidence (always below 0.5, ambiguous) and
both votes go in `note`. Rows a human already reviewed survive a re-merge.

## Columns

`combo_id` (`0xTTTTIIII`), `tpage`, `name`, `class`, `confidence`, `judge_agreement`
(`agree`, `disagree`, `single`, `none`), `source` (`judge`, `human`, `default`),
`envmap_swatch` (heuristic: at most 16 px and nearly flat), `has_alpha`, `levels`
(`;`-joined level names using this texture, from `texture-levels.json`; empty if unused
anywhere or the file wasn't generated), `reviewed`, `tags` (semicolon-separated, `fur`/`precursor`,
empty by default), `note`.

## Level membership

`decompiler/config/jak1/jak1_config.jsonc`'s `write_texture_levels` flag (same shape as
`write_tpage_imports`) makes the decompiler dump `TextureDB::texture_ids_per_level` to
`decompiler_out/jak1/import/texture-levels.json`: `{"<tpage>/<name>": ["level", ...]}`,
one entry per texture (empty list if it isn't used by any level). `sandover-census build`
reads it into the FiftyOne dataset's `levels` field automatically when the file is
present. `merge` populates `levels` on new `materials.csv` rows from the same file; to
refresh the column on an existing CSV without re-running the judges:

```sh
uv run sandover-materials refresh-levels
```

## Apply human corrections

Either route marks the row `source=human`, `reviewed=True`, `confidence=1.0`, and records
the change in `note`. A non-empty `tags` column on the correcting CSV also overwrites the
row's `tags` in `materials.csv` (an empty `tags` column leaves it alone).

- **Reviewer (recommended for the spot-check).** `uv run sandover-review-materials --csv
  $SANDOVER_DATA/materials/spotcheck/spotcheck.csv` -- see below.
- **Spot-check CSV, by hand.** Open `spotcheck/sheet-00N.png` next to
  `spotcheck/spotcheck.csv` (the sheet caption shows index, size, judged class and
  confidence). Fill `human_class` for every row you looked at: the same class to confirm,
  another to correct; optional `human_note`. Leave a row blank to skip it. Then
  `uv run sandover-materials apply --csv $SANDOVER_DATA/materials/spotcheck/spotcheck.csv`.
  Any CSV with `combo_id` and `human_class` columns works the same way.
- **FiftyOne.** `uv run fiftyone app launch` (with `FIFTYONE_DATABASE_DIR` pointing at
  `$SANDOVER_DATA/fiftyone`), open `jak1-textures`, pick the saved view `needs-review`
  (disagreements and confidence below 0.6, lowest first). Tag a sample `class:<name>` to
  correct it or `ok` to confirm it, then
  `uv run sandover-materials apply --fiftyone`.

After either, run `load` again so the dataset and the saved view reflect the corrections.

### Rule-made corrections: `baked-tint` (#275)

There is no committed rule file: a correction is always a CSV keyed by `combo_id`, and the
CSVs live in `$SANDOVER_DATA/materials/corrections/` (local data). A clean install has no
`baked-tint` rows until this is re-run. The rule: class `metal` and a texture name matching
`*ecovalve*` (33 rows on the current table; existing tags such as `precursor` are kept,
because a non-empty `tags` column overwrites the row's tags). From `sandover/tools`:

```python
# save as bt.py, then: uv run python bt.py > $SANDOVER_DATA/materials/corrections/2026-09-26-ecovalve-baked-tint.csv
import csv, sys
from sandover.materials.table import csv_path, read_csv
w = csv.writer(sys.stdout, lineterminator="\n")
w.writerow(["combo_id", "tpage", "name", "human_class", "tags", "human_note"])
for r in read_csv(csv_path()):
    if r["class"] == "metal" and "ecovalve" in r["name"]:
        tags = [t for t in r["tags"].split(";") if t and t != "baked-tint"] + ["baked-tint"]
        w.writerow([r["combo_id"], r["tpage"], r["name"], "metal", ";".join(tags),
                    "eco vent dish keeps the baked red (#275)"])
```

Back up `materials.csv` first (as `corrections/materials.before-baked-tint.csv`), then:

```
uv run sandover-materials apply --csv $SANDOVER_DATA/materials/corrections/2026-09-26-ecovalve-baked-tint.csv
```

It should print `applied 33 corrections; skipped 0`. Restoring the backup undoes it.

### Per-texture correction: Jak's nail strips are skin (#264)

The judges split on Jak's two nail-strip textures (`eichar/bam-bignail` 0x003e0001 and
`eichar/bam-multinail` 0x003e0008: organic 0.40 vs metal 0.50), the more confident metal won, and
his thumbs and toes then mirrored the mood environment (blue at night, violet at finalboss). Both
are skin-tan strips, so they become `organic`. This is an explicit two-row correction keyed by
`combo_id`, not a name rule: other actors can have real metal hands or feet (robot enemies), and
no other row is touched. The CSV:

```
combo_id,tpage,name,human_class,tags,human_note
0x003e0001,eichar,bam-bignail,organic,,"Jak's nail strip, dielectric skin (#264)"
0x003e0008,eichar,bam-multinail,organic,,"Jak's nail strip, dielectric skin (#264)"
```

saved as `$SANDOVER_DATA/materials/corrections/2026-09-27-jak-nails-organic.csv`.

Back up `materials.csv` and `pbr-maps/index.json` first (as
`corrections/materials.before-skin-dielectric.csv` and
`corrections/pbr-index.before-skin-dielectric.json`), then apply and re-derive the two maps, since
the engine takes `metallic` from the maps index, not from the class:

```
uv run sandover-materials apply --csv $SANDOVER_DATA/materials/corrections/2026-09-27-jak-nails-organic.csv
uv run sandover-materials derive-maps --only eichar/bam-bignail --only eichar/bam-multinail --no-sheets
```

It should print `applied 2 corrections; skipped 0`, and the index entries then read `organic`,
`metallic 0.0`. Restoring both backups undoes it.

### Per-texture correction: fake light shafts carry `shaft` (#376)

Translucent geometry that fakes a sun ray gets the `shaft` tag, which sets bit 0x10 of the
texture's class byte (`kPbrShaftBit`); TFragment skips those draws while the class shading runs.
The class is unchanged. Like the rows above, the tag is a local correction CSV, so a fresh
`$SANDOVER_DATA` has no `shaft` rows until it is re-applied; the release package ships the local
`materials.csv`, so a packaged install carries whatever the packaging machine's table has. The
CSVs, one per confirmed shaft texture (lighting.md section 9 has the census):

```
combo_id,tpage,name,human_class,tags,human_note
0x03c60000,jungleb-vis-alpha,junb-white,special,shaft,"fake light cone under the plant boss room's ceiling opening; hidden in the remaster look"
```

saved as `$SANDOVER_DATA/materials/corrections/2026-10-09-junb-white-shaft.csv`. Back up
`materials.csv` first (as `corrections/materials.before-shaft-tag.csv`), then:

```
uv run sandover-materials apply --csv $SANDOVER_DATA/materials/corrections/2026-10-09-junb-white-shaft.csv
```

It should print `applied 1 corrections; skipped 0`; restoring the backup undoes it. No PBR map
changes, so no `derive-maps`. Misty's `e-white` (the arena tower beams) is a shaft too but stays
untagged by the maintainer's decision (lighting.md section 9); its retired CSV is
`corrections/superseded-2026-10-09-misty-e-white-shaft.csv`.

### Candidate classification for a review round

Play sessions read the live `materials.csv` and `pbr-maps/`, so a batch of reclassifications is
shown to the maintainer as a round (A today / B candidate / C vanilla) before it is applied. The engine
already takes both paths from the environment (`Materials.cpp` `csv_path`, `maps_root`), and the
rig passes its environment through to gk, so the candidate needs no build and no rig flag:

- `$SANDOVER_MATERIALS`: a full `materials.csv` (the live file with the candidate rows changed).
- `$SANDOVER_PBR_MAPS`: a full maps folder; the class picks the derive recipe, so a class change
  without re-derived maps keeps the old class's normal and roughness.

Build the candidate under `$SANDOVER_DATA/materials/candidates/` (local data): copy the live
`materials.csv` and change only the `class` of the candidate rows (and their `tags`, if the
correction sets any: the engine reads them), keep the matching correction CSV
(`combo_id,tpage,name,human_class,tags,human_note`) next to it, copy `pbr-maps/` to a candidate
folder and re-derive only the changed rows into it. `sandover-materials apply` cannot target a
candidate CSV (it writes only the live table and ignores `$SANDOVER_MATERIALS`), so edit the
candidate by hand or with a short script. The `pbr-maps/` copy is large: keep it under
`$SANDOVER_DATA`, never on a system-drive scratch folder.

```sh
uv run sandover-materials derive-maps --materials $SANDOVER_DATA/materials/candidates/<name>-materials.csv \
    --out $SANDOVER_DATA/materials/candidates/<name>-pbr-maps --only <tpage>/<texture> ... --no-sheets
```

Shoot B with both variables set for that one command, A and C as usual:

```sh
SANDOVER_MATERIALS=$SANDOVER_DATA/materials/candidates/<name>-materials.csv \
SANDOVER_PBR_MAPS=$SANDOVER_DATA/materials/candidates/<name>-pbr-maps \
    uv run sandover-capture from-snapshot <snapshot>.json --bin <staged> --out-dir <round>/B
```

The gk log of B must show `materials: ... from <candidate csv>` and `materials: map index ... from
<candidate maps>`. After the pick, back up the live files and apply the correction CSV and the same
`derive-maps --only` run to them, as above.

### Per-texture correction: snow ground rows

Eight snow ground textures were judged `stone`, so the main snow surfaces took stone's response
and no sun glints. Moving them to `snow` was picked in a review round (shot with the candidate
recipe above). The CSV:

```
combo_id,tpage,name,human_class,tags,human_note
0x02c60001,snow-vis-tfrag,snow,snow,,"snow ground read as stone; snow response picked in a review round"
0x02c60006,snow-vis-tfrag,snow-03,snow,,"snow ground read as stone; snow response picked in a review round"
0x02c60049,snow-vis-tfrag,snow-path,snow,,"snow ground read as stone; snow response picked in a review round"
0x02c60046,snow-vis-tfrag,snow-path-sides,snow,,"snow ground read as stone; snow response picked in a review round"
0x02c6004d,snow-vis-tfrag,snow-clumps,snow,,"snow ground read as stone; snow response picked in a review round"
0x02c6004f,snow-vis-tfrag,snow-clumps-2x1,snow,,"snow ground read as stone; snow response picked in a review round"
0x02c60019,snow-vis-tfrag,snow-treesnow,snow,,"snow ground read as stone; snow response picked in a review round"
0x02c70000,snow-vis-shrub,snow,snow,,"snow ground read as stone; snow response picked in a review round"
```

saved as `$SANDOVER_DATA/materials/corrections/2026-10-01-snow-ground-rows.csv`. Back up
`materials.csv` and `pbr-maps/index.json` first (as `corrections/materials.before-snow-ground.csv`
and `corrections/pbr-index.before-snow-ground.json`), then apply and re-derive the eight maps (the
class picks the derive recipe, so without it the rows keep stone's normal and roughness):

```
uv run sandover-materials apply --csv $SANDOVER_DATA/materials/corrections/2026-10-01-snow-ground-rows.csv
uv run sandover-materials derive-maps --only snow-vis-tfrag/snow --only snow-vis-tfrag/snow-03     --only snow-vis-tfrag/snow-path --only snow-vis-tfrag/snow-path-sides     --only snow-vis-tfrag/snow-clumps --only snow-vis-tfrag/snow-clumps-2x1     --only snow-vis-tfrag/snow-treesnow --only snow-vis-shrub/snow --no-sheets
```

It should print `applied 8 corrections; skipped 0`, and the eight index entries then use the
`snow` recipe. Restoring both backups undoes it. gk reads the table once per process, so the
change shows from the next game start.

## `sandover-review-materials`

```sh
uv run sandover-review-materials --csv $SANDOVER_DATA/materials/spotcheck/spotcheck.csv
uv run sandover-review-materials --csv <same> --all-needs-review
uv run sandover-review-materials --csv <same> --order file        # keep the CSV's own order
uv run sandover-review-materials --csv <same> --viewer-port 8765  # sandover-viewer's --port
```

A local Gradio app at `127.0.0.1:7864` (hardened: analytics and the version
check off, no external requests) that classifies one texture at a time instead of reading contact
sheets and editing the CSV by hand. Shows the texture at 512 px or larger (nearest-neighbour
upscale for anything smaller; the caption states the true size), alpha doubled from the PS2
convention (128 = opaque) over a checkerboard, plus a second alpha-as-grey thumbnail, next to
the judges' class, confidence and whether the two passes agreed -- this is a correction task,
so the judgement is shown, not hidden. Buttons assign a class, one per class in
`sandover/presets/materials.toml`: keys `1`-`6` for `stone`/`wood`/`metal`/`foliage`/
`organic`/`special`, `9`/`0` for `glass`/`cloth`, `I`/`L` for `snow`/`lava` (`7`/`8` are the
tag keys below). Letter keys are case-insensitive. `Enter` accepts the judges' class as is;
`S` skips to the next texture without recording anything; `B` steps back. Every button/key
press writes straight to the CSV, so closing the tab loses nothing.

**Queue order** (`--order`, default `confidence`): least confident first, and at equal
confidence a judge disagreement (or `single`/`none`) sorts before an `agree`, so the rows
most worth a human's time surface first. `--order file` keeps the CSV's own row order
(the stratified sample order `spotcheck` wrote, or whatever order a hand-edited CSV has).

**Tags** (keys `7`/`8`, or their buttons): toggle `fur` or `precursor` on the current row,
independent of class -- shown as "tags: ..." beside the class buttons. Stored
semicolon-separated in the `tags` column, carried through `apply` into `materials.csv` and
pushed to FiftyOne as a `material_tags` list field by `sandover-materials load` (named
`material_tags`, not `tags`, to not collide with FiftyOne's own per-sample `tags` list that
`apply --fiftyone` already reads for `class:<name>`/`ok`).

**Context for the judges**: the note field (label "Context for the judges", above the class
buttons) is guidance, not just a note -- it feeds `rejudge --with-notes` (below). A class is
optional here: **Save context only (no class)** writes the note and moves to the next row
without touching `human_class`, so a texture can carry guidance before anyone commits to a
class for it.

`--all-needs-review` appends the FiftyOne `needs-review` saved view (disagreements and low
confidence, from `sandover-materials load`) to the queue after the 100 spot-check rows,
skipping anything already in the CSV by `combo_id`.

The "Finish" button runs the same `apply --csv` path described above (folds `human_class`
into `materials.csv`, marking `source=human`, `reviewed=True`), reloads the FiftyOne dataset
and saved view, and prints a per-class table of how often the maintainer agreed with the judges.
Run `sandover-materials apply` by hand instead if the page was closed before pressing it --
the CSV already has every answer.

Queue/CSV logic (`apply_action`, `merge_needs_review`, `agreement_by_class`, `toggle_tags`,
`sort_queue_by_confidence`, image sizing) lives in `sandover.materials.review` and is
unit-tested without a browser (`tests/test_review_materials.py`).

### Context panel: the 3D viewer, in place

A collapsible "Context (3D viewer)" panel embeds `sandover-viewer` (`kb/howto/viewer.md`) in
an iframe, framed on the current texture: `GET /api/models-using/<name>` on the viewer
(`--viewer-port`, default `8765`; the viewer must already be running -- this reviewer does
not launch it) picks the first model, and the panel loads
`http://127.0.0.1:<viewer-port>/?model=<id>&highlight=<name>`, which highlights every
primitive using that texture with a pulsing tint and fits the camera to just those
primitives. A short list of any other models using the texture appears as links under the
iframe; clicking one swaps the iframe's `src` in place (no page reload, no server round
trip). "no model uses this texture in the rip" appears when the list is empty; if the viewer
isn't reachable at all, the panel says so instead of showing a blank frame. `V` toggles the
panel.

**Keyboard limitation**: once focus is inside the iframe (e.g. after clicking to orbit the
camera), the reviewer's own shortcuts (`1`-`8`, `S`, `B`, `Enter`, `V`) stop working --
keydown events fired inside an iframe's document never reach the parent page's listener,
regardless of origin. Click back into the reviewer page (e.g. the note field or a button) to
get keyboard shortcuts back.

## Browse tab: emissive class and the paged grid

A second tab next to Review, for looking at a whole class at once instead of one texture at
a time. Pick a class from the dropdown -- the ten `materials.toml` classes plus three derived
from emissive data -- and page through a thumbnail grid (alpha x2 over checkerboard, name and
confidence, `sandover.materials.browse.render`/`paginate`, 60 per page). Click a tile to show
it in the texture panel under the grid (below, "The texture panel"), where `N`/`E`/`U` flag it
and its class, tags and note are edited in place. "Open in Review queue" does what a tile click
used to: jumps the Review tab's queue to that texture (`jump_to_texture`: reuses its row if
already queued, else appends one via the same `merge_needs_review` the FiftyOne needs-review
queue uses) and switches tabs.

The three derived classes (`sandover.materials.browse.class_label` gives the dropdown and page
label their longer names below; the identifiers in code stay short):

- **`emissive`**, shown as "emissive (ships)": textures that ship emissive
  (`sandover.materials.browse.ships_emissive`). A human override always wins
  (`sandover.materials.overrides`, written by the viewer's inspect panel -- `kb/howto/viewer.md`
  -- or the in-game inspect tool); otherwise the emitter score (`emitter-score.json`, below)
  says the texture is an emitter -- score at or above its threshold, a ladder anchor, or a human
  ladder emitter (`emissive_strengths.is_emitter`). A texture with no score entry (no baked E,
  so `export-sidecar` never scales it) falls back to the raw E table exceeding
  `sandover.materials.emissive_table.EMISSIVE_THRESHOLD` (0.1, the sidecar report's own "clearly
  emissive" histogram split) -- the same rule this class used before the emitter score existed.
- **`receiver`**, shown as "receiver (E scaled to 0 at export)": raw E over `EMISSIVE_THRESHOLD`
  -- what `emissive` alone used to mean -- but scored below the emitter score's threshold: the
  lit-by-neighbours set `export-sidecar` scales to 0 at export (`receiver_scale`, issue #171,
  see "Feeding the ladder back" below). Kept browsable and correctable with `N`/`E` in case the
  score got one wrong. Excludes overridden textures, which already ship one way or the other.
- **`not-emissive-override`**: a human override says `not-emissive` -- its own bucket, so a
  batch of "table said yes, human said no" cases (or over-eager markings) can be reviewed
  together.

The per-texture panel's "why" line (below, "The texture panel") already shows the emitter score
and its verdict (`emitter` / `lit receiver`) for a scored texture; the class label above is
about which bucket the Browse grid puts it in, not a second judgement.

### Precomputing the E table

```sh
uv run sandover-materials emissive-table                 # all 21 levels
uv run sandover-materials emissive-table beach jungle     # just these
```

Walks every level's `<level>-background.glb` (materials, per-primitive draw indices) against
its `.unbake` sidecar (`$SANDOVER_DATA/lighting/<level>.unbake`), and for every texture takes
the max E over the vertices any tfrag/tie primitive using it actually draws. Shrub is
skipped: the sidecar's vertex-order guarantee only covers tfrag/tie (see its module
docstring). Writes `$SANDOVER_DATA/materials/emissive-table.json` (texture name -> max E);
the Browse tab loads this once per page load/reload, not per texture, so browsing is
instant. Re-run it after re-exporting sidecars (`sandover-unbake export-sidecar`) or after
a rip that changes which primitive draws which texture.

### `emissive-overrides.csv`

`$SANDOVER_DATA/materials/emissive-overrides.csv`: `combo_id, tpage, name, level, draw_id,
verdict (emissive|not-emissive|empty), note, timestamp`. Append-only, like `spotcheck.csv` --
the last row for a key (`combo_id`, or `name` when `combo_id` is blank) wins
(`sandover.materials.overrides.latest_verdicts`); an empty `verdict` clears a prior one. Three
writers share this file and its columns exactly: the viewer's inspect panel (`POST
/api/mark`), the in-game inspect tool (`p2/inspect-tool`), and, indirectly, anyone hand-editing
the CSV. Reading it never needs write access -- the Browse tab and `class_members` only read.

## Emissive review

Output for the PBR phase: a per-texture **emissive map** (texture space, 8-bit) and a
per-texture **emissive strength** relative to a torch. Three pieces:
`sandover-materials emissive-maps` bakes the maps and default strengths, the reviewer's
**Emissive** tab places each texture on a strength ladder, and `sandover-unbake
export-sidecar` feeds the ladder back into the sidecar's per-vertex E.

### `sandover-materials emissive-maps`

```sh
uv run sandover-materials emissive-maps --lighting-dir <sidecar dir>   # all 21 levels, ~20 min
uv run sandover-materials emissive-maps --reindex                      # seconds, see below
```

For every texture drawn by a level's tfrag/tie trees, the sidecar's per-vertex E is
interpolated across each triangle through the mesh UVs into texture space
(`sandover.materials.emissive_maps.rasterize`), max over every draw of the texture in every
level. UVs outside [0, 1) wrap, so a tiled floor writes all its repeats into one map; a
triangle smaller than a texel still marks the texel under each corner. Shrub is skipped for
the same reason as the E table (no vertex-order guarantee).

Writes `$SANDOVER_DATA/materials/emissive-maps/<tpage>/<name>.png` (greyscale, E x 255, the
texture's own size) and `index.json`: per texture `tpage`, `width`, `height`, `max`, `mean`
and `peak` (95th percentile) over covered texels, `coverage` (fraction of texels with
E >= 0.1, the engine bloom ramp's start and the Browse tab's threshold), `role` (the mood slot
role that contributed most E: flames, light, lava, fade-lights, lightning, caustics, fade,
night, static), `levels`, `default_strength` and `source`:

- `bake`: texture with coverage > 0; default = `peak / torch peak`.
- `luminance`: a human `emissive` override (`emissive-overrides.csv`) with no bake coverage,
  or a lava fallback anchor (below). The map is a smoothstep of the texel luma (0.5..0.9);
  default = 0.5 x the texture's bright-texel luma (95th percentile over opaque texels) / the
  torch's, the 0.5 because an unbaked texture is less certain to be a light. Anchors skip
  that discount.

`--reindex` keeps the rasterized maps and redoes only the luminance entries, anchors and
defaults; run it after marking new overrides. `--textures` (default
`$SANDOVER_ROOT/decompiler_out/jak1/textures`) resolves each stem to its tpage, preferring the
tpage named after a level the texture is drawn in.

**Anchors** (`ANCHOR_PATTERNS`): the fixed marks on the ladder. Each is the median-peak texture
among those whose name matches the role's pattern, whose dominant E role fits and whose
coverage is at least 0.25 -- the median, because E saturates at 1.0 across whole levels
(firecanyon's lava slots light every wall), so the brightest candidate is as likely a lit
wall as a lamp.

| Role | Name pattern | E roles | Found on the current rip | Default |
|---|---|---|---|---|
| torch (reference, 1.0) | `torch\|flame\|fire` | flames | `maincave-vis-tfrag/cv-torch-01` | 1.000 |
| lamp glass | `lamp\|lantern\|light\|bulb\|glass` | night, static, light, flames | `misty-vis-tfrag/cmn-light` | 0.801 |
| eco | `eco-valve-inside\|blueeco\|eco-glow` | any | `sunkenb-vis-tfrag/sun-precursor-blueeco-flash` | 0.323 |
| crystal | `crystal` | any | `village3-vis-tfrag/vil3-giant-crystal-nt-02` | 0.435 |
| lava | `lava\|magma` (lava E role); none qualify, so the luminance fallback `lava-(flow-01\|ground-hot\|floor-deadly)` | -- | `lavatube-vis-tfrag/lava-floor-deadly` (luminance) | 0.777 |

The lava surface itself is not a lit tfrag/tie surface in any sidecar (lavatube's lava-named
textures are lit by its `light` slot, firecanyon's by its lava slots), hence the fallback.

Current rip: 904 textures drawn by tfrag/tie, 668 with bake coverage, plus 3 luminance
(the lava fallback textures; no emissive overrides yet), 671 maps in all. Caveat: E is light
the bake put on a vertex from anything but sun and ambient, so a wall next to a lamp gets E
too, and a tiled texture takes the max over every repeat -- 503 of the 671 maps cover the
whole texture and 185 defaults sit at 1.0 (within 1%). The ladder is where that gets sorted out.
`sandover-materials emitter-score` (below) separates the two.

### `sandover-materials emitter-score`

```
uv run sandover-materials emitter-score --validate --sheets   # ~40 s, all 671 maps
uv run sandover-materials emitter-score --rescan              # redo the geometry scan
```

Scores every texture of `emissive-maps/index.json` with a probability of being a real emitter
(lamp, lava, crystal, screen) rather than a lit receiver (a wall, log or stone the bake lit with
a lamp's light). Components, all kept in the output so the reviewer can show why:

| Component | Emitter | Receiver |
|---|---|---|
| (a) `correlation`: E map vs albedo luma, over opaque texels | positive | ~0 (even E) |
| (b) `coverage`: texels with E >= 0.1 | any | near 1 |
| (c) `gradient`: mean `(max - min) / max` of corner E over the lit triangles | low (evenly lit) | high (falloff) |
| (d) `consistency`: lit triangles / all triangles drawing the texture, every level | near 1 | low (only near lamps) |
| (e) `slot`: prior of the dominant E source (`role`) | flames, lava, luminance | lightning, caustics, fade |
| (f) `brightness`: E-weighted mean albedo luma | bright | dim |

`score = sigmoid(bias + sum(weight * component))`; weights, slot priors, the threshold (0.5)
and the receiver default live in `sandover/presets/emissive.toml` `[emitter_score]`. (c) and
(d) come from a tfrag/tie scan of the level glbs and sidecars, cached in
`$SANDOVER_DATA/materials/emitter-score/draws.json`.

Output `$SANDOVER_DATA/materials/emitter-score.json`: `threshold`, `params` and per texture
`score`, `emitter`, `components`, `tpage`, `role`, `class` (materials.csv), `human`
(`emitter`/`receiver` from an override or a human ladder strength), `anchor` and
`ladder_default` (the bake default for emitters, 1/64 for receivers). Anchors and human
emitters are always emitters. For the Emissive tab,
`sandover.materials.emissive_strengths.scored_queue(queue, scores)` reorders `build_queue`'s
rows (emitters first by score, unscored next, receivers last) and swaps receiver defaults to
the rail bottom, keeping the old one as `bake_default`.

`--validate` prints precision/recall against the maintainer verdicts and a name-based weak label set
(`weak_label`: torch, lamp, lava... vs rock, wood, metal...; report only, never scored), lowers
the threshold if a human emitter scores below it, and prints receivers per material class.
`--sheets` writes `emitter-score/sheets/top-{emitters,receivers}.png` (albedo | E map | score
and components). Results on the current rip:
`kb/findings/2026-09-24-emitter-score.md`.

### Emissive tab (`sandover-review-materials`)

Third tab of the reviewer. Queue: every texture in `index.json` with coverage > 0 (or a
luminance source), plus human `emissive` overrides not yet in the index (source `override`,
at the rail bottom), minus `not-emissive` overrides. When `emitter-score.json` exists the
order comes from `emissive_strengths.scored_queue` (emitters first by score, unscored next,
receivers last at the rail bottom); otherwise strongest default first. It opens on the first
texture with no row yet in `emissive-strengths.csv`; "Go to texture" jumps to one by name (and
says so when it is not on the ladder).

- **Left**: the texture panel (below).
- **Right**: the 3D viewer (`--viewer-port`) on a background chunk from a level the texture's
  glow was baked in, with `?bloom=1&emissive=<stem>&strength=<f>` (`kb/howto/viewer.md`,
  "Bloom preview"). Strength changes are posted to the iframe (`postMessage`), not reloaded.
- **Bottom, the ladder**: a log rail from 1/64 to 16 x torch. Blue marks are the anchors
  (torch gold at 1.0), the dashed line is the texture's default, the orange marker is its
  current strength, and up to three textures just below and three just above it show as
  thumbnails (spread out so equal strengths don't stack, each with a tick at its true place).

Keys (listed on the page, from `emissive_review.TAB_KEYS`): `Up`/`Down` nudge a quarter octave,
drag the marker (or click the rail) to set, `R` reset to the default, `Enter` accept and move
on, `S` skip, `B` back, `N` not emissive, `E` is emissive, `U` undo the last N/E. Every change
appends a row immediately. Accepting a scored receiver at its rail-bottom default writes a
`human` row, so `export-sidecar` applies it (an untouched `default` row is ignored there). The
Review tab's keys are ignored while another tab is showing, and vice versa. Clicking into the
3D iframe takes keyboard focus there (same limitation as the Context panel); click back on the
page. Entering the tab re-reads the overrides, so a flag set in Browse or the viewer shows.

### The texture panel (Browse and Emissive tabs)

`sandover.materials.material_panel`: one texture, everything needed to judge and edit it.

- **Images**: the albedo; the albedo with every texel the bake counts as emissive (E >= 0.1)
  tinted orange (checkbox to hide it, opacity slider); the emissive mask alone (white = E 1.0).
  When `emissive-maps/<tpage>/<name>.png` does not exist the panel says so and why.
- **Why**: EMISSIVE / NOT EMISSIVE and the reason: a human override, or the bake with its E
  source -- a light-source effect slot (flames, light, lava, fade-lights), an outside-light slot
  (lightning, caustics, fade: usually a lit surface), `night` (brighter at night than moonlight
  explains) or `static` (lit at every hour). Then E max and coverage, the emitter score with its
  components when `emitter-score.json` exists, and the ladder: strength, step from the torch in
  quarter octaves, who placed it, queue position.
- **Where it appears**: the levels its E was baked in; the viewer models that draw it (links
  open the viewer with that model loaded); night and noon rig captures of the first such level
  (newest golden per set, then `shots/`), linking to the full image
  (`--hub-port`, default 7870). Captures are matched by level, so the texture may be out of
  frame; with none, use a model link.
- **Edit**: class (the judges' class is in the label), tags and a note, saved with "Save class /
  tags / note" to the review queue CSV like the Review tab's keys (the texture is queued first
  if needed). The note also goes on N/E override rows.
- **Flags**: `N` appends a `not-emissive` row to `emissive-overrides.csv`, `E` an `emissive`
  row, through `overrides.append_override` (the writer the viewer's `POST /api/mark` uses, so
  one format); `U` appends the key's previous verdict (empty = back to the bake). The undo
  stack lasts for the page session; after a reload, flip a verdict with N/E instead. `E` on a
  texture the bake already makes emissive writes nothing: an `emissive` override forces E to 1
  on every vertex at export, so it is for lights the bake missed. A `not-emissive` texture
  leaves the Browse `emissive` class and the ladder at once and shows under
  `not-emissive-override`.

### `emissive-strengths.csv`

`$SANDOVER_DATA/materials/emissive-strengths.csv`: `combo_id, tpage, name, strength, source
(default|human), note, timestamp`. Append-only; the last row per key (`combo_id`, or `name`
when blank) wins (`sandover.materials.emissive_strengths.latest_strengths`). `human` = moved on
the ladder or accepted off its default; `default` = reset, or accepted as is. Ladder math and
CSV I/O live in `sandover.materials.emissive_strengths`, the queue and actions in
`sandover.materials.emissive_review` (`tests/test_emissive_strengths.py`).

### Feeding the ladder back: `sandover-unbake export-sidecar`

`export-sidecar` reads `emissive-strengths.csv` and `emissive-maps/index.json` when both
exist (`--emissive-strengths`, `--emissive-maps-index`; `--no-emissive-strengths` for raw E).
For each texture whose last row is `human`, E on the vertices its primitives draw is
multiplied by `strength / default_strength` and clamped to [0, 1]
(`sidecar.load_e_factors`, `sidecar.vertex_scale`). A vertex shared by several textures takes
the largest factor, an unlisted texture counting as 1, so turning one texture down never
dims a neighbour's vertices. The per-level report lists the scaled textures under
`emissive.scaled_textures`. No engine change: the engine reads E as before, and bloom and
the SSAO fade follow. Limits: E is 8-bit and capped at 1.0, so raising a texture that already
sits at E = 1 changes nothing in the sidecar (the strength still reaches the PBR phase through
the CSV); luminance-sourced textures have no E to scale.

**Receivers.** `export-sidecar` also reads `emitter-score.json` when present
(`--emitter-score`; `--no-receiver-scale` to skip): every receiver (score below the threshold,
not a ladder anchor, not a human emitter) has its E multiplied by `receiver_scale` from
`sandover/presets/emissive.toml` `[emitter_score.export]` (default 0: receivers stop glowing;
`--receiver-scale` overrides it). Textures with a human ladder row or an override are left to
it: the maintainer factor replaces the receiver scale, and overrides are applied after scaling.
The vertex rule is the same as above (largest factor wins), so a lamp vertex shared with a
wall keeps its E.

The export also writes the unscaled, unforced sidecars to `$SANDOVER_DATA/lighting/raw/`.
`emissive-table`, `emissive-maps` and `emitter-score` scan that directory when it exists
(`sidecar.scan_dir`), so a scaled receiver does not vanish from the maps and lose its score
on the next run.

### Actor emissive table: particles and merc models (`sandover-materials actor-emissive`)

The sidecar only carries E for background geometry (tfrag, tie). Eco collectables, vents, power
cells, precursor orbs, fire and eco beams are particles (sprite renderer) or merc models, so the
bake never flags them (issue #173). They get E from the same override and ladder rows as any
other texture, and the engine reads a small table exported from those rows:

```sh
uv run sandover-materials actor-emissive --seed      # append rows for [actor_emissive], export
uv run sandover-materials actor-emissive             # export only (after marking E in the reviewer)
```

- `--seed` appends an `emissive` override (`emissive-overrides.csv`, note `actor emissive seed
  (#173)`) and a ladder row (`emissive-strengths.csv`, source `default`) for each texture listed
  in `sandover/presets/emissive.toml` `[actor_emissive]`. A human `not-emissive` verdict or a
  human ladder row is never overwritten; rows already current are not appended again.
- The ladder step comes from the brightness rule (`actor_emissive.ladder_step`): the highest
  quarter octave whose bloom weight keeps the element's baked peak under `cap` after bloom
  (`kb/findings/2026-09-24-eco-brightness.md`). `peak` per texture is measured from vanilla
  night captures (`sandover/shots/eco-emitters.yaml`). A texture whose element already clips in
  vanilla has `peak = 1.0` and an explicit `step` (the rule has no safe step for it). `octaves`
  raises every seeded step by that many octaves: 1 is the shipped look (rule + 1 octave, eco-001
  pick D), which relies on bloom's hue clip (`ssao.md` Bloom,
  `kb/findings/2026-09-25-eco-hue-clip.md`).
- Export: every texture whose latest verdict is `emissive` and has a `combo_id` goes to
  `$SANDOVER_DATA/materials/emissive-actors.csv` (`combo_id, name, e, min_chroma, lights`), with `e` =
  ladder strength x the torch's peak E, capped at 1 (no ladder row: the torch, 1.0). So an `E`
  pressed in the reviewer on a merc texture (a creature's eye) takes effect after an export.
- `min_chroma` (sprites only): particle textures are shared across effects (`bigpuff` is eco,
  fire, dust and smoke), so a listed sprite writes E only where its vertex colour is at least
  that saturated; grey smoke on the same texture stays out.
- `lights` (0..1, default 1): share of the texture's E that feeds the screen-space tile lights
  (`kb/howto/ssao.md`, "Per-texture light share"). Bloom and the SSAO fade always take the full
  E. The precursor orb textures (`egg-ndimadman`, `hud-egg`) are 0: they glow, they light nothing.

Engine side (`game/graphics/opengl_renderer/EmissiveTable.{h,cpp}`, Jak 1): consulted only while
the emissive mask is live (bloom, or the SSAO mask). Sprite3 looks the bucket's texture up; a
listed world sprite writes `e x coverage` (coverage saturates where the sprite adds a quarter of
white) into the mask with a max blend, so stacked additive particles no longer sum past the
ramp. Merc draws of a listed texture (merc2 program, not the envmap pass) write `e`. A missing or
empty file changes nothing. `$SANDOVER_EMISSIVE_ACTORS` points the engine at another file (test
runs); Renderer Debug -> Bloom shows the row count and re-reads the file.

## `sandover-materials rejudge`

```sh
uv run sandover-materials rejudge --with-notes
uv run sandover-materials rejudge --with-notes --limit 40
```

Re-runs the vision judge on the current needs-review set (disagreement or confidence below
`REVIEW_BELOW`, excluding rows a human already reviewed), as a third, independent opinion.
Two-phase, run twice:

1. **Prepare** (no pending batch): picks the candidates (least confident first, same
   ordering as the reviewer's default), renders them as their own numbered sheet set and
   manifest under `$SANDOVER_DATA/materials/judgements/rejudge/<timestamp>/`, and writes
   `prompt.txt` there -- give that prompt to the vision judge the same way as a normal pass
   (`kb/howto/materials.md` "Re-run from scratch"), except its output path is
   `judgement.json` inside that same directory. `--with-notes` prepends a guidance block
   built from every non-empty `human_note` recorded so far in `spotcheck.csv`:
   `Human guidance from the spot-check:` followed by one `- <tpage>/<name>: <note>` line
   per note (not just notes on rows in this particular batch -- it is general guidance for
   the judge, e.g. patterns like "texture names in this tpage are more reliable than the
   pixels here"). `--limit N` caps how many candidates go into the batch.
2. **Merge** (a pending batch's `judgement.json` now exists): folds the new classes and
   confidences into `materials.csv` for rows still unreviewed, writes `rejudge: <note>` into
   `note`, and prints how many rows' class actually changed. This does **not** mark rows
   `reviewed` -- a rejudge is a third opinion, not a human sign-off; run the reviewer
   normally (or `apply --fiftyone`) to close a row out.

Running `rejudge` again after a prepare-but-not-yet-judged batch reports that it's still
awaiting judgement instead of starting a second one. Pure logic (`rejudge_candidates`,
`guidance_lines`/`guidance_block`, `apply_rejudge`'s diff count) lives in
`sandover.materials.table` and `judge.rejudge_prompt`'s formatting in
`sandover.materials.judge`, both unit-tested (`tests/test_materials.py`).

## PBR maps

`sandover-materials derive-maps` derives a tangent-space normal map, a roughness map and a
metallic scalar for every texture in `materials.csv` from that texture's own albedo, with a
filter recipe chosen by its class. It is algorithmic, not generative: no model, no new detail,
only what the painted albedo already carries, which keeps the stylized look.

```sh
cd sandover/tools
uv run sandover-materials derive-maps                     # all 4002, about a minute
uv run sandover-materials derive-maps --class wood        # one class (or a tag: precursor)
uv run sandover-materials derive-maps --only village1-vis-tfrag/vil1-woodplank --height
uv run sandover-materials derive-maps --sheets-only       # redraw the contact sheets
uv run sandover-materials derive-maps --source vanilla    # derive from the rip, not the pack
```

Albedo source: the alpha-normalized 4x pack (`$SANDOVER_DATA/assets/esrgan-v1.0.1-alpha-normalized`,
written by `sandover-alpha-normalize`) when it has the texture, else the vanilla rip. Recipe
distances are in vanilla texels and scale with the source, so a pack texture and its vanilla
original get the same look (`--source vanilla` derives from the rip instead; the pack gives
crisper edges, see `kb/findings/2026-09-24-pbr-maps.md`). Worker count is logical threads - 2.

Output under `$SANDOVER_DATA/materials/pbr-maps/` (the emissive-maps layout):

```
pbr-maps/
  <tpage>/<name>.normal.png   # RGB, glTF convention (+X right, +Y up), same size as the albedo
  <tpage>/<name>.rough.png    # greyscale roughness
  <tpage>/<name>.height.png   # only with --height; band-passed height, 0.5 = mean
  <tpage>/<name>.parallax.png # masonry only (see "Parallax"); height 0..1, 1 = top
  index.json                  # "<tpage>/<name>" -> class, tags, recipe, source, metallic,
                              # scale, slope p50/p95, rough mean, coverage, flat; recipe_version
  sheets/<class>.png          # albedo | normal | roughness for 12 textures per class and tag
```

`--class` and `--only` update their entries in the existing index. The per-class sheets live under
`sheets/`.

### Per texture

1. Luminance (Rec.601) of the albedo. Texels under the engine alpha test (doubled PS2 alpha
   < 0.128) are masked: they are filled by normalized convolution of the opaque texels before
   filtering, so cut-out edges do not ring, and get a flat normal and the base roughness.
2. Height = G(band_lo) - G(band_hi) of the luminance (difference of Gaussians), plus any
   weighted `extra_bands` (a small multi-scale pyramid, as Materialize does), negated when
   `invert`. Everything broader than the widest band (baked shading and AO gradients) drops
   out, which is the delighting step; for `grain > 0` blurred along the structure tensor's dominant orientation when
   its coherence clears `grain_coherence`. All filters run through the FFT, so every texture
   is treated as tiling.
3. Normal = Sobel slope of the height with wrapped borders, times `normal_strength`,
   soft-clipped by tanh at `max_slope`.
4. Roughness = `rough_base` + `rough_contrast` x local contrast of the height +
   `rough_luma` x (luminance - mean), clipped to [`rough_min`, `rough_max`].
5. Metallic = the recipe's scalar, in the index only.

Textures smaller than `min_size` vanilla texels, envmap swatches, and the `flat` classes get a
flat normal.

### Parallax

Masonry textures also get a height for a fragment-shader parallax offset (no tessellation, no
geometry change). A texture qualifies when its class is in `[parallax] classes` of
`pbr-recipes.toml` (stone) and its name contains one of `cues` (brick, stone, block, wall, tile,
cobble), or when its `materials.csv` row has `parallax = yes`; `parallax = no` opts it out.
Textures with under `min_coverage` opaque texels, flat and tiny ones never qualify. The height
is the recipe's band-passed height stretched to 0..1 between its 1st and 99th percentiles.
`derive-maps --parallax` regenerates only the qualifying textures.

The engine packs the height into the existing RGBA8 map (normal xy in rg, z rebuilt in the
shader, roughness in b, height in a): no extra VRAM and no extra bind. tfrag, tie and etie
draws with a packed map follow the view ray to the height field in up to 3 taps, with the depth
`[parallax] depth_m` of `pbr.toml` (world metres, default 5 cm, clamped to 8 cm; turned into
UV by the surface gradients of the texture coordinates, so it holds at any texel density), and
fade the offset out at grazing angles and with distance. The distance fade `fade_m` is for a
texture repeating every `fade_ref_m` metres and scales with the texture's own metres per
repeat (x0.25 to x4), so coarse textures keep their relief further out.
Masonry that is not class stone (the precursor stonehenge floor is class metal) takes
`parallax = yes` in `materials.csv`. The alpha stays unshifted, so alpha tests and silhouettes do
not move. `(pc-set-pbr-param 22 0/1)` toggles it; 23 sets the depth in millionths of a metre.
The surface frame (normal, and the world gradients of u and v) comes from a per-vertex basis
built at level load for each tfrag/tie tree that has a sidecar (`unbake_px_texture`), so the
offset is continuous across triangle edges; a per-triangle frame from screen derivatives made
it step at every edge of curved rock (#274). `[parallax] basis = 0` or `(pc-set-pbr-param 24 0)`
switches back to the per-triangle frame for comparison; trees without a sidecar always use it.

### Recipes: `sandover/presets/pbr-recipes.toml`

A texture takes `[defaults]`, then `[class.<class>]`, then `[tag.<tag>]` for each tag it
carries (so `precursor` overrides `metal`). Unknown keys are rejected. Tuning is an edit and a
rerun; bump `version` when a change alters the output.

| Class | Height | Normal | Roughness | Metallic |
|---|---|---|---|---|
| stone | dark = low (engraved lines, cracks), band 0.5-5 texels + 5-12 at 0.3 | strong | high, 0.86 base | 0 |
| wood | band 0.4-3, blurred 3 texels along the grain | medium-strong | 0.72 | 0 |
| metal | inverted high-pass 0.35-1.5 (bright scratches = grooves) | weak, clipped | low, 0.32, bright = glossier | 1 |
| precursor (tag) | like stone, band 0.5-5 + 5-10 at 0.25 | medium | medium-low, 0.45 | 1 |
| foliage | soft band 0.8-4 (veins) | soft | 0.6 | 0 |
| organic | fine band 0.35-2 | soft | 0.62 | 0 |
| cloth | fine band 0.35-2 (weave) | soft | high, 0.9 | 0 |
| snow | very fine band 0.35-1.2 (sparkle) | soft | 0.55 | 0 |
| glass, special, lava | flat | flat | 0.06 / 1.0 / 0.9 | 0 |

Precursor maps = bronze recipe on all 301 precursor-tagged rows, 36 of them (envmap-masked alpha, circuitpattern family) derived as opaque; chosen 2026-10-09 (the engraved parallax variants lost).

The viewer previews the maps: `kb/howto/viewer.md` "Derived maps".

## Engine: class shading spike

The engine reads `materials.csv` once per process (`$SANDOVER_MATERIALS`, else
`$SANDOVER_DATA/materials/materials.csv`, else `<project>/sandover/data/materials/materials.csv`)
and maps each level texture's `combo_id` to a class byte at load (`Materials.cpp`); a missing file
or unknown id is `special`. `metal` rows tagged `precursor` get their own bronze class. Off by
default. From the rig:

```sh
uv run sandover-capture run ../shots/pbr-classes.yaml --out-dir $SANDOVER_DATA/shots/pbr/s10 \
    --goal "(define-extern pc-set-pbr-param (function int int none))" \
    --goal "(pc-set-pbr-param 0 1)" --goal "(pc-set-pbr-param 1 1000000)"
```

`pc-set-pbr-param` indices (values in millionths): 0 enable, 1 strength, 2 detail scale
(the normal-map strength with per-texture maps), 3 highlight scale, 4 view (0 shaded, 1 class
colours, 2 detail normal, 3 highlight only, 4 sun visibility in red and key-light weight in
green / merc view-vector check), 5 characters on/off, 6 merc view sign, 7 backgrounds on/off,
8 key-light scale where the sun share is ~0 (default 0.5), 9 detail source (0 procedural, the
spike's look; 1 per-texture maps, the default). Per-class parameters are in the ImGui
"Materials" node. Findings: `kb/findings/2026-09-24-pbr-spike.md`.

### Per-texture maps and the preset

With source 1 every shaded draw (tfrag, TIE, envmapped-TIE base, shrub, merc, generic) binds
its texture's derived map on unit 17 next to the albedo: one RGBA8 texture per texture, rgb
the tangent-space normal from `<tpage>/<name>.normal.png`, a the roughness from
`<name>.rough.png`, looked up by the same `<tpage>/<name>` key as the texture pack. Root:
`$SANDOVER_PBR_MAPS`, else `$SANDOVER_DATA/materials/pbr-maps`; metallic and the `flat` flag
come from its `index.json`. Flat, missing and anim-slot/eye textures bind a 1x1 flat default
and keep the class gloss. The map normal perturbs the sidecar smooth normal (else the face
normal) through a cotangent frame from screen derivatives of position and UV, so mirrored
UV islands need no tangents. Roughness sets the Blinn exponent (`2 / r^2 + 2`) and nudges the
highlight strength; metallic makes the highlight follow the albedo hue. The class still picks
shading on/off, the band shape, tint (precursor bronze) and strength. Source 0 binds the
procedural array on unit 16 instead and never touches unit 17.

Maps are decoded on the loader thread when a level loads with the toggle on, else on the
first shaded draw (logged: `materials: <level> maps ... MB, ms`). The ImGui node shows the live
map count and VRAM.

`sandover/presets/pbr.toml` holds the global defaults and the per-class values (`normal` is
the map strength, default subtle: 0.4-0.6). The engine reads it at start-up
(`$SANDOVER_PBR_PRESET` overrides the path) and on "Reload preset"; values sent with
`pc-set-pbr-param` afterwards win. It never turns the toggle on. `[maps] max_size` caps the
map size at load (0 = as derived).

### Material response and looks

On the shaded paths the class shading is a material-response model (metallic workflow, a
per-frame mood environment reflected along the reflection vector, a GGX sun and key lobe,
hemisphere relief of the detail normal; `kb/findings/2026-09-25-material-response.md`).
Its knobs are `[global]` keys (`metallic`, `env`, `fresnel`, `ambient`, `roughness_contrast`,
`band`, `lobe`, `specular`, `metal_darken`) and per-class keys (`env`, `rough_bias`, `lobe`,
`sheen`). `[look.<name>]` and `[look.<name>.class.<class>]` sections override them for one look
(`faithful`, `bold`, `cinematic`); `[global] look` picks the start-up look and
`(pc-set-pbr-param 11 n)` reloads the preset with look n (0 base, 1 faithful, 2 bold,
3 cinematic; values sent after it win). Params 12-19 and 21 set the knobs directly, 20 flips the
merc world-up sign.

Metal hue (`[global] metal_hue`, `(pc-set-pbr-param 25 n)`, 0..1, default 1): with 1 a metal
reflects the baked light's colour, not only its luminance (every metal behaves like `baked-tint`),
its mood environment is desaturated to its luminance so the mood hue is not counted a second
time, and F0 takes half the class tint (the precursor bronze 1, 0.72, 0.42 acts as about 1, 0.86,
0.71; the texture is already bronze). Under neutral light a metal keeps its texture's colour;
under Sunken's blue bake the precursor walls read blue-teal like vanilla instead of brown.
`(pc-set-pbr-param 25 0)` restores the #284 response for A/B; values in between blend. Views: `(pc-set-pbr-param 4 n)` with 3 reflection and lobes, 5 roughness (r)
and metallic (g), 6 the mood hemisphere.

Class response rules (one rule per class, physical parameters in `[class.<class>]`, each with a
global switch that scales it; all three on in `pbr.toml`, off when the material shading is off):

- Backlit leaves (`transmit`, `transmit_power`; foliage 1.0 and 2): the sun through a thin leaf
  seen from behind, `transmit x albedo^2 x sun colour x sun share x max(-N.L, 0) x
  max(-V.L, 0)^transmit_power`, faded out as the sun reaches the horizon (sun height 0 to 0.25).
  N faces the eye, so only the far side of the leaf from the sun gains; the lit side and leaves in
  shade (sun share 0) are unchanged; flat ground with the sun above it never transmits. Global
  switch `[global] transmit`, `(pc-set-pbr-param 26 n)` (millionths; 0 off, 1000000 as the preset).
- Soft terminator (`wrap`; snow 0.5): the sun and key relief use wrapped N.L,
  `(N.L + w) / (1 + w)`, on both the detail and the geometric normal, so relief on snow rolls into
  shade instead of clipping at N.L = 0 (its contrast drops by 1 / (1 + w)). It acts on the term
  the class shading adds; the bake's own per-vertex falloff is unchanged. Global switch
  `[global] wrap`, `(pc-set-pbr-param 27 n)`.
- Soft terminator in the bake (`bake_wrap`; snow 1.0; switch `[global] bake_wrap`, 0 until picked,
  `(pc-set-pbr-param 29 n)`): on trees with sidecar records, the bake's own sun term (sun gain x
  visibility x day share, from the sidecar) takes a soft N.L on the sidecar normal, `(N.L + w)^2 / 4w`
  between -w and w (unchanged above w, 0 below -w), plus `0.3 w` of cool light (0.84, 1.01, 1.4)
  scattered just inside the shade (a Gaussian in N.L around -0.2, width 0.35), as snow scatters
  light under its surface. Visibility 0 (cast shadow), night and draws without records are
  unchanged. It adds light: the edge of a drift gains up to about half the sun term.
- Backlit leaves need the sun to reach them (`[global] sun_occlusion`, `(pc-set-pbr-param 30
  0/1)`; `leaf_density`, param 31, 0..4, default 2.5; the chosen look is
  transmit 1, sun_occlusion 1, leaf_density 2.5, the `pbr.toml` defaults). The glow is multiplied
  by a sun transmittance (`pbr_sun_transmittance`, pbr_bg_frag.glsl), computed only where the
  sun is behind the leaf: 6 steps of 1.5 m toward the sun through the crowns' leaf volume
  (optical depth `-ln(1 - cover) / thickness` between the lowest and highest leaf, times the
  density), then `lh_sun_blocked` against the level heightfield, starting 2 m out sideways.
  Volume maps: `sandover-unbake canopy-vol` writes v3 maps to `$SANDOVER_DATA/lighting/canopy/`
  (sky levels only); the loader reduces them to 0.5 m cells (2x2 texels, texel i in cell i >> 1)
  and the shader reads them with texelFetch from a two-layer array on unit 43 (`CanopyVol.h`).
  The canopy shade keeps its own v2 maps in `lighting/` (`sandover-unbake canopy`), so the two
  never change each other. Levels without a volume map run neither test. Other strengths, as
  forms: no occlusion `(pc-set-pbr-param 30 0)`; stronger glow `(pc-set-pbr-param 26 2000000)`.
- Wind TIE (`[global] wind_tie`, `(pc-set-pbr-param 28 0/1)`): the instanced wind draws (palm
  canopies) run the TIE wind program, which had no class shading. With 1 they take their
  texture's class like other TIE; there are no sidecar records for them, so the sun share comes
  from the bake (`0.6 x smoothstep(0.35, 0.9, lum(bake))`) and the normal from the triangle.

## Pat census: the footstep material under each texture

Jak's footstep sound comes from the collision triangle's pat material (`pat-h.gc`
`pat-material`: stone, pcmetal, wood, grass, sand, snow, ice, ...; bits 6-11 of the 32-bit
`pat-surface` each `tfrag3::CollisionMesh` vertex carries). `sandover-pat-census` uses it as a
second opinion on `materials.csv`; it only writes candidates, never the live table:

```
uv run sandover-pat-census                     # every level with an .fr3 and a background glb
uv run sandover-pat-census --levels jungleb,snow --out-dir <dir>
```

- Inputs: `out/jak1/fr3/<level>.fr3` (collide mesh, decoded as in `sand_textures.py`; set
  `SANDOVER_ROOT` to the primary checkout from a worktree) and the rip's
  `$SANDOVER_DATA/unbake/riproot/.../levels/<level>/<level>-background.glb` (tfrag, tie, shrub;
  node matrices applied for wind ties).
- Match: each visible triangle's centroid to the nearest of its 16 nearest collision triangles
  by exact point-triangle distance, within `--max-dist` (0.5 m) and `|cos| >= --min-cos` (0.7).
  By default only walkable surfaces: floor-facing visible triangles (`|n.y| >= 0.7`) on
  ground-mode collision. Walls and trunks mostly carry stone, so `--all-surfaces` is noisy.
- Join: glb material name (minus a `-envmap` suffix) to `materials.csv` by name and the level's
  DGO; `join` says how (`name+level`, `name`, `same-class`, `conflict` when one name has rows of
  different classes on the level's tfrag/shrub/pris tpages).
- Verdict per (texture, level): `agree`, `disagree` (a non-stone pat contradicts the class),
  `disagree-stone` (stone is enum 0 and the default, weak evidence), `no-class` (swamp,
  quicksand, tube, waterbottom, stopproj, rotate, neutral have no table class), `no-match`.
- Outputs: `$SANDOVER_DATA/materials/candidates/pat-census.csv` and `pat-census-report.md`
  (reliability, pat areas, disagreements, precursor, stonehenge/darker-metal, ice/snow, and
  table stone on metal or pcmetal). About 4 minutes for all levels.
- Limits: only background collision is in the .fr3; actor surfaces (platforms, doors, elevators)
  have their own and show up as `no-match`. About 28% of visible floor triangles find collision
  in range (finalboss 1%, jungle 49%).

## Sun glints on snow

Sunlit snow sparkles (class `glint`; snow 6.0). A grid of world cells (`[global] glint_cell_m`,
3 cm) on the axis plane facing the surface normal; `glint_density` of the cells hold one crystal
facet, its normal tilted from the surface normal by a Gaussian of rms `glint_spread` (0.35 rad); a
facet lights inside a `glint_width` (0.12 rad) lobe around the sun half vector, as a round HDR
spot near its cell centre (0.06 cells, at least 2 px, at most 0.3 cells) of peak `glint` x sun
colour. The 2x2 cells nearest the pixel are summed, so spots are never cut at cell edges. Where a
cell spans under 3-6 pixels the field fades into its own mean, a sheen along the sun's mirror path,
instead of sub-pixel sparkles.

- Gates: the sidecar's sun share (draws without a sidecar get none), a strong sun above the
  horizon, the face toward the sun, not under an actor shadow, and times the share left after fog
  and haze, so fog stays as dense as before.
- Bloom: the spots (not the far sheen) write the emissive mask as R = max(emissive, e), G =
  max(e - emissive, 0): they bloom, but `emissive_tiles` (R - G) still sees only the emitter, and
  B/A are untouched.
- Switch: `[global] glint`, `(pc-set-pbr-param 32 n)` (millionths; 0 at boot, 1 in `pbr.toml`,
  the candidate variant B2 was 2.5).
