---
title: Tile and world lights shaded with the vertex normal, so the floor relief vanished under them
date: 2026-10-22
issue: play capture runs/20261022-1234-play/0006 (maincave, 12.3 h)
status: verified
sha: ed0917a7b (base), fix/tile-light-relief
---
## Answer (≤5 lines)
The tile-light pass (`emissive_light.frag`) is deferred. It shaded every screen tile light and
every world light (the mushroom glow props, eco shots) with the normal target: the sidecar's smooth
vertex normal, or the face normal from screen derivatives. It never saw the material's mapped
normal, so the parallax-shifted normal map was missing too. The sun and key light get their relief
in `pbr_respond` from `Nd`. Fix: background material draws write `Nd` into a small detail target,
and the tile pass shades with it through the same relief term. Relief under the light (normalised
local contrast of the added light) goes from 0.24 to 0.35 at the reported spot. Nothing changes
where the tile lights add no light.

## Symptom
Play snapshot 0006 (maincave at 12.3 h, crop `crops/0006-a1.png`): "Why is the light not using the
normals/parallax effects? The cave floor looks perfectly smooth. I think this also applies to
yellow eco shots in the swamp level." The green light is the cave mushrooms' glow (glow props in
the world light table, plus their screen tiles). It lights the floor and wall with a smooth
gradient, while the same floor shows its relief under the baked light around it.

## Mechanism
- Which normal: `emissive_light.frag` `targetNormal()` reads the normal target (colour attachment 1,
  `kb/howto/ssao.md` "Normal target"). Tfrag and tie at LOD 0 write the `.unbake` sidecar's smooth,
  area-weighted vertex normal there. Shrub, wind tie, envmapped tie and generic write the face
  normal from screen derivatives, and the tile pass falls back to depth reconstruction where the
  target is empty. `tileLight` and `worldLight` take `wrapped N.L` and the GGX highlight on that
  normal. The position is the depth copy, so it is the geometric surface; parallax only shifts UVs.
  So the pass sees neither the normal map nor the parallax-shifted normal.
- The reference: `pbr_background` builds `Nd` with `pbr_uv_detail` (normal map at `uv +
  pbr_uv_shift`, i.e. at the parallax-displaced texel), and `pbr_respond` adds the sun and key
  relief as `rg x (N(Nd).L - N(Ng).L)` on top of the bake, with `rg = 1 + 2 x ambient` (2 in the
  default bold look). The lobe uses `Nd` and the map roughness.
- Why not write `Nd` into the normal target: AO (`ssao_ao.frag`) and the contact shadows read the
  same target. AO's horizon test treats shallow samples whose smooth normal matches the pixel's
  as the same surface. A per-pixel mapped normal would break that test and change AO everywhere,
  including in sunlit areas. The vertex normal has to stay there.

## Fix
- `ssao_normal_frag.glsl`: a second output, draw buffer 6 (`GL_RG8`, the frame's sample count).
  It holds `Nd`'s tangential part around the shading normal in a frame built in the tile pass's
  view space (`ssao_dn_frame`, singular only for a normal facing away from the camera). Every
  normal-target writer clears it to "unperturbed". `pbr_background` sets it from its final `Nd`,
  after the wet-film and puddle flattening.
- `SsaoRenderer`: attaches, clears and masks it with the normal target only while the tile lights
  and the background material shading are on. `Materials.cpp` sends the world-to-view rotation
  (`view_sign x camera_rot^T`, the tile pass's own).
- `emissive_light.frag`: rebuilds `Nd` per sample around the target's normal. The diffuse term is
  `max(wrap(N.L) + relief x (wrap(Nd.L) - wrap(N.L)), 0)` with the pass's `kWrap` (0.5), the same
  term and gain as the sun's relief in `pbr_respond` (whose wrap is per class). An earlier draft added `relief x (Nd.L -
  N.L)` unwrapped: 1.5x the sun's term and unclamped past the terminator. The highlight
  lobes use `Nd` and the material roughness that the emissive mask already carried.
- No new setting. The tile lights stay on their own row. With the material shading off, or on
  draws without a mapped normal (merc, generic, unshaded classes), the detail is zero and the pass
  computes exactly what it did before.

## Proof
Rig, one instance at a time, 3840x2160. Master = the master build with master shaders. Fix = this
branch. Off = the same build with the tile lights off (`pc-set-emissive-lights 0 0`). Added light =
linear frame minus off. Region: pixels where master's blurred added luma is above 25% of its 99th
percentile, in the lower frame. `rel_hp` = mean |added - blur6(added)| / mean added, `grad` = mean
|grad added|^2 / mean added^2 (normalised gradient energy).

| spot | region px | added mean (master / fix) | rel_hp | grad | image local contrast (RMS) |
|---|---|---|---|---|---|
| maincave mushrooms, `from-snapshot 0006` | 156k | 0.0751 / 0.0757 | 0.244 -> 0.347 | 0.138 -> 0.265 | 0.042 -> 0.054 |
| swamp-start, Jak's yellow eco shot (draft term) | 291k | 0.413 / 0.395 | 0.146 -> 0.208 | 0.032 -> 0.050 | 0.115 -> 0.131 |

The maincave row is the final term. The swamp row was measured with the earlier unwrapped draft
(1.5x the sun's relief, 0.253 -> 0.466 / 0.142 -> 0.431 at maincave) and was not re-shot; expect
its gain to shrink by a similar share. Metrics on linearised PNG luma (Rec. 709), `blur6` a
Gaussian with sigma 6 px, local contrast the RMS of luma minus `blur6(luma)` in the region.

The light's mean power is unchanged within 1% (maincave) and 4% (swamp). Only its texture changes: the cave floor's cracks
and the swamp grass and rock relief now show under the light as they do under the sun.

Parity:
- Tile lights off, master vs fix. Maincave: max 20, 0.009% of pixels > 2/255. Swamp: 1.3% > 2,
  all on the eco shot's own sprite and Jak's pose, which vary from run to run in the frozen
  3-frame spawn; the static scene is identical.
- Tile lights on, outside the dilated lit region. Maincave: max 5, 0.002% > 2. Swamp: 0.3% > 2,
  the same sprite and pose. So the sunlit and baked look is unchanged.
- Vanilla is not touched. With the tile lights off, nothing is allocated or attached: `ssao_dn` is 0
  and no shader writes location 6 to a bound buffer.

Frames: `$SANDOVER_DATA/runs/tile-light-relief/shots3/` (final maincave pair; the off frames and
region mask are shots2's), `shots2/` (`0006-{before,after,off-*}` of the draft,
`swamp-*/swamp-yellow-shot`, crops `ab-cave.png`, `ab-swamp.png`, metric masks `m-cave/`,
`m-swamp/`). Swamp manifest: `runs/tile-light-relief/swamp-eco.yaml`.

## Cost
Not measured cleanly yet. One solo pair at maincave-crystals and swamp (base frames,
4K) ran while other builds and a gk were active and is inside the noise: tfrag 2.24 -> 2.11 and
2.81 -> 3.10 ms, emlights-light 0.15 -> 0.16 ms at swamp. To finish: stage `bin/<fix>` and
`bin/<master>` (the master C++ build; the other changes are shader and preset only), give one project root
the master or fix shader set, and run `sandover-perf --only maincave-crystals swamp --no-toggles
--frames 900 --bin <...> --proj <root>` as A/B/A/B with no other gk and no build running. Expected
cost: one RG8 colour write per background sample (half the normal target's bytes), plus two
texel fetches and a frame build per sample in the tile pass.

## Limits
- The parallax self-shadow (`pbr_parallax_shadow`) marches toward the sun only. Tile and world
  lights get the parallax-shifted normal but no height-field self-shadow. That would take the
  height per pixel and a march per light in the deferred pass.
- Characters (merc) keep the vertex normal under the tile pass. They take the world lights from
  ActorShadows' local lights instead, so only screen tiles light them here.
