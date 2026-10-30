---
title: Screen-space contact shadows and parallax self-shadow
date: 2026-10-18
status: ready for review (rig proof 2026-10-22; strength rounds not run)
sha: branches feat/contact-shadows (on master 7cc714ec9), feat/pom-shadow (on feat/contact-shadows)
---
## Answer (≤5 lines)
Both features render and are glance-visible at close range. Contact shadows darken the sunlit
contacts the 4096 world map misses (rocks on sand, Jak's limbs) by about a quarter of the
luminance there at low sun; parallax self-shadow darkens the relief pits of masonry facing away
from the sun by about a third. Off, and under vanilla rendering, the frame is unchanged.

## Settings
- Contact shadows: one on/off row in Options > Sandover (text id 0x5a64), off by default and in
  the boot state; on sends option 41 at the strong level. Greyed with vanilla rendering, with
  actor shadows off and without temporal AA (FSR 3.1 or DLSS); `sandover-effective!` forces it
  off where temporal AA does not run.
- Parallax self-shadow: no row. It runs wherever parallax runs (Materials on, pbr-param 22), at
  the strong level (engine default of option 43).
- Strengths were set without a strength round: on = strong for both. Options 41 and 43 keep
  1 (moderate) and 2 (strong) for an A/B.

## Proof (2026-10-22; 4K, play look, FSR 3.1 at 100 %, one instance)
Captures: `$SANDOVER_DATA/shots/ss-shadows-422-proof/`; manifests and logs
`$SANDOVER_DATA/runs/20261022-ss-shadows/`.
- Probes: `probe-cs-beach-factor` (option 42 3, factor view): 21,846 px below 0.9, mean factor
  0.76, on Jak and the rocks, none on open sand; `probe-pom-wall-on` vs `pom-wall-off`: 14.6 % of
  the frame darker by more than 8/255.
- Contact shadows, beach-start 07:46 (low sun, Jak, rocks, chest), pixels where the factor view is
  below 0.9: off RGB (90.5, 70.8, 53.6) -> on (69.1, 53.7, 41.1), luminance 73.7 -> 56.1 (-24 %);
  where it is below 0.7: (89.4, 69.7, 52.3) -> (55.9, 42.4, 32.5). Crop
  `crop-cs-beach-jak-off-on.png`. Jungle ledge at noon (Jak, foliage): 6.8 % of the frame darker by
  more than 8/255, there (92.5, 162.6, 173.5) -> (71.3, 132.3, 142.8); a similar share is lighter,
  which is foliage and water motion between shots, not the term (it never lightens).
- Parallax self-shadow, village1 stone wall at noon: 13.9 % of the frame darker by more than
  8/255, there (67.8, 63.7, 51.6) -> (44.9, 42.8, 35.4); 0.2 % lighter. Crop
  `crop-pom-wall-off-on.png`. Precursor floor inside the jungle stonehenge: no change (0.001 %), as
  expected: no sun reaches it and the term only removes the sun share.
- Settings row, live in one session: `row-cs-on` (field set, option 41 untouched) matches the
  option-41-strong frame (0.37 % of the lower frame differs by more than 8, mean 3.0 in the hit
  mask, from water and foliage motion); in the hit mask row off (87.9, 68.2, 51.8) -> row on
  (68.0, 52.9, 40.7).
- Vanilla rendering with contact shadows on and parallax self-shadow at its default vs a plain
  vanilla boot of the same shot: 0 of 5,094,000 static pixels differ (sky, cliff, sand, rocks,
  chest); only the water (wave clock) and Jak's idle pose differ between the two sessions.
- DLSS: the pass now takes the DLSS frame flag too (it took only FSR 3.1's, so under DLSS the row
  was on and nothing drew). Code read only: DLSS does not run on this rig.

## Evidence (4K, FSR 3 native, play look)
- Parallax strong, village1 stone wall at 2 m: relief pits shade toward the sun, no seams seen
  (smoke/stonewall-px vs -off).
- Contact hit view: hits on grass-clump bases, fountain blocks, Jak's body (arm, straps, legs);
  none on flat ground (no acne); at 22:29 the moon direction drives the rays; indoors the share
  view goes to 0 under the roof (world map), so the interior is untouched.
- First smoke hit nothing: the 3x3 clip matrix's w was pzw x the view depth; it is now divided
  by |pzw| so w is the view depth in game units (the SSAO copy x 4096).
- Visible change, strong vs off (pixels darker by > 8/255; frames also differ by animation and
  water): door 0.3 %, square 2.6 %, jungle 7.3 %, beach 15 % (mostly water), village1 edge 3 %.

## Temporal convergence (contact shadows)
FSR and DLSS reject high-variance history, so they do not denoise the dithered term. Raw term
into R16F; temporal pass into `ScreenHistory` (RGBA16F: term, view depth m, frames), camera
reprojection with the previous frame's 3x3 clip matrix and position, disocclusion when the
stored depth differs from the expected one by more than 2 % + 3 cm, history clamped to the raw
3x3 min/max +-0.02, blend 1/(n+1) with n up to 16, reset on the motion-vector cut flag, a frame
gap or a resize; composite = depth-aware 3x3 of the history, multiply blend, stencil bit 7 clear.

## Design: contact shadows (`ContactShadows.{h,cpp}`, `ScreenHistory.{h,cpp}`, shaders `contact_shadow*.frag`, `screen_history.glsl`)
- Where: three full-screen passes right after `SsaoRenderer::render` (bucket 56: opaque
  background, alpha tfrag and characters are in the depth copy), before the swamp mist. The
  composite multiplies (`DST_COLOR, ZERO`), destination alpha untouched, stencil bit 7 clear only
  (no sky, far ocean, translucent actors). Debug views (option 42) replace the colour.
- Inputs: the SSAO camera-space depth (`csz`, metres) and normal target via
  `SsaoRenderer::bind_view_inputs`, the emissive mask getters, the world and actor sun map
  receiver uniforms (world space). No SSAO, upscaler or motion-vector file changes.
- Geometry: camera offset (world axes, game units) to clip x, y, w is the 3x3 of
  `make_new_cam_mat` (MotionVectors' `cam_to_clip` rows x, y, w carry no translation), divided by
  |pzw|; `w = csz x 4096`. Position `P = K^-1 (ndc w, w)`; the normal target's clip-space normal
  maps to camera space by `K^T`, else the depth face normal.
- Ray: toward the world map's light (`world_shadow.sun_world`: the sun by day, the moon at night,
  one light and fade for every consumer). Start offset along the normal by 2 cm + 0.4 % of the
  distance; length 0.8 m (moderate) or 1.5 m (strong) near the camera, x1-3 from 20 to 60 m,
  capped at 8 % (moderate) or 12 % (strong) of the frame height on screen. Taps: 16 (8 at shadow quality low), first tap at
  a dithered fraction (interleaved gradient noise rotated per frame, 64-frame cycle). Hit: the
  depth sample is in front of the ray by more than 1 cm + 0.2 % and less than the thickness
  (0.3 / 0.45 m, + 2 % per metre of distance); weight `1 - t^2` (end fade) times a fade toward the
  thickness limit. Faded on grazing light (N.L 0.02-0.15) and with distance (60-120 m).
- Direct share: `r = s N.L (1 - occ_map) / (s N.L (1 - occ_map) + 1 - s)`, `s` the fitted sun
  share of the live palette at N.L = 1 (`ActorShadowFrame::share_global`), `occ_map = max(actor
  sun map, 1 - world map)`. Term `strength x fade x hit x r`; strength 0.7 / 0.95 times the
  light's fade and the shadow strength option. Fog and haze: `t^fog_k` of the SSAO normal
  target's fog transmission and the ground mist, as the SSAO composite; emissive pixels by the
  SSAO E ramp. It never lightens and never touches the baked ambient share.
- Gates: option 41 > 0, SSAO frame, actor shadows and world map active with a light fade > 0,
  not stereo or XR, and the temporal AA path this frame (`m_fsr3_frame || m_dlss_frame`); otherwise
  no GL work, and under a non-temporal mode one log line.

## Design: parallax self-shadow (`pbr_parallax_shadow` in `shaders/pbr_bg_frag.glsl`)
- After the parallax offset finds the surface point: march toward `pbr_sun_dir` in the same basis
  the offset used (per-vertex `pbr_pxn/pxu/pxv` where the tree has it), 8 taps from the
  point to where the ray leaves the top of the field (at most four depths along the surface);
  occlusion = the largest height of the field above the ray over `softness x depth`, damped over
  the march; `pbr_px_sh = 1 - amount x occ`.
- Applied in `pbr_respond`: the relief term loses `(1 - pbr_px_sh)` times the sun part of the
  light at the fragment: the bake's direct share `pbr_px_sun` (`pbr_ksun` x N.L at the sidecar
  normal over the baked light's luminance, set in `pbr_bg_frag.glsl`; `sv` x N.L where the
  sidecar has no record) plus the relief's own sun term; `sv` (lobe, transmission) is scaled by
  it; untouched when `pbr_px_sh` is 1 (merc never sets it). Until 2026-10-22 it took `sv`, the
  share at N.L = 1, whatever the N.L (see "Fix: blotches under a grazing light").
- Background only (`set_class_uniforms(merc = false)`); no dithering, so no temporal pass needed.
- Settings: no row of its own; it runs wherever parallax runs, at the strong level (engine
  default). Option 43 stays for an A/B.

## Fix: blotches under a grazing light (2026-10-22)
- Report (play, jungle cliff by the waterfall, hours 20.5 and 22.2, moonlight): "the POM shadows
  on this cliff look completely bugged and broken": hard black blotches in the rock's pits, in
  patches bounded by triangle edges, faces next to them untouched.
- Elimination, rig re-shoots of the snapshots (4K, play look, one instance): option 43 = 0
  removes every blotch, 1 keeps them weaker, 2 is the report; at noon the same camera shows mild
  pits. Debug views over the darkened pixels: N.L of the cliff to the moon 0.18 (p10 0.04, so
  many faces sit at the 0.02 cutoff), `sv` 0.74, the bake's actual direct share
  (`pbr_ksun` x N.L / lum(baked)) 0.13. The village1 wall at noon: N.L 0.20, `sv` 0.58, share 0.19.
- Mechanism: the self-shadow removed `sv`, the sun share at N.L = 1 (doubled and clamped in the
  vertex shader), not the share the bake holds at this N.L. Under a grazing light that is 3-5x the
  sun actually in the pixel, so occluded pits lost most of their ambient light too and went near
  black; and faces either side of the `ndl <= 0.02` early-out switched between full removal and
  none, which drew the triangle-shaped patches. Ruled out: the march itself (tmax, ray leaving the
  field), the tangent basis (shadows sit in the right relief, only their depth was wrong), the
  height scale (one global depth, the same at noon), the moon's colour/strength (the share is the
  night slot's own fit).
- Fix: remove the bake's direct share at the fragment (above). Arms tried on the same frames:
  `sv` x N.L (close, wall p5 ratio 0.93), the bake's share (chosen, 0.88), a contact-shadow-style
  N.L fade on the old share (cliff still 4 % of pixels below a quarter of off).
- Proof (cliff region x 192-2600, y 0-1075, waterfall mist excluded; o2 against o0 of one build):
  0002 (20.5 h): pixels kept below 25 % of off 3.7 % -> 0.0 %, darker by > 8/255 11.5 % -> 0.8 %,
  p5 ratio 0.30 -> 0.95; 0004 (22.2 h): 7.5 % -> 0.0 %, 20.0 % -> 3.8 %, p5 0.16 -> 0.82; noon
  control: 0.9 % -> 0.0 %, 5.8 % -> 2.3 %, in its darker mask (59.6 -> 34.7) -> (75.9 -> 54.6).
  Repeat noise of the same shot: 0.1 %. Captures `$SANDOVER_DATA/shots/pom-shadow-cliff/`
  (`before/`, `after/`, `exp/` arms and debug views 13/14 from an uncommitted shader, crops
  `crop-*-off-before-after.png`: off, before, after); logs `$SANDOVER_DATA/runs/pom-shadow-cliff-*.log`.
- Review follow-ups: the term fades in over N.L 0.02-0.1 (the relief's sun term did not reach 0 at
  the early-out, so triangle edges still stepped), and a pixel under a cast shadow keeps the
  self-shadow off in proportion (`pbr_sun_occ`), since the map already took the direct share.
  Re-shot (`after2/`): 0004 darker by > 8/255 3.8 % -> 1.7 %, p5 0.82 -> 0.93, none below 25 %;
  noon cliff inside the world map's shadow (shadow view, 23 % of the region): darker 9.5 % (master)
  -> 2.3 % -> 1.4 %, p5 0.44 -> 0.92 -> 0.97; the lit part unchanged (2.3 %, p5 0.96).
- Consequence: the term is subtler where the bake's sun share is small. The village1 wall at noon
  went from 14.7 % of the frame darker by > 8/255 (pits at 0.63 of off) to 3.7 % (0.82): the earlier
  proof numbers above were measured with the over-removal. The pits now keep the bake's ambient.

## Parameters per strength
| Feature | Strength | Values |
|---|---|---|
| contact | moderate | 16 taps, 0.8 m, thickness 0.35 m, strength 0.8, screen cap 8 %, history 16 |
| contact | strong (row on) | 16 taps, 1.5 m, thickness 0.5 m, strength 1.0, screen cap 12 %, history 16 |
| parallax | moderate | 8 taps, amount 0.7, height x1.0, softness 0.3 |
| parallax | strong (default) | 8 taps, amount 1.0, height x1.6, softness 0.15 |

## Open items
- Contact shadows are subtle with a 4096 world map; the map already holds most contacts.
- No strength round: both features use the strong level when on; moderate stays on the options.
- The contact shadow history has no live pan or Jak-run proof yet (stills only).
- The direct share is a global estimate (fitted sun share at N.L after the maps), not the
  sidecar's per-vertex share: where the bake is in shadow but the maps are lit, a contact can
  darken baked shadow a little.
- Cost: measured in the combined perf pass after merge (GPU timer group `contact-shadows`).
