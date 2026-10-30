---
title: What does merc's per-vertex RGBA mean, and why does the plant boss look grey in "texture only" mode?
date: 2026-09-23
issue: none (source: viewer bug report `viewer/reports/20260923-100111`, model `jungleb/plant-boss-main-lod0.glb`)
status: verified
sha: d2b37b508
---
## Answer (≤5 lines)
Merc's per-vertex `rgba` is a constant, art-authored tint/albedo-ish multiplier baked into the PS2 level data at export time — it is read straight off the fragment's raw lump data (`extract_merc.cpp:513`) with no lighting math applied, and copied unchanged into the exported `MercVertex` (`extract_merc.cpp:1209-1212`) and then into glTF `COLOR_0` (`fr3_to_gltf.cpp:404`, `/255`, alpha forced to 1). At runtime the vertex shader computes lighting *separately* from 3 directional lights + ambient (`merc2.vert:75-82`) and only then multiplies it onto this tint: `vtx_color = rgba * light_color` (`merc2.vert:101`); the fragment shader multiplies that by the texture: `color = vtx_color * T0 * 2` (`merc2.frag:24`). So the glb's `COLOR_0` is the tint half of the product, not the lit result — the viewer's "texture only" mode (texture alone) and its "baked" mode (texture × COLOR_0 × 2) both drop the runtime directional/ambient term; the difference between them is entirely the presence/absence of the artist's tint. This is the opposite convention from tfrag/tie, where the baked per-vertex color *is* the lighting result and the texture supplies the color (kb/facts/mood.md claim 4; `tfrag3.vert:52-56`).

## Evidence (path:line@sha, short excerpts)
**1. The `rgba` attribute is raw PS2 art data, not a computed value.**
- `decompiler/level_extractor/extract_merc.cpp:513@d2b37b508`: `vtx.rgba = frag.unsigned_four_including_header.at(rgba_ptr);` — read from `frag.header.rgba_off` (`:370`), a lump offset into the fragment's raw uploaded VU data, alongside position/normal/UV. No lighting, normals, or light directions are consulted to produce it.
- `decompiler/level_extractor/extract_merc.cpp:1209-1212@d2b37b508`: `out.rgba[0..3] = vtx.rgba[0..3];` — copied verbatim into the exported `tfrag3::MercVertex`.
- `goal_src/jak1/engine/gfx/merc/merc-h.gc:32@d2b37b508`: `(rgba-off uint8) ;; location of rgba data` — the GOAL-side struct names this offset the same way; it is PS2 GIF `RGBAQ`-register-style per-vertex Gouraud color data (matching the frag shader's own comment, `merc2.frag:22`: `// all merc is tcc=rgba and modulate`), not a lighting output.

**2. The exporter writes this raw value straight into glTF `COLOR_0`.**
- `decompiler/level_extractor/fr3_to_gltf.cpp:394-406@d2b37b508` (`make_color_buffer_accessor(const std::vector<tfrag3::MercVertex>&, ...)`): `floats.push_back(((float)vertices[i].rgba[j]) / 255.f);` for j=0..2, then `floats.push_back(1.f);` for alpha. No lighting is evaluated here; it is a pure byte→float normalization of the same `rgba` field from (1), with alpha discarded and hardcoded to 1.0. This accessor is wired to `prim.attributes["COLOR_0"] = colors;` (`:1798`).

**3. The engine computes lighting separately at runtime and multiplies it onto this tint.**
- `game/graphics/opengl_renderer/shaders/merc2.vert:75-82@d2b37b508`:
  ```
  vec3 light_intensity = light_dir0_fade.xyz * rotated_nrm.x + light_dir1_fade_en.xyz * rotated_nrm.y + light_dir2 * rotated_nrm.z;
  light_intensity = max(light_intensity, vec3(0, 0, 0));
  vec4 light_color = light_ambient + light_intensity.x * light_col0 + light_intensity.y * light_col1 + light_intensity.z * light_col2;
  ```
  computed from the bone-space normal (`rotated_nrm`) and the per-model `VuLights` uniforms (3 directions/colors + ambient, uploaded per-draw — `Merc2.cpp:1287-1303`, already noted in `kb/facts/renderer.md` claim 3).
- `game/graphics/opengl_renderer/shaders/merc2.vert:101@d2b37b508`: `vtx_color = rgba * light_color;` — the art-time tint (`rgba`, from (1)/(2)) is multiplied by the just-computed runtime lighting (`light_color`). These are two independent terms multiplied together, not one value.
- `game/graphics/opengl_renderer/shaders/merc2.frag:24@d2b37b508`: `color = vtx_color * T0 * 2;` — the fragment shader then multiplies that combined `tint × lighting` term by the texture sample and doubles it (standard PS2 GS "modulate ×2" convention, also used by tfrag: `kb/facts/mood.md` claim 4).
- Net: `final_color = rgba(tint) * light_color(runtime) * texture * 2`. The exported glb only has `rgba` (as `COLOR_0`) and `texture` — the runtime `light_color` term from real in-game directional/ambient lights is absent from any static export. A viewer's "texture × COLOR_0 × 2" mode is therefore an approximation that recovers the artist's tint but substitutes an implicit light of 1.0 for the true runtime lighting; "texture only" drops the tint entirely and is missing both terms.

**4. tfrag/tie is the opposite convention — contrast.**
- `game/graphics/opengl_renderer/shaders/tfrag3.vert:52-56@d2b37b508` / `tfrag3.frag:17-21@d2b37b508`: `fragment_color = texelFetch(tex_T10, time_of_day_index, 0); fragment_color *= 2;` then `color = fragment_color * T0;` — here the per-vertex color (`tex_T10`, the time-of-day-blended baked palette) *is* the lighting result (an offline PS2-era bake, byte 128 = 1.0), and the texture supplies the actual albedo/color. There is no separate constant "tint" term for tfrag/tie — confirmed and already recorded as `kb/facts/mood.md` claim 4 (status: verified).
- So: for tfrag/tie, vertex color = baked lighting, texture = albedo. For merc, vertex color (`COLOR_0` in the export) = constant art tint, and the runtime *computes* lighting separately and never exports it. These are not the same kind of data despite both being called "vertex color".

## Sampling: is the tint pattern common across merc models, or specific to the plant boss?
Sampled 7 merc glbs (all with `JOINTS_0`/`WEIGHTS_0`/a skin, confirming the merc/bone-rig path) across 4 levels, via `pygltflib` reading each primitive's actual referenced vertices (by index buffer, not the whole shared vertex array) and comparing the primitive's base-color texture sampled at `TEXCOORD_0` against that same sample multiplied by `COLOR_0 * 2`. Mean saturation (HSV) and mean RGB, per model (aggregated over all textured primitives):

| Model (level) | texture-only mean sat | mean RGB | texture×COLOR_0×2 mean sat | mean RGB |
|---|---|---|---|---|
| plant-boss-main (jungleb) | 0.252 | (0.80, 0.84, 0.78) — grey-tan | 0.535 | (0.53, 0.58, 0.49) — olive/green |
| eichar / Jak (common) | 0.076 | (0.83, 0.81, 0.80) — near-white | 0.639 | (0.42, 0.34, 0.24) — tan/brown |
| sidekick / Daxter (common) | 0.581 | (0.87, 0.61, 0.35) — already orange | 0.736 | (0.66, 0.39, 0.16) — deeper orange |
| mayor (beach) | 0.496 | (0.63, 0.48, 0.45) | 0.526 | (0.53, 0.39, 0.36) |
| lurkercrab (beach) | 0.620 | (0.70, 0.32, 0.28) | 0.775 | (0.47, 0.16, 0.21) |
| gambler (village2) | 0.491 | (0.61, 0.48, 0.39) | 0.561 | (0.47, 0.36, 0.29) |
| crate-wood (common, merc-rigged prop) | 0.210 | (0.42, 0.43, 0.36) | 0.210 | (0.42, 0.44, 0.36) — **unchanged** |

Saturation rises with the tint applied for 6 of 7 models (largest jumps on plant-boss and eichar, both of which use near-flat/near-white base textures and rely on `COLOR_0` for most of their perceived color). `crate-wood` is the exception: its `COLOR_0` is ~neutral (~0.5,0.5,0.5, i.e. raw byte ~128), so `texture × 0.5 × 2 ≈ texture` — the tint mechanism is present (same vertex format, same shader path) but the artist simply didn't use it for this asset, letting the texture carry all the color. So the pattern — texture-only reads flat/desaturated, texture×COLOR_0×2 recovers the artist's intended color — is common on characters/creatures and not universal on inert props, but the *mechanism* (constant per-vertex tint × runtime lighting) is uniform across all sampled merc models.

**Plant boss detail** (per-material breakdown, actual indexed vertices, `plant-boss-main-lod0.glb`): the two body-shell materials use `plant-white4x4` — a flat white (sat 0.000, RGB (1,1,1)) placeholder texture — so their color comes *entirely* from `COLOR_0`; e.g. one region has `COLOR_0` mean (0.18, 0.22, 0.12) → texture×COLOR_0×2 = (0.37, 0.44, 0.24), a clear green with G>R>B. The `plantarms-stripes` material: texture-only (0.92, 0.90, 0.86) near-white/tan (sat 0.075) → texture×COLOR_0×2 = (0.42, 0.46, 0.14), olive-green (sat 0.711). Not every material on this mesh is green (mouth/headtop textures are magenta/pink by design, the eye is yellow), but the dominant body/arm materials go from grey-white to green only once `COLOR_0` is applied, matching the report's "the plant should be green."

## Implications for the design
1. **PBR albedo for characters must include the vertex tint.** Sampling a merc texture alone as "albedo" (as tfrag/tie textures can be, since those textures already are full-color and pre-lit-vertex-color is separate) is wrong for merc: many character/creature assets push real color information into `COLOR_0`, sometimes exclusively (flat-white placeholder textures like `plant-white4x4`). Any albedo extraction or texture-upscaling pipeline for merc models must multiply in the per-vertex tint (or bake it into a per-material color multiply) to get a correct base color; `crate-wood`-style props are a safe degenerate case (tint ≈ neutral) but can't be assumed generally.
2. **Character relighting must keep the tint and the lighting term separate.** If a future relighting/PBR pass replaces `light_color` (the 3-directional+ambient runtime term) with a new lighting model, `rgba` (the tint) must still be multiplied in afterward — it is not part of "lighting" and must not be discarded or re-baked into it. Conversely, don't reuse `COLOR_0` from a glb export as a lighting proxy the way tfrag/tie vertex color can be used; for merc it is albedo-adjacent, not light.
3. **Viewer UI.** Rename "unlit texture" mode to something like "texture only (no vertex tint)" to make clear it is missing the artist's per-vertex color, not just missing dynamic lighting. Make the tinted mode ("texture × vertex color × 2", currently "baked") the default preview for character/merc models, since it is materially closer to what the game actually draws (tint present; only the true directional/ambient runtime lighting term is still missing/approximated as uniform).

## Follow-ups
- Extend the viewer's "baked" mode to accept an explicit `light_color` override (matching the report's stored `light` block) so texture×COLOR_0×light_color×2 can be compared directly against an in-engine screenshot at the same camera/light, rather than approximating light_color as 1.0.
- If a PBR material pipeline needs a single flattened "base color texture" per merc part, consider baking `COLOR_0` into a new texture variant at export time (multiply per-triangle/per-UV-island) so downstream tools don't need to special-case merc vs. tfrag/tie vertex-color semantics.
