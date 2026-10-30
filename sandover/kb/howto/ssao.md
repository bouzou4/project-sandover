# SSAO how-to (Jak 1)

Screen-space ambient occlusion. Off by default; Jak 1 only. The kernel is GTAO (a port of Intel's
XeGTAO, below; the SAO kernel of the 2026-09 rounds is deleted). `sandover/presets/ssao.toml` holds
the four ambient occlusion quality tiers of the settings menu in their order, which is also the
`(pc-set-ssao-preset n)` index: 0 `deep-low`, 1 `deep` (medium, the default and the play look,
tuned to read as the SAO deep pick), 2 `deep-high`, 3 `deep-ultra`.
Results and costs: `kb/findings/2026-09-23-ssao.md` (pipeline, SAO), `kb/findings/2026-10-18-gtao.md`.

## Turn it on

- ImGui: Renderer Debug → **SSAO**: enable, candidate preset, debug view (raw AO, blurred
  AO, normals AO uses, depth after bucket 56, stencil mask, normals from depth, normal
  source, emissive E), the normal-target switch, the emissive-mask switch and ramp, GPU
  timers, and a dev-only parameter tree. Renderer Debug → **Bloom** holds the bloom controls.
- GOAL (Jak 1 kernel symbols):
  - `(pc-set-ssao-preset n)`: on, with built-in preset `n` in TOML order. `n < 0` turns it off; an index past the table logs an error and changes nothing.
  - `(pc-set-ssao-param which millionths)`: 0 enable, 1 radius (m), 2 intensity,
    6 merc weight, 7 luma floor (u8), 8 cave scale,
    10 debug view (0-9; 1 raw AO, 2 denoised AO, 9 fog transmission), 11 GPU timing,
    14 normal target (default on; 0 = normals from depth, the pre-target behaviour),
    15 emissive mask (default on; 0 = ignore E, the pre-mask behaviour), 16 and 17 the E ramp
    (defaults 0.1 and 0.4), 18 fog weight: AO strength is scaled by (fog x mist
    transmission)^weight, default 1; 0 = AO multiplies fogged pixels at full strength, the
    pre-fix behaviour; 2 = a stronger fade. 19 GTAO slices, 20 steps, 21 full resolution (0/1),
    22 thin occluder compensation: preset fields. 23 denoise passes after the history (1-3, default
    2), 24 AO history (default on): not in presets. 3-5, 9, 12 and 13 were SAO's and are gone.
    Setting a parameter 1-2, 6-8 or 19-22 marks the preset "custom".

## GTAO

`shaders/ssao_ao.frag` is XeGTAO's main pass (MIT, licence in the file) and `ssao_denoise.frag`
its edge-aware denoiser; the depth copy, z mips, normal target, stencil, emissive mask, fog fade,
composite and the tile lights' inputs are the pipeline's. Design, numbers and open items:
`kb/findings/2026-10-18-gtao.md`.

- Tiers (preset fields `gtao_slices`, `gtao_steps`, `gtao_full_res`, `gtao_thin`): low 1 slice x
  2 steps, medium 2 x 2, high 2 x 3, all at half resolution; ultra 3 x 3 at full resolution (AO
  targets, denoise and history per full-res pixel; the composite skips its upsample). `radius` is
  XeGTAO's effect radius (times its 1.457 multiplier); `gtao_thin` its thickness heuristic.
- Response: the AO target holds linear visibility; the composite darkens by `intensity x
  occlusion^0.5` (occlusion = 1 - visibility), the shape of SAO deep's `sqrt` response, capped at
  0.8 including the character weight (`max_darkening`). XeGTAO's own `visibility^2.2` with a 0.03
  floor at strength 1 drove creases near black (2026-10-21). Views 1 and 2 show `1 -
  occlusion^0.5`, the composite at strength 1, so they compare with the old SAO views.
- Upsample (half-resolution tiers): joint bilateral over the four half-res texels (distance to
  each texel's kept pixel x a depth weight), nearest in depth where none matches.
- Exclusion: before the AO pass a stencil-tested draw writes an R8 mask from the render target's
  depth-stencil texture (bit 7, sky, far ocean, translucent actors: 1; characters, bit 6: 0.5).
  A texel any of whose samples is excluded skips the kernel and is stored negative; the history
  and the denoiser skip it, and the composite treats it as no AO (it gates on the stencil anyway).
  Seabed under clear inland water is not excluded (its depth is in the capture).
- Stencil marks (`SsaoRenderer::set_bucket_stencil`, `StencilMarks.h`): bit 7 excludes (sky, far
  and mid ocean, translucent actors 45-46), bit 6 marks characters (48-56), bit 5 merc level props
  with local lights (tile lights only). Buckets 0-30 write no stencil: a stencil write before the
  heavy depth-tested background made every later background draw about twice as slow
  (`kb/findings/2026-10-23-ao-prerequisite-cost.md`). Their marks are recovered after bucket 30
  (vista and sun disc drawn) by one full-screen pass per source at depth `EQUAL`: sky and far ocean
  where the depth still equals its copy after bucket 4 (only if buckets 0-4 drew a sample), the
  deferred mid ocean and the merc prop buckets (10, 17) where it still equals the copy after them
  and they changed it. Copies are taken only for the sources drawn (perf group `ssao-marks`; the
  prop copies count in `merc`). The later buckets (31-56) write as before.
  `(pc-set-perf-opt ...)` without bit 5 restores the per-bucket writes; probe
  `(pc-set-ssao-option 4 16384)` keeps them, writes the late marks into scratch bits 0-1 and logs
  per frame the samples where the two differ (`SSAO-MARKS ... sky7_old_only= sky7_late_only=
  prop_old_only= prop_late_only=`), then clears the scratch bits.
- Noise and history: with motion vectors (the temporal AA path) the noise index is `frame % 64`
  (XeGTAO's Hilbert + R2 pattern), and `TemporalAccum` keeps an AO history at the AO resolution:
  reprojected with the frame camera or the merc object velocity with this frame's and the
  history's projection jitter removed, rejected on clip-w mismatch
  (disocclusion), clipped to this frame's AO around the texel (3x3 mean +- 1.5 std devs, inside
  the 3x3 min/max; occlusion a moved occluder cast does not linger), reset on motion-vector cuts,
  blend 0.9 static and 0.6 on characters; then two denoise passes. `(pc-set-ssao-param 24 0)`
  turns the history off (the temporal AA alone resolves the noise; A/B only);
  `(pc-set-ssao-param 25 0)` drops the clip and `(pc-set-ssao-param 26 <std devs x 1e6>)` sets its
  width, 0 for the plain min/max (dev A/B). Without motion vectors (stereo, XR, upstream rendering) a fixed pattern and
  three denoise passes, logged once (`SSAO: no motion vectors`). Every tier runs there as saved.
- Facets: each slice's horizons start at the depth face plane (not only the smooth normal's
  hemisphere) and visibility is divided by that facet's own unoccluded integral, so a flat facet
  under a smooth normal does not shade itself; shallow samples with the same smooth normal are
  ignored as in SAO.
- Renderer Debug -> SSAO: the preset combo, the preset fields under "Parameters (dev)", and the
  XeGTAO constants and history switch (dev only). The GPU timer "blur" covers history and denoise.

## Normal target

While SSAO is on, the render framebuffer gets a second colour attachment, `GL_RGB10_A2` with
the frame's sample count (about 59 MB at 2560×1440 MSAA 4). Buckets 5-30 (opaque
background) and 48-56 (characters) draw with it as draw buffer 1; every other bucket has
draw buffer 1 set to `GL_NONE`. It is cleared to zero before bucket 0 of each dispatch
(each stereo eye).

- Content: the unit surface normal in clip `(x, y, w)` space, stored `n * 0.5 + 0.5`; all
  zero means "no normal". The AO pass maps it to view space with the projection's x and y
  scales (`nrm_scale`), so no pass needs a view matrix. With fog weight on, a fogged pixel's
  normal is stored shortened to length `0.1 + 0.9 t`, `t` the share of the surface colour its fog
  and haze left; readers normalise, and the composite scales AO by `t` times the transmission of
  the ground mist drawn after it (the swamp's, or Misty Island's sea mist; `swamp_mist.glsl`,
  shared with the mist pass), raised to parameter 18; view 9 shows the product.
  Alpha carries the draw's alpha only for blending and is not read.
- Sources: tfrag and tie at LOD 0 (envmapped tie base and shine passes included) use the
  `.unbake` sidecar's smooth, area-weighted normal (`background/Unbake.h`), merc and emerc the
  skinned vertex normal (inverse-transpose of the blended bone transform, so it matches the
  positions exactly), and shrub, wind tie, generic and tfrag without sidecar data the face normal
  from screen derivatives. Smooth normals take the side of the face normal, since winding
  orientation is arbitrary.
- Blending: draws that blend with anything other than plain alpha (`SRC_DST_SRC_DST`) mask
  draw buffer 1 (`glColorMaski`), so additive and fixed-alpha layers keep the normal
  underneath; alpha-blended draws blend their normal in by their alpha.
- The AO pass averages the pixel's samples that hold a normal and falls back to the 4-tap
  best-pair depth reconstruction where none does (sky, water, anything drawn outside those
  buckets). Debug view 7 shows the source: green target, red fallback.
- Each AO sample must rise above both the smooth normal's plane and the depth face normal's
  plane, and shallow samples whose smooth normal matches the pixel's count as the same surface.
  Without this, smooth normals on flat low-poly facets make each facet shade itself.
- Smooth normals need the level's sidecar (`out/jak1/fr3/<level>.unbake`, else
  `$SANDOVER_DATA/lighting/` or `<project>/sandover/data/lighting/`, see `howto/lighting.md`).
  Without it the level's tfrag and tie use face normals, which
  still avoid the depth reconstruction's cross-edge mixing but keep facets.
- Detail normal target (tile lights only): while the tile lights and the background material
  shading are on, colour attachment 6 holds the material's mapped normal (normal map at the
  parallax-shifted UV, `pbr_background`'s `Nd`) as its tangential part around the shading normal,
  RG8 at the frame's sample count, `128 + 127 t` per channel, cleared to 128 (unperturbed). The
  frame (`ssao_dn_frame`) is built in the tile pass's view space and is continuous except at a
  normal facing away from the camera. It goes with draw buffer 1 (same buckets, same blend masks);
  merc and the other draws leave it unperturbed. AO and contact shadows never read it, so their
  look is unchanged; the normal target keeps the vertex normal they need.
- With SSAO and the tile lights off nothing is attached and the shaders' `ssao_nrm` switch is 0;
  the extra fragment output then has no draw buffer. Switching both off detaches the target once.
  With SSAO off and the tile lights on, the depth copy, normals and stencil still run and the AO
  passes are skipped, so AO off leaves the tile lights as they were.

## Emissive mask

Glowing background geometry (lamps, flames, lava, crystals) must not receive AO.
The `.unbake` sidecar (version 2, `howto/lighting.md`) carries a per-vertex emissive value E and
its source; the opaque tfrag and non-envmap tie draws of buckets 5-30 write the live value to
an R8 colour attachment 2 (the frame's sample count, about 15 MB at 2560×1440 MSAA 4),
cleared to 0 before bucket 0 of each dispatch. Live E = stored E × the source's weight this
frame: the palette weight of an emitter slot (flames, lamps, lava, crystal fades; lightning,
caustics and camera fades weigh 0), the summed night-slot weight for lamps baked into the
night slots, 1 for light that is on at every hour.

The composite multiplies the background AO by `1 − smoothstep(lo, hi, E)` (E averaged over the
pixel's samples), so pixels with E ≥ `hi` keep their colour. Character pixels (stencil bit 6)
ignore E. Depth-writing merc draws now write their own E, so this guard only matters for shrubs and
generic, which still draw over the mask without clearing it.

- Debug view 8 shows E as a grey level over the scene dimmed to 25%.
- Needs SSAO on (or bloom on, below); with both off nothing is attached or drawn.
- Limits: shrubs, generic, wind ties and envmapped ties write no E, so they keep the value of
  whatever emissive surface they cover (visible in view 8 as grass over a lamp pool). Levels
  without a v2 sidecar write E = 0 everywhere.

## Bloom

Bloom from the same mask, off by default, Jak 1 only. Additive world sprites (particles) add
0.4 × their brightest channel to the mask and HUD sprites clear it, while bloom is on; sprite
and merc textures listed in the actor emissive table write their own E instead (max blend,
`materials.md` "Actor emissive table"). After
bucket 66 (sprites, before the HUD buckets): resolve the frame, bright pass
`smoothstep(lo, hi, E) × max(colour − threshold, 0)` (the SSAO ramp) at half resolution, a separable Gaussian (two passes), then an
additive composite over RGB (destination alpha untouched). Under the FP16 target
(`pc-set-hdr-target`) its copies are RGBA16F, so colour above 1 reaches the bright pass and the
composite keeps the headroom for the tonemap.

`(pc-set-bloom which millionths)`: 0 enable, 1 strength (default 1), 2 radius in
half-resolution pixels (default 6, sigma = radius / 2), 3 threshold (default 0.3), 4 hue clip
(default on; 0 = the old per-channel add).

Hue clip: where `min(frame, 1) + bloom` has a channel above 1, the composite adds only
`max(sum / max(r, g, b) - min(frame, 1), 0)` instead of letting each channel clip, so a blue
vent stays blue rather than going white. Bloom never lowers a channel: a pixel the frame has
already clipped keeps its vanilla colour (it is at most not boosted). Dividing the whole pixel
by the max (the first version) pulled red and green of stacked additive particles below
the frame itself, turning pale blue eco into flat deep blue. Pixels the bloom does not
reach, or where the sum stays at or under 1, are a plain add as before. Hot cores: as the
sum's max channel goes from the tonemap's white start to white full (`pc-set-tonemap` 6 and 7,
default 2 and 3.5), the hue-kept target blends toward `min(sum, 1)`, the per-channel clip, so
a very bright core goes white as in vanilla while its halo keeps its hue. The add is per pixel
from the resolved frame and is applied to every MSAA sample; under the FP16 target values
above 1 are kept for the tonemap. goalc
builds older than this change need `(define-extern pc-set-bloom (function int int none))` first.
Bloom off does no GL work. Where E = 0 (daylight with no emitter live) it adds nothing.
Depth-writing merc draws not in the actor emissive table write E = 0, so a character, prop or
door never blooms with the light of what is drawn behind it (the sunken iris door showed an
x-ray of the lit rooms behind it). Envmapped TIE base draws (`etie_base`) write their own sidecar E
like tfrag and plain TIE; before, they wrote none, so the village2 lanterns' glass (E 0.49-1.0) never
bloomed. Shrubs and generic still keep the E of the surface they cover.
Side effect: a character covering an emitter on screen also removes that area from tile-light
seeding, so the emitter's light can dip slightly while he crosses it.

## Emissive tile lights

Light from emissive pixels (particles, lamps, lava, the actor emissive table) on nearby
floor and walls, off by default, Jak 1 only. It reads the SSAO pipeline's depth copy, stencil and
normal target, which run for it with AO off too. `EmissiveLights.h`;. After bucket 66 (sprites), before bloom:

1. The frame is cut into a 64-wide tile grid (64x36 at 16:9). One fragment per tile reads every
   second pixel: flux = mean of `smoothstep(lo, hi, E) x colour`, and the E-weighted camera-space
   position of that emission. Particles write no depth, so emissive sprites (own E at least half
   the ramp's low end) keep their nearest 1 / z in metres on colour attachment 4 (R16F, max
   blend) and a tap they cover takes that depth; the result is still pulled to at most 1 m
   behind the tile's nearest surface, or behind its nearest emissive sprite when a
   non-emitting surface in the tile is nearer (an occluder edge in front of a far particle).
2. One point per tile ranks the tiles by flux (luma) and the 64 brightest above `threshold`
   land in a candidate row; power = flux x the tile's area at the light's depth (m^2). The row
   is read back (1 KB) and tracked on the CPU (`LightTracker.h`, below).
3. Every pixel with stencil bit 7 clear (opaque background and characters) adds, per light
   within `radius_m`, `power x N.L x (1 - d / radius)^falloff x intensity`, where N.L is
   `max(wrap(N.L) + relief x (wrap(Nd.L) - wrap(N.L)), 0)`, the same term as the sun's relief in
   `pbr_respond` with the pass's own wrap, `wrap(x) = max((x + 0.5) / 1.5, 0)`. `N` is the vertex
   normal, `Nd` the mapped normal from the detail normal target and `relief` the material
   response's gain (`1 + 2 x ambient`). Highlights use `Nd`, half of it tinted by the surface colour. Pixels with E above the ramp get nothing, and a
   light fades to zero over its nearest 0.2 m (no self-light, no hard ring). The summed light
   is soft-limited to 0.35 luma (`x 0.35 / (0.35 + luma)`), so several lights stacked on one
   small emitter give a coloured pool instead of a white patch.

`(pc-set-emissive-lights which millionths)`: 0 enable, 1 intensity, 2 radius (m), 3 falloff
exponent, 4 tile threshold. Defaults and round candidates: `presets/emissive.toml`
`[emissive_lights]`; `sandover-preset apply emissive default` prints the forms (with the
`define-extern` an older `out/` needs), `--preset emissive=default` sends them in a rig run.
With it on, additive sprites write E even while bloom is off.

Per-texture light share (design decision 2026-09-26: precursor orbs glow but light nothing).
The emissive mask is RG8: red is E (bloom, the SSAO fade, the no-self-light test in step 3),
green is the part of that E whose texture gives no light. The tile select (step 1) reads
`max(r - g, 0)`. Merc and listed sprites write `green = E x (1 - lights)` from the actor emissive
table's `lights` column; the ocean writes green = red (its sun glint, crest sparkle and old Blinn
streak bloom but are reflected sunlight, not a light: the near ocean writes no depth, so the tile
took the seabed's or the beach's and lit the sand), as do lava and snow glints; everything else
writes 0, so tfrag, tie and unlisted particles light as before. The knob is data: `lights = ...` on a texture in `presets/emissive.toml`
`[actor_emissive.textures]`, then `sandover-materials actor-emissive` to export
`emissive-actors.csv` (`kb/howto/materials.md`); the engine re-reads it from the bloom debug
window's reload button or at the next boot. `lights = 0` today: `egg-ndimadman` (orb model) and
`hud-egg` (orb glint). The power cell's `fuel-cell-inside`, `flare` and `starflash` keep 1. Under
the sprites' max blend, a lit and an unlit texture on the same pixel give an approximate share;
both are listed-texture pixels, so this is rare. World lights honour the same decision through
`entity_types.json` (`money` emits false, `kb/howto/lights.md`).

Temporal tracking (`LightTracker.h`, snapshot run 2026-09-25): the tile grid is fixed to the
screen, so a moving camera moves each emitter across tile edges and the brightest tiles'
position, colour and class jumped every frame (lava and orb light flickered). The candidates
are matched (greedy nearest, within 2 tile widths at their distance) to 32 fixed light slots
kept in world space; a matched slot moves its position and power `temporal_blend` of the way
per frame. A tile is thin (arcs, sparks: its glowing taps fill under 20% of their bounding box;
keeps 15% of its power) until its fill passes 50%, and back only under 20%. A slot shows after
`temporal_frames` frames with a match, hides after as many without, and fades over
`fade_frames`; with every slot taken, a candidate twice as bright as the faintest slot fades
that slot out. Tracking runs once per frame (the second stereo eye only re-projects the slots)
and restarts after a camera cut (8 m or 30 degrees in one frame). Indices 13..16: blend, frames,
fade, `world_suppress`. Preset `unsmoothed` turns the smoothing, hysteresis, fade and world
priority off but is not the pre-tracking build: slot membership still persists, the thin cover
is binary (15% or 100%), arcs no longer drop thin tiles and nothing dedupes tiles against world
lights. The true baseline is a master build. Cost: two 1 KB reads, a full mid-frame GPU sync;
read it from the debug window's "readback + track" CPU number, not the GPU timer.
`(pc-set-emissive-lights 17 N)` saves the next N frames as `<shot>-fNN.png` and logs each
frame's candidates and drawn lights (`EMLIGHTS`); the rig's `pan:` shots use it (below).

World lights (`WorldLights.h`): the nearest `world_count` (default 16, max
32) census emitters that are on join the tile lights, so a vent, cell, lamp or crystal off
screen or small on screen still lights the ground near it. Table: `WorldLights.inc`, compiled
into gk, generated by `sandover-lights bake` (`kb/howto/lights.md`). Per light: power = colour x
class intensity (`world_strong`, `_lamp`, `_weak`, `_arc`, `_particle`) x gain x state, radius =
baked radius x `world_radius_scale`, capped at `world_radius_max_m`. World lights have priority:
a tile slot within `world_suppress` x a world light's radius gives no light while that world
light is on (in step with its fade), and no new slot starts there, so orbs, lava, lamps,
crystals and arcs light from their fixed census position. World lights fade in and out by
table row with the same frame counts. Indices 5..12 of
`pc-set-emissive-lights` (12 = lamp intensity); 5 = 0 turns the list
off. GOAL answers the entity states once per frame from the display loop
(`pc-world-lights-update` in `main.gc`).

## Rig captures

Temporal stability: a manifest shot with `pan: {frames: 16, yaw_deg: 1.5}` (or `move_m: [x, y, z]`
per frame; needs an explicit camera) settles, then saves 16 consecutive frames while the camera
turns about the world up axis each frame. The sidecar's `pan.lights` holds the engine's light
lists per frame; `sandover-lights flicker <dir>` writes `<name>-strip.png` and reports, per
frame pair, how far the lights moved and how much of their power appeared, vanished or changed
(`raw` = candidates fed to the tracker, `out` = tile lights drawn, `world` = world lights drawn
by row), the step in total drawn power, and the tracked slots' position variance. Compare
against the same shots with `--preset emissive=unsmoothed` (`--baseline`). It also follows a grid
of boxes over the frames by cross-correlation and reports each tracked patch's luma and
blue-minus-red steps (the lit geometry, not the light lists); `--control` takes the same shots
with `--preset emissive=off`, so a step present there too is not from the tile or world lights:

```sh
uv run sandover-capture run ../shots/light-stable.yaml --parallel 2 --preset ssao=deep \
    --preset emissive=default --out-dir $SANDOVER_DATA/shots/light-stable
uv run sandover-capture run ../shots/light-stable.yaml --parallel 2 --preset ssao=deep \
    --preset emissive=unsmoothed --out-dir $SANDOVER_DATA/shots/light-stable-unsmoothed
uv run --no-sync python -m sandover.lights flicker $SANDOVER_DATA/shots/light-stable \
    --baseline $SANDOVER_DATA/shots/light-stable-unsmoothed
```

```sh
cd sandover/tools
uv run sandover-capture run ../shots/auto-v0.yaml --preset ssao=deep-high --out-dir $SANDOVER_DATA/shots/ssao/high
uv run sandover-capture one --continue-point beach-start --preset ssao=deep \
    --goal "(pc-set-ssao-param 10 5000000)" --out stencil.png        # debug view 5
uv run sandover-capture run ../shots/auto-v0.yaml --bin <staged copy> --out-dir master/  # another build
uv run sandover-grid $SANDOVER_DATA/shots/ssao/{off,deep-low,deep,deep-high,deep-ultra} --out sheets/ --captions ssao
```

`--preset ssao=round<N>/<name>` takes a candidate from a `[round<N>.presets]` table (its `base`
preset plus the keys it overrides) while a round is open; decided rounds are deleted. `--preset phase=name` resolves the TOML to `pc-set-ssao-param`
forms (`sandover-preset apply ssao <name>` prints them) and records them in the sidecar's `toggles`. `--goal` sends extra
forms once per instance before the first shot.

## Cost

```sh
uv run sandover-ssao-cost --json cost.json    # refuses to start if any gk is running
```

It stages village1-hut, beach-start and jungle-start at noon. It switches the game to 2560×1440
MSAA 4, then logs the second 240-frame window of each `SSAO-TIMING` scope for `off` and every
preset. Run it twice and compare: GPU power state alone can double the numbers.

## Presets are generated into the engine

The TOML is the source of truth. After editing it:

```sh
uv run sandover-preset gen-header ssao          # rewrites game/graphics/opengl_renderer/SsaoPresets.inc
uv run sandover-preset gen-header ssao --check  # CI-style staleness check
```

Then rebuild `gk`. Rig runs do not need the rebuild, because `--preset` reads the TOML directly.

## Parity check

Capture the same shots with SSAO off on this build and on a master build (`--bin`), and
again off/off for the noise floor. Compare with `sandover-capture compare A B`. Off must sit
within the off/off spread, with differing pixels only on animated actors and creatures.

## Known limits

- The AO history needs motion vectors; stereo and XR run the fixed pattern with three denoise
  passes. The history rejects on clip-w mismatch only (no normal test).
- Levels without a sidecar get face normals on tfrag/tie, so their facets can still show.
- Characters cast AO onto the ground at the background strength; `merc_weight` scales only AO on character pixels.
- Stencil: sky/far ocean and translucent actors (45-46) are excluded; blended tfrag (31-44) receives AO.
- The luminance floor and `cave_scale` apply only while a fixed-slot level is loaded.
- Stereo: per-eye by construction. It is untested live because enabling stereo from the
  rig before the level load crashes in Tie3 wind (a pre-existing bug).
