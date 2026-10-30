---
title: Why do precursor metal and some trees pierce the fog in neighbour mode, and how should misty keep its haze?
date: 2026-09-24
issue: 174
status: verified (captures, far-band fit) / settled (A/B superseded by the landmarks review)
sha: bc2d45dc4 + branch neighbour-fog
---
## Answer (≤5 lines)
Two background shaders never got the unified fog: `etie.vert` (the additive envmap pass of
precursor metal: `fogginess = 0`, so its shine was added at full strength on top of a fogged
base) and `tfrag3_sun.vert` (tfrag and TIE whenever the moving sun or actor shadows are on:
vanilla fog only). Both now use the same `atmo_fog`; the envmap pass fades to black, not to the
fog colour. Measured from the stand-ins, misty island and geyser rock need about 10-12x the
unified density to keep the haze the artists gave them; `presets/vista.toml` `intent` does that.

## Evidence (path:line@sha / URL, short excerpts)
Audit of the background paths used by the vista (neighbour) draws and the viewer levels:

| path | shader | unified fog before |
|---|---|---|
| tfrag, alpha tfrag (trans/dirt/ice/water) | `tfrag3.vert` | yes |
| tfrag and TIE with sun or actor shadows on | `tfrag3_sun.vert` | **no** |
| TIE | `tfrag3.vert` | yes |
| TIE wind (trees) | `tie_wind.vert` | yes (view depth, camera height) |
| envmapped TIE, base | `etie_base.vert` | yes |
| envmapped TIE, envmap pass (precursor metal) | `etie.vert` | **no** (fog 0 even in vanilla) |
| shrub (alpha-tested foliage) | `shrub.vert` | yes |
| merc, emerc, sprites | `merc2.vert`, `emerc.vert`, `sprite3_3d.*` | yes since the landmark-actors change |
| generic | `generic.vert` | no; draws only near actors of the viewer levels in Jak 1, never neighbours |

- The envmap pass blends `SRC_0_DST_DST` (`process_envmap_draw_mode`,
  `decompiler/level_extractor/extract_tie.cpp`), an add weighted by the base's alpha, so the
  fix scales its colour by `1 - fog` (`etie.frag`, `atmo_fade`) instead of mixing in fog colour.
- Temple top, noon, pre-fix build vs this branch, both unified 150/150/800: 901 px in the misty
  box change by more than 8/255, mean (before - after) = (+11.8, +7.4, +4.6); the orange precursor
  structures lose their unfogged sheen. Foreground: 0 px differ. Dusk: 1062 px, (+10.3, +6.6, +4.0).
- The tree case: the sun path skipped the unified fog for every tfrag/TIE draw once sun or actor
  shadows were on (the play session); rig captures run with both off, so the rig never saw it.

Per-neighbour fog (`pc-set-vista-neighbour-fog level density tint cap`, `gfx.h`
`VistaNeighbourFog`): while the vista draws a listed neighbour, `first_tfrag_draw_setup` scales
the unified density, tints the viewer's fog colour and optionally caps the fog amount.

Far-band fit: mean colour over the pixels where the stand-in or the real island is (the union of
`|off - unified| > 10`), per shot. Vista off = the vanilla stand-in, the target.

| shot / neighbour | stand-in L | unified L | x4 | x8 | x16 | intent L |
|---|---|---|---|---|---|---|
| temple-top noon / misty | 140.7 | 74.8 | 104.4 | 128.5 | 150.0 | 138.2 |
| temple-top dusk / misty | 133.8 | 67.0 | 93.3 | 114.9 | 134.0 | 123.7 |
| temple-top noon / geyser rock | 162.9 | 95.9 | 129.8 | 149.9 | 160.8 | 154.8 |
| temple-top dusk / geyser rock | 146.6 | 89.0 | 118.0 | 135.3 | 144.7 | 139.4 |
| beach-v1 noon / geyser rock | 138.9 | 96.7 | 127.7 | 149.3 | 164.3 | 155.3 |
| beach-v1 dusk / geyser rock | 154.6 | 135.8 | 148.6 | 159.2 | 166.6 | 162.2 |

- The stand-ins are not darker than the real islands; they are hazier (vanilla fog near its
  cap). Matching them needs density, not a darker fog colour. The best scale depends on distance
  (misty x12-16 from the temple top; geyser rock x16 from the temple top but x6 from the beach),
  so one scale per neighbour is a compromise: misty x12 with tint (1.0, 0.97, 0.88) (the
  stand-in is a little less blue than the fog), geyser rock x10.
- `intent` keeps misty slightly darker than its stand-in at dusk (L 124 vs 134), in line with the
  human's "stays darker".

Captures (`$SANDOVER_DATA/shots/neighbour-fog/`, manifest `sandover/shots/neighbours.yaml`,
2560x1440 MSAA 4, Jak hidden, effects/wind/particles off): `off` (vista off, stand-ins), `before`
(pre-fix build, unified), `unified`, `intent`, `sweep-d4/8/16`; numbers in `far-band.json`.
New shots `temple-top-noon/dusk`: on the jungle temple top (next to the `towertop` actor),
facing between misty island and geyser rock.

Rig: every staged draw logged an NVIDIA "shadow sampler on a non-depth texture" warning while
actor shadows were off (the `tex_T12..14` samplers pointed at empty units); neighbour mode drew
enough to write ~16k lines/s and the village1 and beach shots timed out. `actor_shadows_set_uniforms`
now binds a 1x1 depth texture to those units when shadows are off.

Comparison: A stand-ins, anchor; B unified; C intent, at the temple top, hut roof and beach-v1 at
noon and dusk.

Parity (vista off): `sandover-parity check --set auto-v0`, build p4nf-2, PASS 16/16, worst FLIP mean 0.00198 (threshold 0.011399). The two dawn shots that timed out under a shared GPU were recaptured alone into the same set.

## Implications for the design
- Any shader added to the background set must carry `atmo`; the sun path is easy to miss because
  the rig captures with the sun off.
- Distant islands that the artists hid in haze want a per-neighbour density; a distance-aware fit
  (density per neighbour and per viewer level) would match every viewpoint, if wanted.
- Landmark actors of a neighbour do not see the per-neighbour override yet.

## Follow-ups
- Settled: the A/B comparison was superseded by the later landmarks review, which chose real neighbour levels with unified fog, haze and the Misty/basin fits.
- Per-viewer-level override if one scale per neighbour is not enough.
- Generic foreground fog if Jak 1 ever draws neighbour actors through generic.
