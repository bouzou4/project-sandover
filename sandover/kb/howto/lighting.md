# Lighting how-to: per-vertex sidecar and moving sun (Jak 1)

Option 2, the moving sun. Findings:
`kb/findings/2026-09-23-sun-experiments.md`.

## 1. Write the sidecars

Needs the unbake fits (`$SANDOVER_DATA/unbake/<level>-fit.npz`, `<level>-summary.json`) and the
Unbake rip (`$SANDOVER_DATA/unbake/riproot/decompiler_out/jak1/levels/<level>/<level>-background.glb`).

```sh
cd sandover/tools
uv run sandover-unbake export-sidecar village1 beach jungle training finalboss --install
```

Writes `$SANDOVER_DATA/lighting/<level>.unbake` and `<level>-sidecar.json` (per tree: vertex
count, fitted count, mean confidence, position digest). `--install` copies the `.unbake` into
`out/jak1/fr3/`, next to the fr3. The game reads the first that exists of
`out/jak1/fr3/<level>.unbake`, `$SANDOVER_DATA/lighting/<level>.unbake` and
`<project>/sandover/data/lighting/<level>.unbake` (where the package ships them), and logs
`unbake: <level> sidecar <path>`. So a copy next to the fr3 overrides `$SANDOVER_DATA`, which
overrides the package; the digest check rejects a copy for other geometry but not an older fit. Options: `--q-lo/--q-hi`
(day-slot residual in u8 mapped to confidence 1 → 0, default 8 → 32), `--k-max` (sun gain
clamp, default 3.99; vertices above it get q = 0).

The writer refuses a level whose glb vertex subset does not reproduce the fit arrays exactly.
With no level arguments it writes all 21. A level without a fit still gets a sidecar: normals
and the emissive mask, q = 0 and an empty slot table. `--install` copies into
`$OPENGOAL_PROJ_PATH/out/jak1/fr3` (default `$SANDOVER_ROOT`).

## 1b. Emissive mask (sidecar version 2)

`sandover/unbake/emissive.py` computes a per-vertex emissive value E from the eight baked
palette colours of the glb alone (luminance, u8 steps, 128 = 1.0), summed from three sources:

| Source | Value | Runtime weight |
|---|---|---|
| effect slot s (role table, `levels.py`) | luminance of slot s | palette weight of s if its role is an emitter (flames, light, lava, fade-lights), else 0 |
| night (hour-table slots live at 0:00 and not 10:00-14:00) | night luminance above `night_ratio` × brightest day slot + 32 | summed night-slot weight |
| static (levels with two or more sun slots) | dimmest sun slot above 96 | 1 |

`night_ratio` is the level's median night/day ratio (0.4-0.7). The night source exists because
the `village1`-table lamps (beach, village1, jungle) are baked into night slots 6-7, not into
an effect slot: beach slot 5 is all zero. Stored: E = min(sum, 255) / 255 in record.w bits
16-23 and the largest source in bits 24-27 (0-7 slot, 8 static, 9 night). Header: the old pad
words now hold `night_mask` and `emitter_mask`. The loader reads versions 1 and 2; version 1
behaves as E = 0. Per-level E statistics are in `<level>-sidecar.json` (`emissive`).
Consumers: SSAO (`howto/ssao.md`, Emissive mask) and bloom.

## 2. Vertex order and the key

The glb exporter writes each tfrag/tie tree's vertex buffer straight from
`unpacked.vertices` (`fr3_to_gltf.cpp`, `add_tfrag`/`add_tie`), the same array the loader
uploads (`LoaderStages.cpp`), and the renderers draw with plain `glDrawElements` on that
buffer, so `gl_VertexID` is the vertex index. The sidecar stores, per tree, the vertex count
and a 64-bit digest of the float32 positions in that order; the loader recomputes both from
the unpacked fr3 and drops any tree that differs. The game log shows one line per level:

```
unbake: village1 7/7 trees match (590416 vertices)
```

A mismatch names the trees (`tie2:digest`); those trees draw vanilla.

Scope: geometry LOD 0 only; tfrag and non-envmap tie draws. Shrubs, wind ties, envmapped
ties and the tfrag lowres tree carry no data (q = 0 or not drawn with the sun shader).

## 3. Engine toggle and controls

Default off. With the toggle off the renderers use the unchanged `tfrag3` shader; with it on,
trees that have sidecar data draw with `tfrag3_sun` (`tfrag3_sun.vert` + `tfrag3.frag`).

ImGui: Renderer → Sun. GOAL (values in millionths):

| Form | Effect |
|---|---|
| `(pc-set-sun-option 0 1000000)` | on (0 = off) |
| `(pc-set-sun-option 1 m)` | mode: 0 identity (every slot targets its own fitted sun; delta exactly 0), 1 follow the arc (one sun at the arc position of the vanilla blend), 2 direction (`pc-set-sun-dir`), 3 fixed arc position |
| `(pc-set-sun-option 2 d)` | offset along the fitted arc, degrees of sun travel (+ = later) |
| `(pc-set-sun-option 3 n)` | night slots 6-7: 0 fitted moon (the bake), 1 table moon direction, 2 ambient only |
| `(pc-set-sun-option 4 s)` | strength of the delta |
| `(pc-set-sun-option 5 v)` | debug view: 1 model in place of the bake, 2 confidence, 3 normal, 4 delta |
| `(pc-set-sun-option 6 u)` | arc position for mode 3 (0..4 = day slots 0..4) |
| `(pc-set-sun-dir x y z)` | direction toward the sun for mode 2 |

The term, per palette slot s with blend weight w_s (per channel, from `itimes`):
`Δ = Σ_s w_s · q · [a·Amb′_s + v′·k·max(0, n·L′_s)·Lgt′_s − (a·Amb_s + v_s·k·max(0, n·d_s)·Lgt_s)]`.
The target equals the fitted slot in identity mode, so the delta is 0 by construction; in
follow mode it equals the fitted slot at 7:00, 9:00, 12:00, 15:00 and 18:00, where one slot
is live alone.

## 4. Experiments

```sh
cd sandover/tools
uv run python -m sandover.unbake.experiments run --parallel 2     # all variants
uv run python -m sandover.unbake.experiments flip                 # FLIP vs the reference column
uv run python -m sandover.unbake.experiments sheets --crop 0.25,0.3,0.5,0.5
```

Manifests: `sandover/shots/sun-day.yaml`, `sun-night.yaml`. Output:
`$SANDOVER_DATA/experiments/sun-experiments/<experiment>/<variant>/`, sheets in `.../sheets/`,
`flip.json`. Variants and captions are defined in `sandover/unbake/experiments.py`.

## 5. Parity

- Toggle off: `uv run sandover-parity check --build current`.
- Toggle on, identity mode: the `day/identity` column of the experiment, FLIP vs `day/vanilla`.

## 6. FP16 target and tonemap (Jak 1)

FP16 target and tonemap, ADR-0013 amendment. Default off; never on in the
faithful look. Clamp is the default mode; filmic and toon are look choices for presets
(`presets/tonemap.toml`, `toon` recommended for SDR).
Finding: `kb/findings/2026-09-24-fp16-target.md`.

| Form | Effect |
|---|---|
| `(pc-set-hdr-target #t)` | main colour target (and the MSAA resolve, stereo buffer) becomes `RGBA16F`; `#f` restores `RGBA8` |
| `(pc-set-tonemap 0 m)` | mode in millionths: 0 clamp (identity, clamp to 1), 1000000 filmic, 2000000 toon |
| `(pc-set-tonemap 1 e)` | filmic and toon exposure, 1000000 = neutral |
| `(pc-set-tonemap 2 k)` | filmic knee: identity below, exponential shoulder to 1.0 above, per channel (default 800000) |
| `(pc-set-tonemap 3 n)` | paper white in nits, stored only (reserved for HDR output) |
| `(pc-set-tonemap 4 k)` | toon knee: luminance identity below (default 950000) |
| `(pc-set-tonemap 5 r)` | toon range: luminance 1 + r maps to 1.0 (default 1500000) |
| `(pc-set-tonemap 6 s)` | white start: max channel where a hot core starts to blend toward the per-channel clip, toon and the bloom hue clip (default 2000000) |
| `(pc-set-tonemap 7 f)` | white full: max channel where the core is the per-channel clip (default 3500000) |
| `(pc-set-tonemap 8 s)` | halo start: toon only, max channel where any overbright pixel starts toward the per-channel clip (default 1200000) |
| `(pc-set-tonemap 9 f)` | halo full: max channel where it is the per-channel clip (default 1800000) |

Filmic works per channel, so with a low knee it compresses everything above the knee: at
0.6 a white wall ends at 0.85 and the dominant channel of a saturated colour drops while the
others stay, which reads as washed out. Toon works on luminance: below the knee (and with no
channel above 1) the pixel is unchanged; from the knee to 1 + range an extended-Reinhard
shoulder rolls luminance off to exactly 1.0, the colour is scaled by the tonemapped /
original luminance (hue and saturation kept), and a channel still above 1 is clipped per
channel, as vanilla's RGBA8 target clips it. Until 2026-10-22 it was brought back by desaturating
toward the luminance (at most 25%) and scaling by 1 / max channel (the bloom hue clip); that
turned the noon sky violet (village1 +15 degrees, chroma x1.11 against vanilla) and greyed sunlit
grass, both of which sit just above 1 on one channel (`kb/findings/2026-10-22-sdr-colour.md`).
A pure white surface ends at 0.975.

On SDR the knee must sit below 1: the shoulder needs room under the display's white, so
identity all the way through white is impossible there (the knee stays at 0.95; a 0.975 white
is not visible). Identity through white with a shoulder above it only exists on HDR output,
where the shoulder lives above paper white. In SDR, values past the point where a
channel reaches 1 clip there, as in vanilla; HDR output keeps them (`hdr_map`).

Hot cores: the luminance shoulder scales a core down before the per-channel clip, so a clipped
shouldered core never reaches white: eco would lose vanilla's white-hot centre and torch flames
go salmon instead of yellow. Toon therefore blends toward `min(c, 1)` of the unshouldered colour
(vanilla's per-channel clip) by a smoothstep of the max channel from white start (2) to white
full (3.5). Below 2 the core is the shouldered colour clipped per channel; at 3.5 and above it
is exactly what vanilla's clamp gives: white for blue eco (every channel
past 1), yellow for fire (red and green past 1, blue not). The max channel, not luminance,
drives it: a blue eco core at (1.3, 1.8, 4.2) has a luminance of only 1.9. On the
`runs/20260926-1011-play` EXRs it changes at most 1.5% of a frame (the green vent, 0004),
otherwise under 0.6%. White start at 1e9 turns core whitening off (shoulder, then the per-channel clip).

Halos: the ring around the core is the same kind of pixel as the core, an additive stack that
vanilla's RGBA8 target clips per channel. For blue eco the blue clips first while green keeps
rising, so vanilla's halo turns cyan; a green vent turns lime, a yellow one lemon. Scaling the
shouldered stack keeps its ratio, so without a clip of the unshouldered colour ours read violet,
pure green and orange. Toon therefore takes the
same per-channel clip from halo start (1.2) to halo full (1.8) as well; in the tonemap it is one
ramp with the white one (max of the two). Since the per-channel clip below the halo ramp
(2026-10-22) the ramp only matters for pixels in the luminance shoulder, whose clip it takes
from the unshouldered colour. On the 16 `eco-reference`
pairs the halo hue lands within about 8 degrees of vanilla (blue vent night 224 vs 216, was 230;
green vent 88/79 vs 79/77, was 98/92). Per-eco vanilla reference and the night halo hues
before and after: `kb/facts/eco.md`. Halo start at 1e9 turns the halo ramp off
(`(pc-set-tonemap 8 1000000000000000)`); the output is still clipped per channel, after the
shoulder. Bloom's clip keeps white start and white full: there the per-channel clip would let bloom whiten a blue vent.
With the defaults the halo ramp is complete at 1.8, so in toon the core ramp (2 to 3.5) no
longer changes anything: every pixel with a max channel of 1.8 or more is the plain per-channel
clip, and white start / white full now only drive bloom's clip. That includes lava crests,
which take vanilla's yellow (0018: 255,91,42 before, 255,144,28 now).

The tonemap modes (clamp, filmic, toon) apply only to SDR output and to the internal-resolution
screenshot copy. With HDR output on (`pc-set-hdr-output`, `hdr-output.md`) the final pass skips
them: `hdr_map` maps the scene to nits with its own shoulder above paper white, and
`pc-set-tonemap` does not change the window (EXR captures still record the mode).

The tonemap runs in the final pass (`do_pcrtc_effects`, `post_processing.frag`) after the MSAA
resolve and before the game's brightness/contrast terms and blackout. Bloom and the SSAO
composite are drawn earlier, so they are tonemapped; so are the HUD and text, which draw into
the same target. The OpenXR eye copy is a blit (clamp only, no filmic). With the target off the
shader passes the `RGBA8` image through unchanged.

GOAL code compiled from a checkout without this branch's `kernel-defs.gc` needs the externs
first, e.g. as rig `--goal` forms:

```sh
uv run sandover-capture run ../shots/auto-v0.yaml --bin <copy> \
  --goal "(define-extern pc-set-hdr-target (function symbol none))" \
  --goal "(define-extern pc-set-tonemap (function int int none))" \
  --goal "(pc-set-hdr-target #t)" --goal "(pc-set-tonemap 0 0)"
```

Internal-resolution screenshots with the target on are tonemapped into an `RGBA8` copy
(without brightness/contrast, like the `RGBA8` path). Every screenshot with the target on
(rig, F9, F2) also writes the untonemapped `<name>.exr` (`kb/howto/hdr.md`) and logs one line:
`hdr-stats: max <largest channel> over1 <fraction of pixels above 1> neg <fraction below 0> nonfinite <count>`.
With `SANDOVER_HDR_MASK` set in gk's environment each screenshot also gets `<name>.hdr.png`
next to it: R = largest channel above 1 (1..2 → 0..255), G = largest channel / 2, B = 255
where any channel is negative.

Cost: `(pc-set-ssao-param 11 1000000)` also times the final pass (`SSAO-TIMING name=pcrtc`).

While the target is FP16 the GS-emulation shaders (`direct_basic`, `direct_basic_textured`)
clamp their output to [0, 1] like an 8-bit target; without it the sky clips to white. The
parity gap per shot and the audited blend paths are in the finding.

## 7. Ocean shading (Jak 1)

Off by default; with it off the ocean shaders run the vanilla code (uniform branch on
`water_on`) and the mid ocean draws where vanilla draws it. Near (`ocean-near`) and mid
(`ocean-mid-far`, mid part) ocean only; ocean-far is drawn by the direct renderer and is
unchanged. Presets: `sandover/presets/water.toml` (`--preset water=bold`, the
revision-2 pick; the engine defaults match it). Findings:
`kb/findings/2026-09-25-water.md` (revision 1), `kb/findings/2026-09-25-water-waves.md`
(revision 2). Standard shots: `sandover/shots/water.yaml`.

| Form | Effect |
|---|---|
| `(pc-set-water 0 1000000)` | on (0 = off) |
| `(pc-set-water 1 w)` | ripple-normal slope scale (gradient of the animated ocean texture's luminance) |
| `(pc-set-water 2 g)` | sun glint intensity; above 1 it exceeds the 8-bit range on the FP16 target |
| `(pc-set-water 3 e)` | glint Blinn exponent |
| `(pc-set-water 4 f)` | fresnel blend toward the mood fog colour at grazing angles, 0..1 |
| `(pc-set-water 5 s)` | crest sparkle: texels brighter than their surroundings (texture and a finer rotated copy), sunlit |
| `(pc-set-water 6 v)` | debug view: 1 normal, 2 glint and sparkle, 3 fresnel weight, 4 view vector, 5 sun direction, 6 foam, 7 depth ramp (red) and shore fade (green), 8 wave height (red) and Jacobian (green), 19 the sun the world map leaves at the water (blue: no cascade) |
| `(pc-set-water 7 a)` | Gerstner amplitude scale; 0 keeps the vanilla vertices |
| `(pc-set-water 8 n)` | max tessellation level of an ocean triangle edge (1..32) |
| `(pc-set-water 9 m)` / `19 m` | waves at full amplitude to the first distance (m), falling off smoothly to none at the second |
| `(pc-set-water 10 c)` / `11 j` | crest foam strength / Jacobian threshold |
| `(pc-set-water 12 s)` / `13 m` | shoreline foam strength / band width (m of water depth) |
| `(pc-set-water 14 s)` / `15 m` | shallow colour strength / depth (m) it fades over |
| `(pc-set-water 16 s)` | subsurface tint on crests, stronger looking toward the sun |
| `(pc-set-water 17 c)` | crests lighter, troughs darker |
| `(pc-set-water 18 a)` | near-ocean absorption per metre of water seen through (default 0.03), per channel: transmittance `exp(-a * absorb_rgb * m)`, filled with the game's own water colour. The depth is re-copied before the near ocean, so Jak and the orbs fog by their own depth, not the seabed's; the near base draws twice (scene x T / max(T), then the over blend with alpha 1 - max(T)). Water off: upstream's single alpha draw |
| `(pc-set-water 20+5i+f v)` | sea wave i (0..4; 4 is the long swell), field f: 0 direction (degrees from +x), 1 amplitude (m), 2 wavelength (m), 3 phase speed (m/s), 4 steepness 0..1 |
| `(pc-set-water 60+5i+f v)` | the same for the lake wave set (levels mapped to `lake` in `water.toml` [bodies]) |
| `(pc-set-water 45 s)` / `46 m` | fresnel reflects the mood environment (0 the fog colour only) / distance (m) over which it hazes into the fog colour |
| `(pc-set-water 47 a)` / `48 p` / `49 m` | per-wave amplitude noise (fraction) / phase noise (turns) / noise cell (m) |
| `(pc-set-water 50 m)` | depth (m) by which the shallow tint and shore foam are gone; no scene depth behind counts as deep |
| `(pc-set-water 102 m)` | water depth (m) by which the near ocean is opaque (default 12) |
| `(pc-set-water 103 r)` / `104 g` / `105 b` | per-channel absorption, times option 18 (default 1.6 / 0.9 / 0.5: red goes first) |
| `(pc-set-water 6 21000000)` | debug view 21, dry: the near ocean covers nothing (the chroma reference for transparency) |
| `(pc-set-water 51 m)` / `52 m` | shallow tint and shore foam fade out between these camera distances (m) |

The ocean vertices arrive in GS screen space. With the shading on, the vertex shader
rebuilds each vertex's world position (inverse of `camera_matrix`, then along the pixel ray
onto the ocean plane, whose height is the median of the depth-reconstructed vertex heights;
the near ocean keeps its depth-reconstructed position, since its vertices carry the game's
own wave heights),
adds a four-wave Gerstner sum (GPU Gems 1, ch. 1) and projects it back. The mid grid is 12 m,
so with displacement on the ocean draws through `ocean_water` (vertex, tessellation control
and evaluation stages, then `ocean_common.frag`): each triangle edge is split by its length
on screen (3 GS pixels or 0.5 m a segment, up to `subdiv`, the same level for both triangles
of an edge), and the new vertices are displaced in the evaluation shader. GS x, y, z, STQ,
colour and fog are all linear in screen space, so the barycentric split is exact. Waves keep
their full amplitude to `range_full_m` and fall off smoothly to none at `range_zero_m`
(shorter waves sooner). The mid ocean covers the ocean map (48 x 48 tiles of 96 m), and
ocean-far only starts past its edge; the mid mesh's outer edge vertices get a wave weight of
0, so the waves meet the flat far band without a step there. The fragment shader evaluates the same sum at the pixel's rest
position (passed as xz/w, 1/w, since the ocean's `gl_Position.w` is 1) for the analytic
normal and the Jacobian of the horizontal displacement: crest foam where the Jacobian drops
below the threshold, cut from a scrolling value noise. Shoreline foam and the shallow colour
come from the opaque scene depth under the surface: with the shading on, the mid ocean is
kept back and drawn after the tfrag and tie of both levels (bucket 18), right after a
single-sample depth copy (`water-depth`); the near ocean reads the same copy. Without the
copy, there is no shore foam or shallow colour. The shallow colour is the level colour,
lightened. Foam is lit but never written to the emissive mask; the glint is, so bloom picks
it up. The sun is the blended fitted day sun of the last background draw (the same source as
the class shading); without a sidecar sun there is no glint or backlit subsurface tint.

Revision 3: the fresnel term reflects the material response's mood environment (horizon, derived
zenith, sun glow; `compute_frame_lights`) along the reflection vector, scaled so the horizon
has the fog colour's brightness, and hazing into the fog colour with distance (`sky_haze_m`).
A fifth, long swell and a slow world-space noise on each wave's amplitude and phase break the
repeat of the sum. The wave set follows the level (`water.toml` [bodies], `kWaterBodies`):
`sea` or `lake`. The tessellated program is compiled on first use with water on; without
GL 4.0 tessellation the ocean keeps its flat mesh and the waves live in the per-pixel
normal, foam and absorption.
Set the environment variable `SANDOVER_NO_TESS` for gk to force the non-tessellated fallback.

Cost: `(pc-set-ssao-param 11 1000000)` also times the ocean passes (`SSAO-TIMING
name=ocean-near`, `name=ocean-mid`).

### Sun glint on the ocean and inland water

The sun reflected in the water: a GGX lobe from the frame's day sun on the wave normal
(`water_sun_glint.glsl`), Smith visibility, Schlick fresnel at F0 0.02, so the reflection is
faint looking down and strong at grazing angles. It is computed and added in linear light
(the framebuffer is display-encoded, decoded with 2.2), soft-capped at `peak`, and then goes
through the same fog mix as the water under it and through the final pass like any other
overbright value: with HDR output `hdr_map` takes the brightest sparkles to the display peak,
white-hot through the toon per-channel clip with the sun's colour around them; the SDR toon
tonemap clips them to white. A quarter of the glint (at most 0.25) is written to the emissive
mask, so bloom adds a soft glow around a sparkle path but no glare sheet.

- Ocean (`ocean_common.frag`): on the Gerstner normal with the texture ripples, the same
  normal the waves are displaced with. Wave slopes the renderer drops (the range falloff and
  the wave AA past a few pixels per wavelength) are added back as roughness (`alpha^2 +=` their
  mean square slope), with the screen-space variance of the normal, so far water shows a
  broad glitter path instead of a thin mirror line. It replaces the Blinn glint
  (`pc-set-water 2/3`); the crest sparkle (`5`), foam and envmap are unchanged.
- Inland water (`merc2.frag`, clear water on levels with a sky): not on the mesh normals (the
  river's smooth normals lean and lit it as one sheet), but on world up plus six crossing short
  waves (`wsg_ripple`, 2.4 m to 0.24 m, deep-water dispersion on the water clock, faded per
  pixel with the lost slope added to the roughness). A broad sheen (a quarter of the lobe) and
  the body of a sharp lobe (alpha / 4) are soft-capped under the toon shoulder (0.93 encoded
  with the water under them); only the sharp lobe's top 15% is added hot, so SDR keeps the
  path under white and HDR still reaches the peak on the sparkles. A bright glint raises the
  alpha so the bed does not add on top. It replaces the capped lobe `pc-set-inland-water 4`;
  mud keeps its broad lobe.
- Which sun: each background setup records its level's day sun by sidecar
  (`water_level_sun`); inland water takes the sun of the draw's level, the ocean that of the
  level holding its wave set. The frame sun is the last background drawn, and an enclosed
  neighbour (jungleb beside jungle) stores a fixed overhead fit after the open level.
- Land in front of the sun: the world sun shadow map (`shadows.md`, option 34) through
  `ws_water_vis`, so every sun term on the water is shaded wherever the static world shadows
  it, with the glint row on or off: on the ocean the glint, the Blinn glint that stands in for
  it with the row off, the crest sparkle, the sky reflection's sun glow, the wave relief, the
  light through the crests and the foam's sun; inland the glint, the capped lobe, the mud lobe
  and the sky's sun glow (`pbr_env_sun_vis`), with the map's penumbra widened six times for water
  (`kWsWaterPen`: a 30 px shore blend at 4K on the beach, contact still crisp); past the last cascade
  (120 m) the glint stays open. The ocean reads it at the pixel ray's point on the water plane
  (world units); inland water at the fragment (merc camera space). The flat plane gives the
  map's receiver normal and slope bias, so no extra bias is added.
- Inland ripples: each of the six waves takes a slow value-noise phase (a full turn over 2.5
  wavelengths) and amplitude (0.4 to 1.6), which breaks the lattice their crossing made.
- SDR room: the inland sheen stays under the final pass's toon knee (`tm_toon_knee`, less 0.02).
- Night: the strength is scaled by the frame's day share (palette weight on the day slots,
  `compute_frame_lights`), so on the village1 hour table it fades from 18:00 to almost nothing
  at 22:00 and is gone from 23:00 to 3:00; it also fades out with the sun under the horizon.

| Form | Effect |
|---|---|
| `(pc-set-water 94 n)` | on (1000000) / off (0); off at boot, sent by the settings page's glint row (`glint?`), on in the play look |
| `(pc-set-water 95 s)` | strength: sun radiance scale |
| `(pc-set-water 96 a)` | GGX alpha between the wave normals (0.01..0.5) |
| `(pc-set-water 97 p)` | soft cap of the glint's linear luminance (1 = SDR white) |
| `(pc-set-water 98 1)` | 95-97 back to the engine defaults (sent by `sandover-settings-reapply`) |

Presets `sandover/presets/water-glint.toml`: `glint` (the engine default and the
chosen default: strength 6), `off` (`sandover-preset apply water-glint <name>`); 95-97 are rig and REPL knobs, not
settings rows. Debug: `(pc-set-water 6 2000000)` shows the glint and crest sparkle alone.
Inland, `(pc-set-inland-water 10 v)` with v (x 1000000): 2 sun, 3 glint normal, 4 world up,
5 mesh normal (merc camera space, `n * 0.5 + 0.5`), 6 the glint before the sun map, 7 the sun
the map leaves (white open, blue where no cascade reaches); from 2 up gk also logs the level
sun and its camera transform every 120 frames.

## 8. Sun disc (Jak 1)

The sun drawn in the sky (`SunDisc.cpp`), at the direction of the fitted sun model. Off at boot;
the play look turns it on with its own switch (`sun?`, the settings menu's Sun row; with neighbour
levels off it also sends the clock address, vista option 5), and
`sandover-settings-reapply` sends option 19, which puts options 11-18 back to their defaults.
Presets: `sandover/presets/sun-disc.toml` (`sandover-preset apply sun-disc sun|off`); the engine
defaults are `sun`, the chosen default (2.2 degrees, glare 0.3), so play needs no
forms. Shot manifest: `sandover/shots/sun-disc.yaml`.

| Form | Effect |
|---|---|
| `(pc-set-sun-option 10 1000000)` | on (0 = off) |
| `(pc-set-sun-option 11 d)` | angular diameter, degrees (default 2.2; the real sun is 0.53) |
| `(pc-set-sun-option 12 g)` | glare: share of the disc's colour the bloom bright pass takes, 0..1 (0.3) |
| `(pc-set-sun-option 13 c)`, `14 r` | HDR output: core and limb luminance in paper whites (2000, 2) |
| `(pc-set-sun-option 15 c)`, `16 r` | without HDR output: the same (12, 1) |
| `(pc-set-sun-option 17 f)` | per-frame step of the occlusion fade, 0..1 (0.35, about four frames) |
| `(pc-set-sun-option 18 m)` | 0: HDR values while HDR output presents; 1 always (captures, so the EXR holds the HDR scene); 2 never |
| `(pc-set-sun-option 19 1)` | 11-18 back to their defaults |

How it works:

- **Where.** The sidecar's day slots (0-4) each sit at the hour their palette weight peaks in the
  level's mood table (village1, beach, jungle, training: 7, 9, 12, 15 and 18 h). The disc slerps
  between those fitted directions by the game's hour (read through `vista_tod_addr`, which the
  vista switch sets) and continues along the arc before the first and after the last slot, so it
  rises at about 6:15 and sets at about 19:00 instead of standing still while the palette holds
  one slot. The lighting stays slot based. Without the hour it uses the palette-weighted blend of
  the day slots (the actor shadows' estimate). It needs a sidecar and a level with a sky
  (`LevelSky.inc`); no stereo or XR yet.
- **Occlusion.** Drawn after the opaque background buckets (and the vista), before the first
  transparent bucket, as a quad at window depth 0 with `GL_GEQUAL`: only samples whose depth is
  still the clear value (the sky) take it, so geometry cuts it per MSAA sample. A 1x1 pass reads
  21 taps of the depth buffer over the disc (centre, half and full radius) and blends the sky
  share into a texture by `fade` per frame; the disc's colour and bloom mask scale by it. A sun
  half behind fronds is half as bright and does not flicker as leaves move; the first frame after
  a gap takes the raw share.
- **Haze.** The game's fog of a distant object at its fog end, `(255 - fog_min) / 255`, and the
  unified fog and haze when that frame draws them (the larger, as `atmo_combine`), is the
  horizontal loss over the fog end distance L. Along the sun ray the layer is thinner: the path is
  `H / sqrt(sin^2(elevation) + (H / L)^2)` with H the unified fog scale height, so the
  transmittance is `(1 - f)^(path / L)`: the vanilla value at the horizon, less loss higher up.
  The disc is multiplied by it and added over the sky, which already carries the haze colour.
- **Brightness.** Luminance in paper whites, falling from the core to the limb on a log scale
  (`core * (rim / core)^(r^2)`), in the sun model's colour, times the haze transmittance. With
  HDR output the core (2000 paper whites; about 46000 nits before the shoulder even at village1's
  horizon loss, T = 0.12) lies far up `hdr_map`'s shoulder, so it lands at the display peak for
  any peak up to 10000 nits, and the toon clip weight turns it white; the limb (2 paper whites, about 400 nits before the shoulder) keeps
  the colour. Without HDR output the toon tonemap shows a white core and a coloured rim.
- **Glow.** No sprite: the disc writes the emissive mask (R = G, a bloom source that is no tile
  light) so the bloom bright pass takes `glare` of its colour. The halo is the bloom blur of the
  disc and goes with it when the disc is covered.
- **Night.** Hidden as the palette's day weight goes to 0 and below the horizon.

The gk log prints a `SUN-DISC:` line every 120 frames while it draws: level, hour, `arc` or
`palette`, direction, elevation, centre in render pixels from the top left, radius in pixels,
day weight, fog end and transmittance, colour, HDR or SDR values and the mask.

### Sky bodies (style 1)

The direction (2026-10-09): the sun and moon look like Jak 3's, not plain discs, drawn with
Jak 3's own sky textures (the derivative work was accepted). Style 1 (`pc-set-sun-option 20`,
the engine default) replaces the disc above on the same switch, arc and occlusion. Jak 3's sky is
read from its code (`$SANDOVER_DATA/research/jak3-sky/findings.md`). No green sun and no Day Star
in v1.

**Assets.** Five sprites from the user's Jak 3 disc (texture-only decompile), read at first use from
`$SANDOVER_DATA/sky/jak3` (data, never committed; `sandover-package` ships them under
`data/sandover/data/sky/jak3` and warns when one is missing). Without them style 1 draws the disc.

| File | Size | Use | Texture unit |
|---|---|---|---|
| `full-moon.png` | 128x128 RGBA | moon disc (alpha = coverage, PS2 alpha doubled); softened at load ("a little crunchy"): 4x bilinear upsample into a transparent margin (the disc fills its square, so the sprite's border cut it), colour smoothed by 1 source texel and the coverage by 2.5, trilinear with mips; the sprite grows by the margin (`moon_pad`) so the disc keeps its size | 51 |
| `sky-glow-soft.png` | 32x32 | sun glow, moon halo (red channel, peak 1) | 52 |
| `glow.png` | 128x128 | starburst | 53 |
| `glow-soft.png` | 32x32 | sun and moon wash | 54 |
| `sky-hotdot.png` | 64x64 | star dot (alpha) | 55 |

| Part | Size (degrees) | Colour | Level (paper white 200 nits, 1000-nit peak) |
|---|---|---|---|
| Sun core | white-hot centre of the glow, `g^8` of the glow sprite | white | 2000 paper whites HDR (12 SDR): lands at the display peak, 1000 nits |
| Sun glow | sprite 4.4 across (twice the 2.2 disc) | ramp, half the fitted sun colour | 3 paper whites HDR (600 nits), 1 SDR |
| Starburst | 15 across; turns with camera yaw | ramp x (0.3, 0.3, 0.15) | display units, Jak 3's colour x alpha: peak 0.3 encoded (about 14 nits over black) |
| Sun wash | 69 across | (0.31, 0.16, 0) orange | display units, Jak 3's (about 15 nits red over black) |
| Moon disc | 2.9 across (Jak 3's moon-to-sun sprite ratio) | the texture, blue-tinted grey | gain 2 HDR (0.85 SDR) on the texture: measured about 1 paper white (200 nits) mean over the disc with the night sky under it, blue-tinted (blue 1.6x red) |
| Moon halo | 3.4x the disc | (0.1, 0.2, 0.4) blue | display units |
| Moon wash | 35 across | (0.03, 0.05, 0.1) | display units |
| Stars | 928 dots over the sphere (about 512 above the horizon), 3 px at 1080p | white | 16 tiers, 0.04 to 2.6 paper whites (8 to 520 nits), most dim; 0.6x without HDR |

- **Colour ramp.** By the sun's elevation: deep orange (1, 0.38, 0.14) at the horizon to pale
  yellow (1, 1, 0.5) at 70 degrees, eased (`1 - (1 - t)^2`) so the orange stays near the horizon.
  Jak 3 ramps (255, 64, 64) to (255, 255, 128).
- **Two passes.** The glow, core, moon, halo, moon wash and stars draw after the opaque background
  on sky samples only (depth test). The starburst and the sun wash are a camera effect (
  2026-10-09: the flares above everything except the UI): `render_flare` adds them over the
  finished scene after bloom and before the HUD buckets, with no depth test, scaled by the sun's
  eased visibility, so a sun behind a palm keeps its glow cut by the fronds while the flare dims
  as a whole instead of being cut along the silhouette. Off screen the visibility is not measured,
  so the flare fades out as the sun leaves the frame by the starburst's radius.
- **Haze.** The core and glow take the disc's transmittance along the sun ray. The flare takes the
  same ray's transmittance to the 0.35 power (constant over the frame, so neither geometry nor the
  horizon haze cuts it). The halo and moon wash take each pixel's ray, so they sink into the haze.
  Stars take it by their own elevation and fade in from 3 degrees below the horizon.
- **Bloom.** The glow writes the emissive mask (R = G) at `glare`, scaled by the sun's haze
  transmittance: the halo past the sprite is real bloom, dimmer through thick haze.
- **Sun at night.** The sun stays on the arc by the hour (below the horizon at night) with no day
  weight, so it draws nothing; the night log shows that direction, not an unset one.
- **Moon.** At `moon_disc_direction`'s light (the night slots' fitted direction, fitted per slot), so the
  sky moon and the night shadows agree. It shows by the level's night weight (palette weight of
  the night slots, 0.05 to 0.3) and its elevation (-1 to 0.6 degrees), not by Jak 3's 17:00-07:00
  window; a level without night slots has no moon.
  No phases. Misty has no sun (its one day slot points below ground) and keeps its moon.
- **Stars.** Only at night (night weight 0.4 to 0.9, and no day weight) and only on sky samples;
  the field turns 15 degrees an hour about a tilted pole.
- **Occlusion.** As the disc: one eased visibility value per body (sun, moon) from the same
  21-tap pass, held while the body is off screen.
- **The game's own sky particles.** Jak 1 draws its sun and stars as particles (`time-of-day.gc`:
  `group-sun` 06:15-18:45, `group-green-sun` 21:45-10:15 where the level's `sun-fade` is not 0,
  `group-stars` at night, 85 in training, village1, beach, jungle, misty, firecanyon and
  finalboss). While our sun (disc or bodies) actually draws in the viewer level (gain above 0:
  day weight and elevation), `pc-get-sky-bodies` bit 1 stops `group-sun`; while our stars draw,
  bit 2 stops `group-stars`. Where ours draw nothing (misty's sun, dawn and dusk before our gain
  rises, missing sprites) the game's particles stay. The green sun stays. With the sun switch off nothing changes (vanilla).
  OpenGOAL's vanilla look draws both (`sky-sun-moon` shots, `vanilla/`, particles on): at beach
  07:00 a large sun glow over the sea (sun count 1, `*sky-parms*` sun 0 at 7 degrees up), at
  village1 and jungle 23:00 85 star dots; the green sun is spawned at both hours (count 1) but
  was not in frame. With the play look the counts read 0 (stars stay in the disc arm, which has
  no stars of its own).

| Form | Effect |
|---|---|
| `(pc-set-sun-option 20 s)` | 0 the disc, 1 sky bodies (default) |
| `(pc-set-sun-option 21 b)`, `22 w` | starburst and wash strength, 1 = Jak 3's |
| `(pc-set-sun-option 23 g)`, `28 g` | sun glow in paper whites, HDR (3) and SDR (1) |
| `(pc-set-sun-option 24 m)`, `25 m` | moon disc in paper whites, HDR (2) and SDR (0.85) |
| `(pc-set-sun-option 26 s)` | star brightness (1; 0 = none) |
| `(pc-set-sun-option 27 d)` | moon diameter, degrees (2.9) |
| `(pc-set-sun-option 29 1)` | 20-28 back to their defaults (`sandover-settings-reapply`) |
| `(pc-get-sky-bodies)` | what drew last frame (gain above 0): 1 our sun, 2 our stars (0 with the switch off) |

The settings page has no row of its own: the Sun row turns the sun, the moon and the stars on and
off together. Presets: `sandover/presets/sky-bodies.toml` (`jak3`, the chosen look; `disc`). Shot
manifest: `sandover/shots/sky-sun-moon.yaml`. The gk log prints `SKY-BODIES:` every 120 frames:
level, hour, sun and moon direction, elevation, centre in pixels from the top left, gains and
transmittance, night weight and star gain.

## 9. Light shafts (Jak 1)

The remaster hides the game's fake sun rays (2026-10-08, beach grotto: "remove all of those
fake volumetric shafts where they appear in the game"). They are not level geometry: they are
sprite particles with the shared `effects/flare` texture, anchored at one edge (`:rot-x 4`),
15-30 m long, fading in and out over 8 s. Because `flare` also draws every orb, crate, cannon and
boss glow, a texture key or material class cannot select them; the launcher's specs do.

`sp-light-shaft?` (`sparticle-launcher.gc`): edge-anchored, no velocity or acceleration, `scale-x`
at least 10 m, `timer` at least 4 s. The `launch-particles` macro skips such launchers while
`*sp-hide-light-shafts?*` is set; rays already launched fade out on their own (at most 8 s).
`python -m sandover.light_shafts` runs the same rule over every `defpart` in `goal_src`:

| Part | Level | Launched by | Spawners | Texture | Size, life |
|---|---|---|---|---|---|
| 666 | beach | state `beach-part-grotto-1` (part-spawner `group-beach-grotto-1`), the grotto | 1 | `effects/flare` | 15-20 m, 8 s |
| 864 | jungle | `group-jungle-dapple-light-1`, 06:00-17:59 | 1 | `effects/flare` | 15-30 m, 8 s |

Two shafts are level geometry, both flat white textures lit by vertex colour in a
`tfrag-trans` tree: jungleb `junb-white` (combo `0x03c60000`: a light cone under the plant boss
room's ceiling opening, and a ring of 11 panels 18 m tall in the same tree) and misty `e-white`
(`effects` combo `0x00020001`: the yellow beams out of the arena towers' eye lamps, day and
night). **The misty tower beams stay drawn by decision (2026-10-09, "keep the tower
beams"): never tag `e-white` `shaft`.** Only `junb-white` carries the materials tag `shaft`
(correction CSV in `$SANDOVER_DATA/materials/corrections/`, applied with `sandover-materials
apply --csv`; a fresh data folder lacks it until re-applied, as with every correction row:
`materials.md`, "fake light shafts carry `shaft`"), which sets bit 0x10 of the texture's class byte (`kPbrShaftBit`;
classes now live in the low 4 bits), and TFragment skips such draws while the class shading runs
(`pbr_active`). Only tfrag reads the bit, which covers every case: no tie or shrub draw in Jak 1
blends except the envmap shine pass.

Geometry census: every alpha-blended or additive tfrag, tie and shrub draw in every
level's `.fr3`, read from the draw modes. The game has 18 such textures:

| Level | Texture | Draws | Blend | Verdict |
|---|---|---|---|---|
| jungleb | `junb-white` | 2 | alpha | shaft, tagged |
| misty | `e-white` | 1 | alpha | shaft (arena eye-lamp beams), kept by decision; its only background draw |
| beach, village2 | `bch-water`, `ya-water` | 1, 3 | alpha | water |
| jungle, village1, village2 | `jng-beach-01path`, `vil-beach-01`, `vil-beach-01path`, `vil2-beach-01` | 1-2 each | alpha | sand blend layers on the ground (dirt trees) |
| village2 | `v2-burnt-ground`, `vil2-burned-ground-small` | 2, 1 | alpha | scorch decals |
| snow | `snow-ice` | 5 | alpha | ice |
| sunken, sunkenb | `sun-glass`, `sun-glass-frame-01` | 2-3 each | alpha | glass domes |
| village2 | `vil2-glass` | 2 | additive | glass |
| village1, village2 | `oracle-glass-eyes` | 1 each | alpha | oracle eyes |

Kept on purpose: short flare bursts (collectables 241-253, crates, misty cannon, robotboss and
sage beams, eco-room activations in jungleb and snow: all 2 s or less) and lightning. Actor
(merc) geometry is not covered by the tag.

| Form | Effect |
|---|---|
| `(pc-set-hide-shafts 1)` | hide the shaft particles (sent with the Materials switch, `pbr?`: the remaster look; no row of its own) |
| `(pc-set-hide-shafts 0)` | draw them (boot, vanilla) |

