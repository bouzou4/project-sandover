---
title: Can a material-response model make metal, stone, wood, cloth and foliage read as different materials under the same light?
date: 2026-09-25
issue: none
status: inferred
sha: c336038fe (base; branch material-response)
---
## Answer (≤5 lines)
Partly yes. With the metallic workflow and the mood environment, metal and precursor bronze now
read as metal (sky in the reflection, a roughness-shaped sun highlight, dark where they face the
ground); rough stone keeps a matte look with deeper relief; roof tiles and wood decks get a glaze
or satin highlight. Foliage changes least: the palm canopies are wind TIE, a path the shading does
not reach. Three looks (faithful, bold, cinematic) in `presets/pbr.toml`; off by default.

## Model (per shaded pixel, on top of the vanilla colour = albedo x bake D)
- **Metallic workflow**: m = metallic(texture) x `metallic`; diffuse x= (1 - m)(1 - `metal_darken`
  x metallic); F0 = mix(`specular` x 0.04, albedo x class tint, m). Precursor keeps its bronze
  through the class tint in F0. Fresnel on metals is damped (hue kept at grazing angles).
- **Mood environment**: per frame on the CPU from the live fog colour (horizon), a deeper and more
  saturated version of it (zenith), the fitted ambient mixed with a warm ground (below), and a
  sun glow lobe from the fitted sun colour; scaled so the mean over the sphere has luminance 1, so
  a reflection is relative to the local bake level (lum D). Sampled with the reflection vector,
  blurred toward the hemisphere irradiance as roughness grows. Metals reflect it fully (with
  `env` = 0 they reflect its mean, i.e. keep the bake's brightness); dielectrics reflect it by
  Fresnel x (1 - roughness)^2 x `env`, so rough stone does not reflect the sky.
- **Lobe**: GGX (normalised to its peak, peak gain capped at 60) x Smith visibility x Fresnel, for
  the sun (weight: sidecar visibility x sun share) and the mood key light; `band` blends in the
  old two-step stylized band; `lobe` scales it.
- **Ambient relief**: the hemisphere evaluated on the detail normal relative to the geometric
  normal, gain `ambient`; also scales the sun/key relief (1 + 2 x `ambient`).
- **Sheen** (cloth 0.8, foliage 0.3): grazing rim in the albedo hue.

## Presets (`presets/pbr.toml`)
| | metallic | env | fresnel | ambient | rough contrast | band | lobe | specular | metal darken |
|---|---|---|---|---|---|---|---|---|---|
| faithful | 0.35 | 0.25 | 0.5 | 0.15 | 1.0 | 1.0 | 0.5 | 1.0 | 0 |
| bold | 1.0 | 0.6 | 1.0 | 0.5 | 1.2 | 0.3 | 0.7 | 2.0 | 0 |
| cinematic | 1.0 | 1.0 | 1.6 | 0.6 | 1.6 | 0.0 | 1.0 | 2.5 | 0.15 |

Class overrides: bold metal/precursor roughness -0.1, stone +0.1, wood lobe 1.3; cinematic
metal/precursor -0.15, stone +0.15, wood -0.05 and lobe 1.5. Base: foliage env 0.4, cloth env 0.3,
roughness +0.15.

## Evidence
Captures (2560x1440, MSAA 4, SSAO deep, bloom, actor shadows, FP16 target with clamp tonemap):
`$SANDOVER_DATA/shots/material-response/{off,faithful,bold,cinematic}/`, manifest
`sandover/shots/material-response.yaml`.
Share of pixels changed by more than 8/255 against shading off (mean abs difference in 0-255):

| Shot | faithful | bold | cinematic |
|---|---|---|---|
| village1 oracle noon | 30.3% (5.3) | 46.0% (8.4) | 50.5% (10.0) |
| jungle temple top noon | 37.4% (8.3) | 42.7% (10.6) | 35.8% (8.2) |
| village1 square noon | 18.8% (3.0) | 32.1% (6.3) | 35.7% (8.2) |

Iterations (scratch, low resolution): a first pass let rough stone reflect the sky through the
Fresnel rim (washed, pinkish cliffs and floor in cinematic); fixed by the (1 - r)^2 factor on
dielectric reflection and F90. The jungle temple floor is precursor (bronze metal), so it takes
the metal model, not stone.

Cost (solo, one instance, village1 oracle, whole-frame GPU timer, second 240-frame window):
off 4.72 ms, faithful 4.96, bold 5.02, cinematic 4.96. Shading on costs about 0.25-0.3 ms at
this spot (maps and response together); the looks differ by less than 0.1 ms.

## Parity
Not run (repo rule: milestone check). Every new branch is inside `pbr_background`,
`pbr_generic` and `pbr_merc`, which run only with `pbr_on != 0`; the new uniforms are set only on
the shaded path. The only change outside them is a `T0_pbr` copy of the merc texture sample.

## Limits
- Wind TIE (palm canopies) and sprites are not shaded, so foliage shows little of the model.
- The merc (character) frame's world-up sign for the environment is inferred from the generic
  path (`pc-set-pbr-param 20` flips it); not checked on a character close-up.
- No sky colour table exists in the mood data; the zenith is derived from the fog colour.
- Specular aliasing on high-frequency normal maps is not filtered (no Toksvig yet).

## Follow-ups
- Choose a direction from the comparison; then fold the chosen look into `[global]` and drop the others.
- Shade wind TIE so foliage joins the model.
