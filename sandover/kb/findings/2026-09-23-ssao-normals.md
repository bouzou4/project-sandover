---
title: SSAO normal target - do real normals remove the facet creases?
date: 2026-09-23
issue: none
status: verified
sha: branch ssao-normals on 1f33521cf
---
## Answer (≤5 lines)
Yes. The opaque passes now write a normal target, and on the two shots reported
(beach-start-noon sand, sunken-start-dusk dome) the facet lines are gone at the revision-1 pick
("deep"). Real normals alone were not enough. Smooth normals on flat low-poly facets tilt each
facet's tangent plane, so the facet shades itself. AO therefore tests every sample against both
the smooth normal and the depth face normal, and it does not count shallow samples that lie on
the same smooth surface. SSAO off passes the parity gate, and SSAO on at intensity 0 matches off.
"Deep" was approved on real normals directly and skipped the third comparison.

## Evidence (path:line@sha / URL, short excerpts)
**Normal target** (verified by code; `SsaoRenderer.h` class comment, `shaders/ssao_normal_*.glsl`).
- Format: `GL_RGB10_A2` with the frame's sample count, attached as colour 1 of the render
  framebuffer only while SSAO is on. It holds the unit normal in clip `(x, y, w)` space as
  `n * 0.5 + 0.5`, and zero means "no normal". The AO pass maps it to view space with
  `(pxx/pzw, pyy/pzw)`, so no pass needs a view matrix. It takes about 59 MB at
  2560x1440 MSAA 4.
- Buckets 5-30 and 48-56 enable draw buffer 1; every other bucket sets it to `GL_NONE`.
  Blends other than plain alpha mask it with `glColorMaski`.
- Sources: tfrag and non-envmap tie (LOD 0) use the `.unbake` smooth normal. Its world
  normal is mapped by the cofactor of the vertex transform's clip-space columns. Merc and
  emerc use their skinned vertex normal, mapped the same way through the blended bones.
  Shrub, generic, wind tie and envmapped tie use the face normal from `dFdx`/`dFdy` of clip
  `(x, y, w)`. Smooth normals take the face normal's side.
- Checked against depth: the view-3 target normals and the view-6 depth normals differ by a
  median 5.5° (beach) and 5.0° (sunken), and only 0.8% / 0.4% of pixels differ by more than
  90°. That rules out a wrong space or wrong signs; the rest is smooth-versus-faceted.
  Coverage on beach-start-noon: 83% of pixels have a target normal, and the only red
  (fallback) regions are the ocean and the far water, which draw outside the buckets
  (`$SANDOVER_DATA/shots/ssao/normals/beach-start-noon-src.png`).
- Sidecars: beach, jungle, village1, training and finalboss come from the lighting set. I
  added sunken, sunkenb and village2 (`sandover-unbake export-sidecar ... --out
  $SANDOVER_DATA/lighting/extra`) so the sunken dome gets smooth normals, and installed them
  into `out/jak1/fr3/`.

**AO sample test** (verified by captures; `ssao_ao.frag` `aoValueFromPositionsAndNormal`).
| Variant | beach crop: AO mean / high-freq | sunken crop: AO mean / high-freq | Look |
|---|---|---|---|
| before (normals from depth, param 14 = 0) | 247.2 / 5.30 | 243.9 / 2.34 | thin facet lines |
| smooth normal only | 240.5 / 7.66 | 234.4 / 3.09 | lines gone, shaded triangle patches and banding (self-occlusion) |
| min(elevation over smooth, over face) | 249.4 / 4.38 | 247.4 / 1.87 | patches gone, lines fainter |
| + same-surface discount (final) | 250.8 / 2.97 | 249.3 / 1.31 | lines gone, contact at the pillar and bushes kept |
Crops are the revision-2 insets on the blurred-AO view (debug view 2), "deep". High-freq is the
mean absolute Laplacian. Discount: a sample whose smooth normal is within about 5-16° of the
pixel's (`smoothstep(0.96, 0.995, |n_C·n_Q|)`) and whose elevation is under about 10-18°
(`1 - smoothstep(0.17, 0.31, sin)`) is not treated as an occluder. Loosening both gates
(8-20°, 15-25°) changed the crop numbers by at most 0.05. Two-up:
`$SANDOVER_DATA/shots/ssao/normals-before-after.png`.

Cost of looks: on village1-hut-noon the mean AO level goes from 243.3 to 247.3 with the
normal target (top half 244.5 → 250.8). The two changes contribute about equally (without
the discount it is 245.8). Most of that was facet shading on distant rocks and walls. Ground
contact under bushes and Jak is kept.

**Parity** (measured; 16-shot `auto-v0`, 4 instances, 2560x1440 MSAA 4). Pixel counts are
per set over all 16 shots.
| Pair | FLIP mean of means | worst mean | px differing (16 shots) | shots identical |
|---|---|---|---|---|
| master vs feature, SSAO off (run a) | 0.00033 | 0.0020 | 49023 | 5/16 |
| master vs feature, SSAO off (run b) | 0.00054 | 0.0021 | 90841 | 4/16 |
| feature off vs off (noise floor) | 0.00053 | 0.0028 | 89934 | 5/16 |
| feature off vs "deep" at intensity 0 | 0.00047 | 0.0025 | 86092 | 6/16 |

Master is a clean build of 1f33521cf, run on the same captured data with master's own
shaders. Every differing pixel sits in the known animated-actor boxes: Jak's idle, birds,
jungle and sunken creatures, and the village1 dawn flyer. Every shot that is identical in the
off/off pair is also identical in off vs intensity 0. Sets:
`$SANDOVER_DATA/shots/ssao/parity-normals/{master-off,off-a,off-b,inert}`. With SSAO off
nothing is attached, and the fragment output at location 1 has no draw buffer.

**Cost** (measured, solo, no optimization; `sandover-ssao-cost`).
Indicative only: one solo run, two 240-frame windows per variant, 2560x1440 MSAA 4, noon. The
run was cut short, so it has none of the usual repeat runs. Raw numbers are in
`$SANDOVER_DATA/shots/ssao/cost-normals-1440p-msaa4.json`, if written.

| Spot | ssao-total, depth normals | ssao-total, normal target | ssao-ao, depth → target |
|---|---|---|---|
| village1-hut | 0.73-0.74 | 0.80-0.81 | 0.12 → 0.29 |
| beach-start | 0.62-0.81 | 0.94-0.96 | 0.12 → 0.31-0.32 |
| jungle-start | 0.67-0.68 | 0.91-0.92 | 0.12 → 0.35 |

The AO pass does most of the extra work: one extra normal fetch per tap plus the averaging
over 4 samples. The opaque buckets' extra output shows up as a noisy 0.1-0.5 ms rise in
whole-dispatch time. No optimization was attempted.

## Implications for the design
- ADR-0013's opaque-pass normal target now exists. A point-light pre-pass can read it: the
  view-space mapping is two scalars.
- Smooth normals on low-poly geometry need the dual test above wherever they drive
  screen-space occlusion or contact effects. On their own they trade lines for patches.
- Levels without a sidecar get face normals on tfrag and tie. They still avoid the depth
  reconstruction's cross-edge mixing, but facet shading remains there. Sidecars for every
  level with an unbake fit are cheap (sunken: 11.5 MB).

## Follow-ups
- Export and install sidecars for the remaining fitted levels (normals only, if the sun
  stays scoped to five levels).
- The crease threshold (params 12 and 13) is now redundant. It stays available with a
  default of 0.
