---
title: Can the baked time-of-day vertex lighting be unbaked into a small light model?
date: 2026-09-23
issue: "#9"
status: verified
sha: 76e8eec (engine sources read); fits run on branch e1/unbake-spike
---
## Answer (≤5 lines)
**NO-GO for a general unbake; the numbers support a narrow GO on five levels.** Reconstruction is fine everywhere (median 1–6 u8 steps with an occlusion term). The sun directions only pass the ~10° gate on the levels that use `*village1-mood-lights-table*` (village1, beach, jungle, finalboss; training at 13–18°). Elsewhere the bake either has almost no directional light to recover (misty, village2, swamp, sunken, sunkenb: sun ≈ 10% of the fitted light, and the direction is not identifiable), or it disagrees with the table by 30–86° (firecanyon, rolling, village3). Night slots miss by 24–43° on village1, beach, jungle and training (finalboss: 8° / 91°). Effect slots are not well explained by point lights.
**Recommendation:** make **detail relighting** the default. Optionally add **delta relighting** (a moving sun) on the five village1-table levels. The maintainer decides at the design review; both paths are costed below.

## Evidence (path:line@sha / URL, short excerpts)

### Setup
- Exporter patch on `e1/unbake-spike`: `kMaxColor` 1 → 8 (`decompiler/level_extractor/fr3_to_gltf.cpp:735@76e8eec`), plus a new `_COLOR_INDEX` vertex attribute holding the palette entry. Output: `COLOR_0..7` on every tfrag/tie/shrub primitive, verified with pygltflib.
- **Rip hazard:** `rip_levels` writes the fr3 **with textures** into `out/<game>/fr3` (`extract_level.cpp:331-345,375-388@76e8eec`, `decompilation_process.cpp:304@76e8eec`), which is the directory the running game loads from. So the rip was run against a sandbox project root under `$SANDOVER_DATA/unbake/riproot` (`OPENGOAL_PROJ_PATH` pointed there, with the configs copied in). The glbs are in `$SANDOVER_DATA/unbake/riproot/decompiler_out/jak1/levels/`, not the shared `decompiler_out/`.
- The shared build tree could not relink `common.dll` while a game instance was running. The decompiler target was built in a separate build dir, `$SANDOVER_DATA/build/e1-decomp` (configure 369 s, `ninja decompiler` 95 s with the sccache hot). Rip: 94 s. It produced 897 glbs (6.0 GB); the 24 level backgrounds are 2.5 GB.
- Fits: `sandover-unbake` (`sandover/tools/sandover/unbake/`), 30k-vertex subsample for the global solve, then per-vertex terms on every vertex. Each level took 2–13 min; four levels ran in parallel, about 50 min in total.

### Model
Per vertex i and sun slot s, in u8 steps (128 = 1.0): `C[i,s] = a[i]·Amb[s] + v[i,s]·k[i]·max(0, n[i]·d[s])·Lgt[s]`.
- Globals `d, Lgt, Amb` come from `scipy.optimize.least_squares` (soft-L1, bounds). `a, k` are solved in closed form inside each evaluation (non-negative, variable projection), and `v ∈ [0,1]` is fitted afterwards.
- Normals are area-weighted face normals on position-welded vertices. Their orientation per connected component is chosen by the fit, because the ripped winding is not outward-consistent (on village1 tfrag only 40% of faces point up).
- Flipping every normal and every sun together gives an identical fit, so suns are kept above the horizon.
- **Prior-free check:** every level with two or more slots was also fitted from "all suns straight up". A brute-force per-slot direction scan was followed by the same solve. On village1 this lands on the same solution as the table-initialised fit to within 1.2°.
- **Grouping by palette entry is not possible.** The palette is vector-quantised: village1 tfrag has 1024 entries shared by 76k vertices (about 75 vertices per entry, with different normals). Everything is therefore fitted per vertex.

### Per-level results
Angles are fit vs `mood-lights` table per sun slot. "Prior-free" is the prior-free solution vs the table-initialised one, taken over slots whose fitted sun is at least 40 u8 and folded for the global sign. Errors are |fit − baked| over tfrag+tie vertices × sun slots × RGB. "Sun share" is the directional term's share of the fitted light.

| level | slot type | sun slots: angle to table (°) | prior-free (°) | median / p90 u8, no occl. | median / p90 u8, with v | sun share | verts needing occl. | verdict |
|---|---|---|---|---|---|---|---|---|
| village1 | cycle | 0:5 1:7 2:5 3:11 4:6 · 6:37 7:35 | 1.2 | 6.1 / 21 | 4.0 / 14 | 19% | 51% | **pass** (day) |
| beach | cycle | 0:4 1:4 2:4 3:13 4:11 · 6:41 7:30 | 0.8 | 5.4 / 20 | 3.3 / 12 | 25% | 46% | **pass** (day) |
| jungle | cycle | 0:9 1:14 2:2 3:12 4:8 · 6:37 7:30 | 0.5 | 4.9 / 18 | 3.0 / 11 | 18% | 52% | **pass** (day, ≤14) |
| training | cycle | 0:18 1:17 2:3 3:15 4:13 · 6:24 7:43 | 4.4 | 6.6 / 25 | 4.1 / 15 | 21% | 57% | marginal |
| finalboss | bespoke | 0:7 1:10 2:12 3:10 4:11 6:8 · 7:91 | 0.1 | 7.7 / 27 | 4.4 / 19 | 26% | 54% | **pass** (day) |
| firecanyon | cycle | 0:29 1:3 2:56 3:57 | 13.2 | 7.6 / 21 | 6.2 / 19 | 12% | 35% | fail (bake ≠ table) |
| snow | cycle | 0:13 1:7 2:12 3:40 | 23.0 | 2.9 / 10 | 2.3 / 8 | 6% | 34% | marginal, unstable |
| ogre | bespoke | 0:12 1:15 2:55 3:38 | 15.1 | 4.8 / 16 | 3.6 / 14 | 12% | 28% | marginal |
| rolling | cycle | 0:32 1:34 2:38 3:57 | 35.5 | 3.6 / 11 | 3.2 / 10 | 11% | 15% | fail |
| village3 | bespoke | 0:86 1:12 2:64 3:74 | 30.1 | 4.1 / 18 | 3.5 / 14 | 15% | 16% | fail |
| misty | cycle (2 slots) | 0:161 1:108 | 54.3 | 3.3 / 12 | 3.3 / 12 | 10% | 2% | no signal |
| village2 | cycle (2 slots) | 0:168 1:85 | 32.2 | 2.0 / 7 | 2.0 / 7 | 10% | 0% | no signal |
| swamp | cycle (2 slots) | 0:18 1:91 | 29.0 | 1.5 / 5 | 1.5 / 5 | 10% | 0% | no signal |
| sunken | cycle (2 slots) | 0:0 1:0 (did not move) | 0.1 | 1.2 / 39 | 1.2 / 39 | 24% | 0% | no signal, R² 0.42 |
| sunkenb | cycle (2 slots) | 0:0 1:0 (did not move) | 80.2 | 2.2 / 11 | 2.2 / 11 | 23% | 0% | no signal |
| maincave | quick (slot 0) | 0:3 | – | 2.3 / 14 | 2.3 / 14 | 21% | 0% | n/a (one slot) |
| robocave | quick | 0:50 | – | 1.4 / 13 | 1.4 / 13 | 21% | 0% | n/a |
| darkcave | quick | 0:179 | – | 0.0 / 0 | 0.0 / 0 | – | 0% | n/a (slot 0 black) |
| jungleb | quick | 0:0 | – | 0.8 / 5 | 0.8 / 5 | 17% | 0% | n/a |
| lavatube | quick | 0:5 | – | 2.1 / 15 | 2.1 / 15 | 32% | 0% | n/a |
| citadel | quick | 0:2 | – | 0.2 / 1 | 0.2 / 1 | 3% | 0% | n/a |

Slot types come from `sandover/kb/facts/mood.md`. "Quick" levels pin slot 0 through `update-mood-quick`. One slot gives three observations per vertex against two per-vertex unknowns, so their direction is not identifiable, and neither are the two-slot levels'. An ambient-only (rank-1) baseline gives medians of 9.4 (village1), 10.5 (beach) and 3.8 (village2) u8. The sun term buys a lot on the sunny levels and almost nothing on the overcast ones.

The fit error varies by geometry type. On the sunny levels, with v: tie 2.8–4.7, tfrag 3.5–5.5, shrub 8–16 u8. Shrubs are worst: they are likely two-sided cards, and their `rgba_base` term is not in the model.

**Night slots (6, 7) are systematic, not noise.** village1, beach and jungle all converge, from both inits, to a moon at about (0.35, 0.91, 0.22) and (0.16, 0.97, −0.14). The table has (0.84, 0.48, 0.26) and (0.35, 0.87, 0.35). The offset is in elevation, not a mirror or axis swap: positions and table share the game frame, and the day slots pin both x and z. Inferred: the bake's night light sat higher than the character light. Caveat: the model has no sky/hemisphere term, and with a weak moon an upward-biased sky fill would also pull the fitted direction up.

**Occlusion is real.** On the sunny levels 46–57% of vertices need v < 0.5 in at least one lit slot, and in each slot 19–44% of sun-facing vertices are shadowed. The bake contains cast shadows, and a moved sun must carry a visibility term.

### Effect slots (point-light clusters, lit vertices = slot mean > 12 u8)
Seeds come from DBSCAN on the hot vertices (eps 2 m, at most 64 lights). Each light is refitted locally against the residual of the others, using both the plain `N·L/r²` falloff and a windowed one. Errors are median / p90 u8 on the lit vertices.

| slot role | levels | lit verts | ambient-only | best point-light fit |
|---|---|---|---|---|
| flames | village1, misty, village2, swamp, snow, village3, maincave, robocave, citadel | 0.2–1.3% | 15–32 / 62–124 | 4–20 / 35–95 |
| light (lamps) | jungle, training, misty, rolling, sunken(b), lavatube, citadel | 0.6–40% | 23–128 / 78–239 | 11–87 / 54–199 |
| lava | firecanyon | 18–21% | 20 / 83–96 | 14–15 / 74–85 |
| fade-lights | darkcave 1–7 | 7–16% | 20–26 / 38–45 | 7–11 / 22–31 |
| lightning, caustics, fade | village2, swamp, ogre, sunken(b) | – | not modeled | – |

Point lights cut lit-vertex error by 30–70%, but the p90 stays at 35–100 u8. On village1 the lit vertices sit 1–4 m from their nearest fitted light, and 39% of them face away from it. That fits self-lit lamp or torch geometry (emissive-like), which point lights cannot reproduce. Around them the light has a sharp ~5 m footprint. Beach slot 5 and lavatube 4–7 have no lit vertices at all. The full table is in `$SANDOVER_DATA/unbake/report.md`.

### Artefacts (all in `$SANDOVER_DATA/unbake/`, gitignored)
- `<level>-summary.json`: per slot, the fitted and table directions, colours and errors.
- `<level>-fit.npz`: per-vertex `a, k, v`, normals, baked and fitted colours.
- `<level>.rrd`: Rerun recording. Timeline `slot`; views baked / fitted / residual / visibility, plus arrows for the fitted and table suns and the point lights.
- `<level>-residual.glb`: `COLOR_0..4` = noon residual, residual without occlusion, noon visibility, baked, fitted.
- `report.md`, `diag.md`: the tables above, regenerated by `python -m sandover.unbake.report` and `python -m sandover.unbake.diag`.

## Implications for the design
Both paths keep the baked palette as the base, so vanilla parity is structural. The differences are what is added on top and what that costs.

**Path A: detail relighting (recommended default).** Keep the bake. Add SSAO and a per-pixel detail term, `baked · (1 + s·(N_detail·L − N_geo·L))`, with `L` taken from the mood table. The table is validated here to about 5–13° against the bake on the village1-table levels. On the other levels it is still a plausible key-light direction for detail only.
- Geometric normals for tfrag/shrub can come from screen-space derivatives, so no fr3 change is needed.
- Cost: about one engine week after SSAO, plus the PBR-phase normal maps.
- Risk: low. Parity holds by construction (s = 0).
- Limit: no moving sun or shadows. Time of day stays the 8-slot blend.

**Path B: delta relighting on village1, beach, jungle, training and finalboss (optional add-on).** Per vertex:
`C(t) = Baked(t) + k·v(t)·[max(0,n·d′)·L′ − max(0,n·d)·L] + a·(Amb′ − Amb)`.
Primed values are the new sun; unprimed values are the fitted sun for the current blend. It is exact parity when the new sun equals the fitted one, and model error only affects the delta. That makes the 3–4 u8 median acceptable.
- **Data:** per vertex n (3×s8), a and k (2×u8) and v for the 7 sun slots (4-bit packed). That is about 10 bytes per vertex, about 7 MB for village1. It cannot live in the palette, which is vector-quantised, so it needs a per-tree, per-vertex `.unbake` sidecar in fr3 vertex order and an extra VBO in the tfrag3/tie3/shrub renderers.
- **Other work:** extend the `add-pc-tfrag3-data` packet with the new sun, and add shader terms.
- **Cost:** about 2–3 engine weeks plus 1 tools week.
- **Risks:**
  - Shadows only exist at the 7 baked sun positions, so v is interpolated along the arc.
  - Night slots disagree with the table, so the fitted values must be used, not the table.
  - The sun is only 18–26% of the baked light on these levels, so the visible change is moderate.
  - Five levels only. Every other level stays on Path A.

**Effect slots: carry through as baked layers in both paths.** Their runtime flicker weights already animate them. Point-light proxies are good enough to *light characters and actors* (≈30–70% error reduction), but not to replace the baked layer.

## Follow-ups
- DECISION (design review): Path A only, or Path A plus Path B on the five village1-table levels.
- If Path B: check vertex-order stability between fr3 and glb export (the sidecar key), and prototype v interpolation between adjacent slots on village1.
- Shrub lighting needs its own look. The `rgba_base` multiplier and the two-sided cards are not in the model.
- sunken R² 0.42 and village3's runtime light-table rewrite: the bake there follows neither the static table nor the runtime override.
- Rerun `.rrd` files are about 1 GB for all levels. They can be regenerated and should not be backed up.
