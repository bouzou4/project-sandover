---
title: Envmapped ties wrote face normals, so local lights drew their polygons
date: 2026-10-23
issue: play capture runs/20261023-1042-play/0008 (jungle-start, 19 h)
status: verified
sha: 9b0faff21 (base), fix/envmap-tie-normals
---
## Answer (≤5 lines)
The jungle canyon wall is an envmapped TIE (`cmn-precursor-metal-plain-01-hitweak`). Envmapped
tie draws never bound the `.unbake` sidecar's smooth normals, so they wrote a per-triangle face
normal into the normal target, and the tile-light pass lit every triangle with its own flat N.L.
Fix: the envmapped base and shine passes read the same sidecar smooth normal as every other tie.
At the reported camera the log-luminance step across face edges drops from 0.35 to 0.08 (inside a
face: 0.05).

## Symptom
Play snapshot 0008 (jungle at 19 h, crop `crops/0008-a1.png`): "I can see the literal polygons in
the eco reflection." A yellow eco shot near the canyon wall lights the rock in flat triangles with
hard steps between them. The highlights-off frame keeps the steps, so it is the diffuse term.

## Mechanism
- The deferred tile-light pass (`emissive_light.frag`) shades with the normal target (colour
  attachment 1). Each background draw chooses what it writes there with `ssao_nrm`:
  1 = the face normal, `cross(dFdx(pos), dFdy(pos))`, constant over a triangle; 2 = the vertex
  normal interpolated across it.
- `Tie3::draw_matching_draws_for_tree` called `unbake_bind_normals` (which binds the sidecar
  records to unit 11 and sets mode 2) only when `!use_envmap`. `first_tfrag_draw_setup` sets mode
  1 for every program, so envmapped draws stayed on face normals. `etie_base.vert` and `etie.vert`
  also hard-coded `ssao_vnrm = 0`.
- On a low-poly rock face the face normals of neighbours differ by tens of degrees. A small local
  light close to the wall turns that into a per-triangle brightness: a step of 0.35 in natural log
  luminance (about 40 %) at every edge. The baked vertex colours are interpolated per vertex, so
  only the added light shows the triangles.
- The sidecar already carries a smooth, area-weighted normal for every vertex of the tie tree
  (record.y, flag bit 8), envmapped draws included: the tree shares one vertex buffer across
  categories, and `etie_base` already reads the same records for PBR and the emissive mask.

## Fix
- `Tie3.cpp`: the normal bind uses one rule for all tie draws (LOD 0, normals active, valid
  sidecar tree), and the envmap shine pass binds it too after its own setup.
- `ssao_normal_vert.glsl`: `ssao_etie_vnrm` maps the sidecar normal to clip (x, y, w) space with
  the cofactor of the etie projection's linear part (view matrix, `persp0/persp1`, the 2048 offset
  and the 256/128 scale are all linear in the view position once multiplied by w). Both etie vertex
  shaders call it; mode 1 or a missing record still gives zero, i.e. the face normal.
- The reflection itself is untouched: `etie.vert` still builds the envmap lookup from the game's
  own per-vertex normal attribute; only the SSAO/tile-light normal changes.
- Cost: one buffer fetch and a 3x3 cofactor per envmapped vertex. Two solo A/B pairs read +1.1 ms
  (jungle, A-A spread 0.9) and +0.4 ms (citadel, spread 0.25); the code does not explain that,
  so the cost needs an interleaved A/B re-measure.

## Proof
Same camera as the report (`jg-shot` of `eco-reflections.yaml`, yellow shot at the wall, play
look), A = base, B = fix. Face edges = pixels where A's normal view (SSAO view 3) changes by more
than 12/255 across ±3 px; faces = 9x9 windows with no change; lit half of the wall only.

| frame | edge step (median ln L) | in-face step | ratio | pass (ratio < 2) |
|---|---|---|---|---|
| play build (reported) | 0.264 | 0.026 | 10.4 | no |
| A base | 0.348 | 0.047 | 7.5 | no |
| B fix | 0.076 | 0.049 | 1.5 | yes |

B's normal view is a smooth gradient over the wall. What remains above the in-face level are
real silhouettes (one rock in front of another), not shared triangle edges.

Elsewhere (citadel generator room, precursor metal): the envmapped pipes and bands now show the
same smooth normals as the rest of the room in the normal view; the reflection look A/B is in the
data dir (`shots/envmap-tie-normals/`). Thin bevel strips at hard creases take an averaged normal,
the same as non-envmap ties with the area-weighted rule.

## Not changed
Shrubs, wind ties and generic still write face normals (`kb/howto/ssao.md`); none showed at this
camera.
