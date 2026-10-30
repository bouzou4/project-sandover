---
title: SDR colour against vanilla, per display-transform stage
date: 2026-10-22
issue: none (user requirement 2026-10-22: SDR colours like vanilla, physically lit, HDR enhanced)
status: verified
sha: 7cc714ec9 (master) + this branch
---
## Answer (≤5 lines)
Below game white the display transform is the identity in SDR and HDR alike (toon knee 0.95, exposure 1,
no grade). The one display-transform drift was toon's handling of pixels with a channel just above 1: it
desaturated them toward luminance and divided by the max channel, where vanilla's RGBA8 target clips per
channel. That turned the noon sky violet (+14.8 deg) and greyed sunlit grass. Fix: per-channel clip after the
luminance shoulder. Every other drift measured traces to a picked feature (PBR look, water, eco lights, mist).

## Method
- Frames: `sandover/shots/sdr-colour.yaml`, 8 cameras matching vanilla library frames (village1 noon, beach
  dusk, jungle noon, sunken night, misty noon, rolling noon, blue vent noon, green vent night). Remaster shot
  with `--play-look` (temporal AA, FSR 3.1 native, bold, toon), one instance, 4K (under temporal AA a screenshot
  at another size than the frame resets the history and misses the background), compared at 2560x1440.
- `python -m sandover.sdr_colour measure|attribute|stages|sheet`: Oklab medians over hand-placed regions of
  content both frames share (sky, sea, grass, rock, skin, eco...); hue shift in degrees, chroma and L ratios,
  dE x100. `attribute` applies each curve to the shot's EXR (the tonemap input): clamp is vanilla's clip, so
  clamp against vanilla is the scene drift and toon against clamp is what the display transform adds.
- Data: `$SANDOVER_DATA/shots/sdr-colour-proof/` (vanilla copies, before, after, toggle-*, sheet.png, JSON).

## Symptom
Before (master): village1 sky hue +14.8 deg, chroma x1.11, L x0.96; jungle bridge planks -10.6 deg, chroma
x0.87; blue vent grass chroma x0.89; beach dusk sky -2.4 deg, chroma x1.04.

## Mechanism
- The EXR clamped per channel matches vanilla in those regions (village1 sky +0.3 deg, chroma 1.00, L 1.00):
  the scene is right, the transform is not. Sky median EXR 0.41, 0.76, 1.13; 31% of village1's pixels and 31%
  of the blue vent frame have a channel over 1, mostly under the halo ramp start (1.2).
- `tonemap_toon` (tonemap_sdr.glsl, included by post_processing.frag): luminance below the knee, so no
  shoulder; then for a max channel over 1 `mix(lt, c, max((1 - lt) / (m - lt), 0.75))` and `c /= max`: red rises, green falls (sky 0.45, 0.71, 1.0
  against vanilla 0.41, 0.77, 1.0). The halo ramp already gave vanilla's clip from 1.2 to 1.8 and the
  core ramp above; the band between 1.0 and 1.2 was the leftover.
- Toggle alone (`toggle-clamp`, play look + `(pc-set-tonemap 0 0)`): village1 sky +0.2 deg, dE 0.16; bridge
  -2.3 deg.

## Stages measured
| Stage | Drift below game white | Notes |
|---|---|---|
| Tonemap toon, luminance shoulder | none below 0.95 encoded luminance; 0.95..1 compresses to at most 0.975 | the SDR room for the shoulder; HDR decode differs from SDR only there (max 5.8 levels, mean 0.001) |
| Toon per-channel step (1.0 to 1.2 max channel) | sky +14.8 deg, grass chroma x0.89 | fixed here |
| Exposure / white point | none (exposure 1, knee 0.95, paper white only scales HDR) | |
| sRGB / gamma | none: SDR writes the encoded value, HDR decodes with 2.2 and the SDR panel does the same | |
| Brightness/contrast | identity at defaults (mult 1, add 0) | SDR applies it after the tonemap, HDR before the decode; same result below white |
| Colour grade | none in the final pass | `hdr_saturation` 1.0 is HDR-only |

## Fix and proof
`c = min(c, vec3(1.0));` after the luminance shoulder (tonemap_sdr.glsl; `hdr.py` `_toon` and its
tests follow). HDR output is untouched: `hdr_map` keeps those channels above paper white, so SDR is vanilla's clip
and HDR the extended range above game white. Re-shot (`after/`):

| Region | Before (hue, chroma, L, dE) | After |
|---|---|---|
| village1 sky | +14.8, 1.11, 0.96, 4.2 | +0.2, 1.00, 1.00, 0.2 |
| beach dusk sky | -2.4, 1.04, 1.00, 0.6 | -0.2, 1.00, 1.00, 0.1 |
| jungle bridge | -10.6, 0.87, 0.96, 3.8 | -2.1, 0.96, 0.96, 3.4 |
| jungle boulders | -3.7, 0.90, 0.92, 5.7 | -2.4, 0.93, 0.92, 5.6 |
| blue vent grass | -1.2, 0.89, 1.05, 4.8 | -2.5, 0.99, 1.04, 4.1 |
| green vent eco | -3.3, 0.97, 1.01, 1.9 | -1.8, 0.98, 1.00, 0.8 |
| all 32 regions (mean abs hue, geo-mean chroma, L, mean dE) | 7.4, 0.854, 0.947, 4.6 | 6.6, 0.861, 0.949, 4.3 |

Re-shot after the shader dedupe (`shots/sdr-colour-merge/`, one bin, the root's `tonemap_sdr.glsl` swapped
between master's and this one): village1 sky +14.8 deg, dE 4.20 -> +0.2, 0.20; jungle bridge -10.7 -> -5.3 deg,
chroma 0.87 -> 0.96; blue vent grass chroma 0.90 -> 0.99. Each PNG matches its own curve on its EXR to 0.34
levels in the 1.0-1.2 band (the other curve: 19-31 levels); pixels at max channel 1.8 or more follow both
curves alike (0.03-0.17 levels), so cores and halos are unchanged.

HDR check (offline `hdr_map` port on each EXR, paper white 200, decoded and re-encoded at 2.2): below game
white it equals the SDR output, mean 0.001 levels on every frame.

## Drift that is a pick, not a bug (toggled alone, new shader)
- PBR look (bold), `pbr?` off: sunken precursor walls chroma x0.67, L x0.73 -> 0.98, 0.98; sunken door +12.7
  deg -> -1.4; jungle river -18.7 deg, chroma 0.80 -> -3.4 deg, 1.16; misty ground -13.2 -> -7.7 deg.
- Water, `water?` off: beach dusk sea +15.6 deg, L x1.29 -> -0.8, 1.03; village1 sea +7.9 -> +0.3.
- Eco lights: green vent stone lit green (hue +106 deg; vanilla eco lights nothing, the remaster adds the
  light eco casts by design). Unchanged by either toggle.
- Not toggled (shadows, AO): Jak's shirt L x0.61, beach dusk sand L x0.76, jungle leaves L x0.85.

## Ruled out on the way
- Rig: a worktree root without `third-party/fidelityfx` falls back to vanilla rendering (`fsr3: unavailable
  ... headers missing`); a 2560x1440 screenshot of a 4K temporal frame loses the background. Both noted in
  `howto/rig-capture.md`.
