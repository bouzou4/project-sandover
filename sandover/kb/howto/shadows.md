# Actor shadows how-to (Jak 1)

Spike: shadow maps cast by actors only, received by the world and by the merc actors. Default
off (the vanilla stencil shadow; nothing else changes). Findings:
`kb/findings/2026-09-23-shadow-spike.md`.

## Turn it on

- GOAL: `(pc-set-shadows setting strength)`: 0 off (vanilla stencil shadow), 1 on (sun and
  local-light maps, every merc and generic actor casts; local lights light the merc actors). It also sets `*pc-shadow-caster-margin*`
  (16 m when on, else 0) so off-screen actors whose shadow can reach the view are still drawn
  (see Culling below).
- Fine controls, values in millionths: `(pc-set-shadow-option which value)`

| which | Effect |
|---|---|
| 0 | setting (as above, without the culling margin) |
| 1 | strength: scales the removed light share, 1 = the estimate |
| 2 | debug view: 1 occlusion, 2 sun share, 3 sun-map coordinates, 4 local lights (merc: grey plus each light's contribution, light 1 red, 2 green, 3 blue, 4 yellow; background: grey, map 0 coverage red, map 1 green, black where an actor blocks that light) |
| 3 | range (m) of the view frustum the sun map covers (default 40) |
| 4 | local light power scale (default 1): actor lights and lamp shadows |
| 5 | landing disc under Jak: strength (default 0.3, 0 removes it) |
| 6 | caster reach (m): casters higher than this above the receiver cast nothing (default 8) |
| 7 | local lights lighting the characters, 0-4 (default 4); the first two cast; 0 = no local lights or lamp maps |
| 8 | local light falloff r0 (m): power / (1 + d² / r0²) (default 5) |
| 9 | local light reach (m), smooth window to 0 (default 16) |
| 10 | darkcave: share of the level light the characters keep away from lit crystals, and the weight of the base palette slot (0) the cave's tfrag, tie and shrub keep (default 0.2; 1 = vanilla). Independent of option 7 |
| 11 | canopy shade strength (default 1, 0 = off): under overhead foliage every surface (ground, walls, actors) is darkened and cooled the same way, with sun patches through the leaf gaps that move with the sun. Needs `$SANDOVER_DATA/lighting/<level>.canopy` from `sandover-unbake canopy`; see `background/Canopy.h` Runs by day only (palette day weight); actor sun shadows show only inside the sun patches; emitters keep their light; also wind TIE (palms) and generic props. Map array on unit 30 (sampler `canopy_tex`). |
| 12 | canopy: darkening of the shade under full cover, 0..0.8 (default 0.45: the shade keeps 55% of today) |
| 13 | canopy: sun patch edge, 0 soft .. 1 hard (default 0.4) |
| 14 | canopy penumbra width per metre of leaves above the receiver (default 0.03) |
| 15 | canopy: cool shift of the sky light under the canopy, 0..1 (default 0.5) |
| 16 | canopy: sun patch brightening over today, 0..0.5 (default 0.2, warm) |
| 20 | terrain sun cover (default 1, 0 = off): an actor whose path to the sun is blocked by the world (roof, tunnel, cliff) loses the sun, and its sun shadow fades by the same amount (below) |
| 30 | sky-hued shadows: 1 on, 0 off (default on; acts only with actor shadows on, so the boot state stays vanilla). See below |
| 31 | sky hue strength (default 0.5, the picked `moderate`): 0 grey, 1 the fitted sun/ambient split; `sandover-preset apply sky-hue <moderate\|off>` |
| 32 | world sun map: tfrag, tie, shrub and wind tie take it (default 1; 0 = their sun shadows are the bake's alone). See World sun map below |
| 33 | actors take the world map per pixel in place of the terrain cover (option 20, default 1; 0 = the heightfield cover) |
| 34 | water may read the world map through `ws_water_vis` (default 1) |
| 35 | world map size per cascade: 1024, 2048 or 4096 texels (default: the quality tier's, option 40) |
| 36 | world map cascades, 1-3 (default: the quality tier's, option 40) |
| 37 | world map penumbra per metre between blocker and receiver (default 0.012) |
| 38 | distance from the camera the last cascade reaches, m (default 120) |
| 39 | 32-38 back to their defaults, 35 and 36 to the current quality tier's (`sandover-settings-reapply` sends it) |
| 40 | quality (default 2): 2 high = 2048² sun map, 512² local maps, 3x3 PCF; 1 medium = 1536², 384², 2x2 PCF at half-texel offsets; 0 low = 1024², 256², one tap (the hardware 2x2 compare). It also sets the world sun map (35, 36): low 1024² x 2 cascades, medium 2048² x 3, high 4096² x 3. The maps are reallocated on a change; the settings menu's Shadow quality row |

Canopy shade looks (the moderate look is the chosen default): the defaults above are the moderate look, so play
shows it with no forms. The strong look (shade at 38% of today, brighter patches) is
`(pc-set-shadow-option 12 620000) (pc-set-shadow-option 13 850000) (pc-set-shadow-option 15 1000000)
(pc-set-shadow-option 16 350000)`, or the same values on the Shadows debug sliders.

- ImGui: Renderer Debug → **Actor shadows** (same settings, plus the live level, sun direction
  source, caster count and flame weight).

## What it does

1. Before bucket 0, `ActorShadows::prepass` reads the camera and mood weights from the first
   tfrag bucket's PC port data, then walks the frame's DMA a second time through the actor
   buckets and draws them depth-only into the maps (`Merc2::render_shadow`,
   `Generic2::render_shadow`, shaders `merc2_shadow`, `generic_shadow`).
2. Sun map: 2048², orthographic along the fitted sun (the `.unbake` slots blended by the live
   palette weights, `unbake_sun_summary`), fitted to a sphere around the first `range` metres
   of the view, extended 60 m toward the sun, centre snapped to whole texels.
3. Local lights (#237, #191, #266): `world_lights::select_actor` takes the rows of the world
   light table (`WorldLights.inc`, `kb/howto/lights.md`) that are on within the reach of Jak and
   ranks them by the luminance they bring to his centre, `lum(power) / (1 + d² / r0²)` windowed
   to 0 at the reach (a light already casting ranks 1.5x higher, so maps do not pop between
   neighbours). Eligible: lamps (static E on night and flame slots, village1 `hutlamp`s on the
   flames slot), fire part spawners (torches, fire pots, flame pots: colour red-dominant, group
   strength at least 0.75), strong emitters (vents, power cells, lit crystals, dark eco crystals
   and boxes, lava) and glow props (ice crystals, cave mushrooms, #267); not eco,
   crate contents, arcs, moths, butterflies, birds or spray. Power in merc vertex-colour units
   (0.5 = the texture as drawn): lamp 1.2, strong 0.8, fire 0.6 x group strength, glow 0.8 (its
   shadow is the tile pass's own: see below), times colour,
   gain and state; a near-white lamp texture (clipped glass) lights as incandescent (1, 0.72,
   0.42). The best four light merc actors (below); the first two also get a 512² spot map from
   the light aimed at Jak (half angle atan(2.2 m / d), 20-75°), so he and whoever stands next
   to him cast. Entity-gated rows (vents, crystals) are queued for GOAL with the tile lights'
   queries, which now also run while shadows and local lights are on.
   Lights the bake does not hold (glow props, dark eco crystals: `ActorLight::added`) cast
   differently: the receivers leave their baked light alone (`shadow_lamp_added`), and the tile
   pass multiplies that light's own contribution by the map (`emissive_light.frag`, the world
   light's alpha = 2 + map index), so a shadow removes exactly the light it adds and never goes
   below the frame without it (crystals-004 measured -20 to -41% with the baked-share rule).
   Their spot opens to 69 deg (half angle 1.2 rad) instead of the 2.2 m around Jak, so the crates,
   chests and enemies around the light cast too. Budget: the sun map (2048²) plus two 512² local
   maps, given to the two local lights that bring Jak the most light (lamps, fires, strong
   emitters, glow props and dark eco crystals alike, the casting ones ranked 1.5x); the other two
   of the four local lights light merc without a map. Each map redraws the actors in its frustum;
   cost is in the `actor-shadows: ... pre-pass gpu` log line (0.11 ms GPU, 0.46 ms CPU at 2560x1440
   in the crystals-003 lavatube log, both maps).
4. Background receivers (`tfrag3`, `tfrag3_sun`, `shrub`, and the envmapped-tie passes
   `etie_base` and `etie`) multiply their colour by
   `1 − strength · (s_sun · sun_share + Σ s_lamp · lamp_share)` with 3×3 PCF on hardware
   compare taps. The sun occlusion only counts casters within the reach, a height above the
   receiver converted to distance along the ray by the sun's elevation, so a floating orb does not throw a shadow far away while a long
   evening shadow keeps its head. Sun share: per vertex from the sidecar (`k·v·max(0, n·L)·Lgt` over the fitted
   light, blended to the global estimate by confidence q); without sidecar data a global
   estimate. Lamp share: twice the local light's estimate at the receiver, same falloff, over the
   receiver's baked light, capped at half of it (a soft shadow that still reads). Merc actors remove light 0 and 1 (the two sun snapshots) of their own light rig
   by the sun occlusion, then add each local light, `power x wrapped N.L x falloff x (1 - map
   occlusion)`, soft-limited to 0.6 luma and tinted by the vertex colour; emitter models (orbs,
   cells, hut lamps, cave and dark eco crystals: `Merc2::is_light_emitter`) keep their vanilla light. `shadow_cnrm` already points outward in the
   `shadow_cpos` space (the position is divided by w = -1; `pbr_merc_vsign` only applies to the
   undivided PBR position). The log prints `local-lights:` with each chosen light (level, class,
   gate, actor id, position, distance to Jak, state, power, light at Jak, map) when the set
   changes, and every frame in view 4. The
   tile pass's world lights skip characters (stencil bit 6) while local lights are on, so Jak is
   not lit twice; the screen tiles still reach him. In darkcave (`neighbours::viewer()`) the merc
   level light is scaled by `cave_dark + (1 - cave_dark) x glow` (emitter models and textures in the
   actor emissive table, and their envmap shine, are exempt), glow = the strongest lit
   crystal's own glow (its `color-mult`, the value its palette fade uses) faded from 5 m to 70 m,
   the fade the game applies to the actors it connects to crystal light (`cavecrystal-light.gc`).
   It is computed per fragment from the lit crystals' positions (`cave_dark_frag.glsl`), so an
   actor lights by its own distance to a crystal, not Jak's; only the `cave_dark` base eases over
   0.3 s, so crossing into the cave does not pop. The GOAL crystal queries run while shadows are on and either
   local lights or the cave factor are in use. The cave's background keeps `cave_dark` of its base
   palette slot 0 (`cave_dark_itimes`); `update-mood-darkcave` puts each crystal's light in slots
   1-7 as palette fades, so rock lit by a crystal keeps that light.
   Envmapped ties (shiny precursor metal) take the global sun share and the same `1 − E` gate as
   `tfrag3`; their additive reflection pass is scaled by the sun term only (issue #172).
5. Emissive receivers: `tfrag3`, `tfrag3_sun`, `etie_base` and `etie` scale the whole shadow term by `1 − E`, with
   E the emissive layer's per-source weighted value (`em_value`). While the maps are on, the
   emissive uniforms are set even with the emissive mask off (no draw buffer then).
6. The stencil shadow (bucket 47) is skipped while the maps are on. In its place a faint soft
   disc (0.45 m) straight below Jak darkens background receivers, fading over a 20 m drop,
   so the landing spot reads at any sun angle (option 5). It is not drawn on actors. Every
   actor that loses its blob casts into the map.
7. Terrain sun cover (option 20, `actor_shadows_cover` in ActorShadows.cpp): one value per merc
   actor, computed along the sun ray as the canopy projects (`shadow_canopy_light`): from the
   actor's root (first bone) five rays (feet 0.5 m, waist 1 m, head 1.6 m, and 0.4 m to either
   side at the waist) march toward the sun in 1 m steps out to 40 m through the level
   heightfield; cover is the share of rays that hit solid world, eased, times the option. The
   rise is clamped as the canopy's (`sun / max(sun.y, 0.1)`), and the cover fades out with the
   sun as the canopy shade does (sun.y 0.25 to 0.1, about 14 to 6 degrees), so the two fade
   together and a twilight sun never marches sideways. The sun map's caster colour is on
   texture unit 44 (`shadow_caster_tex`, set explicitly). Option 20 is reset to 1 by
   `sandover-settings-reapply`. Solid is
   `LevelHeightfield::solid_at`/`solid_span`: below the column's top and outside its open span,
   the tallest gap of 2 m or more between surfaces (64 height bins per column, built at load
   from every collision triangle sampled half a texel across and 0.5 m up, so walls fill their
   column). A ray that passes under an arch or a roof and out the other side stays lit (Jak in
   the sun under the beach arch 31 m up); a ray that leaves a tunnel or hut through its wall or
   roof is blocked; each step tests the whole vertical segment, so a thin roof between two
   samples still counts. Merc takes `max(actor map, cover x sun share)` as its sun occlusion
   (`shadow_cover`, set per model), so the whole actor loses light 0 and 1 and the key light,
   never per pixel. The sun share (`Merc2::sun_share_of_rig`) keeps the game's designed light
   kits: the time-of-day rig (light group 0) has no third light, while the kits of
   `make-light-kit` (hut interiors through `set-target-light-index`, NPCs with their own
   `light-index`) add a down light, so a rig whose light 2 carries 15% or more of lights 0 and 1
   is a kit and keeps its lights; Jak in his hut keeps kit 7. The sun map gets an R8 colour
   target holding `1 - cover` of the nearest caster (`caster_sun` in `merc2_shadow`, 1 for
   generic casters); each PCF tap multiplies its occlusion by the smallest of the four texels it
   compared (`textureGather`, unit 44), so a covered caster's sun shadow fades whole, penumbra
   and core alike. The landing disc (option 5) is not changed by cover. Local lights and their
   spot shadows (item 3) are unchanged. Levels without a sky have no heightfield: cover 0. Known
   misses: roofs without collision (village2's blue sage hut has none, so its interior reads as
   open); one open span per column (a stilt hut over open ground reads as solid under its
   floor); cover is the world's, not the bake's: where a cliff stands between an actor and the
   sun but the original bake keeps the ground lit, the actor darkens on lit ground; generic-
   renderer actors keep the sun. Agreement with the bake: `kb/findings/2026-10-01-terrain-sun.md`.
8. Stereo: the pre-pass runs once per frame (skipped on the second eye); the maps are in
   world space and the merc camera space is mono.

## World sun map

The static world casts sun shadows too (`WorldShadows.{h,cpp}`, `world_shadow.vert/.frag`,
`world_shadow_frag.glsl`, `world_shadow_vert.glsl`; design: `kb/findings/2026-10-08-world-sun-shadows.md`).
Drawn in the actor pre-pass (after the actor maps) while actor shadows are on and any of options
32-34 is set, so the boot state stays vanilla.

- Casters: tfrag (normal, trans, dirt, ice trees), tie (normal and trans, their envmap base draws,
  wind ties with their unbent instance matrix) and shrubs of every loaded level, geometry LOD 0,
  depth only. A draw with an alpha test discards below its alpha reference (plain alpha blending
  below half); additive, subtractive and half-transparent draws, draws without depth write and
  animated-slot textures' alpha do not cut. A plain alpha-blended draw whose texels that pass its
  reference are mostly under 0.9 opacity is a translucent sheet and casts nothing (on today's
  levels only snow's `snow-ice` overlay; the gk log lists them per level, `translucent sheets`). Culled per cascade box by the trees' BVH (shrubs in
  runs of about 1536 indices, wind instances by a 20 m sphere). Depth clamp on, polygon offset
  (1.5, 2).
- Direction: `sun_disc_direction` (SunDisc.h), the disc's arc by the game hour; the actor sun map
  takes the same direction while any of 32-34 is on. At night the light is the moon
  (`moon_disc_direction`: the sidecar's night slots, header `night_mask`, their fitted direction
  blended by the palette). Each light fades by `world_shadows_sun_strength`: its slots' palette
  weight (0.05 to 0.3; day slots for the sun, night slots for the moon) times its elevation (y 0.03
  to 0.08), so a sun past the horizon or a moon fit below it (swamp, rolling) casts nothing. The
  stronger light casts with the difference of the two fades, so dusk and dawn hand over through
  zero, never with a pop. Every consumer takes the one light and fade: the world map, the actor
  sun map (`shadow_sun_gain`), the roof cover and the canopy (on top of their own low-light fades).
  The removed share counts only that light's slots (`ub_sw` in `tfrag3_sun.vert`): by day the day
  slots, at night the night slots, so the moon removes only its own baked direct light. The PBR sun
  term (Materials) blends the day slots and the above-horizon night slots' fitted light the same
  way. Day and night slots come from the sidecar header (`day_mask` without `night_mask`); the arc
  places each slot at the circular mean of its peak hours (`MoodTable::slot_hours`). Moon direct
  share (lgt over lgt + amb, luminance, per night slot): beach 0.37/0.48, village1 0.26/0.38,
  jungle 0.48/0.57, training 0.22/0.28, misty 0.82, firecanyon 0.18, snow 0.23, village2 0.60,
  village3 0.70; swamp and rolling below the horizon.
- Cascades: one 32-bit depth array (units 49 `ws_map` with compare, 50 `ws_raw` without, sampler
  objects); splits at range/12, 0.29 range and range for three, range/6 and range for two. Each box
  bounds the sphere of its view slice (the field of view from the game's perspective), its centre
  snapped to whole texels; 300 m of depth toward the sun.
- Receivers (`ws_sun_vis`): the first cascade holding the point, blended into the next over the
  outer band of the box (84-98% of its half width), the last into no map; a normal offset of one
  texel along the screen-space geometric normal turned toward the sun (sliver triangles, whose
  derivatives are near parallel, take the sun direction and a 2-texel bias instead of the plane); receiver-plane depth bias from the screen derivatives
  (`ws_begin(p)` at the top of `main`, before any discard); an 8+1 tap blocker search, then 4
  compare taps when nothing blocks, or 16 Poisson compare taps over a penumbra of
  `max(1.5 texels, option 37 x blocker distance)`. With perf-opt bit 28 (default on) the outer
  cascades (1 and 2) search with 5 depth gathers (the centre and one ring at the search radius, 20
  texels; a thin occluder between ring points is missed, which master's 8 reads at 0.5-1 radius
  would partly catch), return lit when none blocks and take 8 zero-mean compare taps otherwise.
  On a 4096 map the first cascade is drawn into a 2048 corner of its layer (texel about 1 cm;
  `ws_par.z` is the share of the layer it fills); its normal offset, constant and slope bias stay
  at the 4096 texel. `kb/findings/2026-10-10-opt-shadow-kernel.md`.
- Background (option 32): `occ = max(actor map, 1 - V)` and the removed share is
  `max(share - (1 - occ) * share_lit, 0)` (`ws_removed`): `share` is the sidecar's direct share as
  baked (`tfrag3_sun.vert`, zero where the bake is in shadow), `share_lit` the same with every
  slot's visibility at 1. Receivers without sidecar data use `clamp(1 - ws_amb / lum(colour))`
  for both (`ws_share_est`: the level's median ambient gain from the sidecar records times the
  live fitted ambient). Canopy shade and the sky hue apply as before. Option 32 off is the master
  path exactly.
- Actors (option 33): merc takes `occ = max(actor map, (1 - V) x rig sun share)` per pixel
  (`shadow_rig_sun`, `Merc2::sun_share_of_rig`: kits keep their lights; emitters 0) and no terrain
  cover is computed (`fr.terrain` 0, so caster colour 1). Local lights and lamp maps are unchanged.
- Water (option 34): `ws_water_vis(p)` in `world_shadow_frag.glsl` returns the map's
  visibility at a water pixel, or -1 where the map is off or no cascade covers it. The water sun
  glint reads it (`wsg_sun_vis`, open where -1): the ocean (`ocean_common.frag` adds
  `WORLD_SHADOW_FRAG`, the pixel ray on the water plane in world units,
  `world_shadows_set_uniforms(rs, program, false)`) and inland water (merc camera space, the merc
  receiver uniforms). Water lookups scale the penumbra and blocker search by `kWsWaterPen` (6).
  Option 34 off leaves the glint unshadowed (`lighting.md`, sun glint).
- Log: `world-shadows: hour H, palette sun (...), N cascades of S (texel ...), sun (...) fade F, D draws T tris, amb floor ...,
  pass gpu X ms cpu Y ms` every 120 frames (GPU from its own timer query; 2.2-3.3 ms at 2048 x 3 in
  world-sun-shadows-001, an item for the optimization pass).
- A/B forms: today's look `(pc-set-shadow-option 32 0) (pc-set-shadow-option 33 0)`; the map on
  is the default. Debug: Renderer Debug -> Actor shadows -> World map rows.

## Sky-hued shadows

In a level whose `level-info` has `:sky #t` (the viewer level, `LevelSky.inc`, generated by
`sandover-levelinfo --write-inc`), a shadow keeps the sky's light, so it darkens toward the
ambient's hue instead of toward grey. Data: the sidecar's fitted sun (`lgt`) and ambient (`amb`)
colours of the live palette slots (negative fit values clamped to 0), blended as
`unbake_sun_summary` blends them. For a removed sun share s at a receiver, channel c loses
`s·L_c / ((1 − s)·A_c + s·L_c)` (L, A at luminance 1): with an orange sun and a blue ambient
(snow, dusk) red goes and blue stays. Option 31 blends these weights from grey (0) to that split
(1). `shadow_keep` (`tfrag3`, `shrub`, `etie_base`, `etie`) then rescales them so the receiver's
own colour loses exactly the luminance it loses with the option off, and pulls them toward grey
just enough that no channel loses more than all of it (no clamp, so a full shadow never comes out
lighter than grey). Lamp shadows and the landing disc take the same hue only where the sun
reaches (`smoothstep(0.02, 0.2, share)`), so interiors keep grey. SSAO stays grey: AO removes sky
light, so a hue toward the sky would point the wrong way. Merc receivers were already right:
they subtract the sun lights of their own rig and keep its ambient. Needs actor shadows on; off
(or strength 0) is the master path.

## Casters

| Path | Buckets | Casts | Notes |
|---|---|---|---|
| merc | 10, 17 (level props), 45 (after alpha), 49, 52, 55 (characters, enemies, pickups) | yes | depth-writing draws only (translucent and water draws skipped) |
| merc emitters | any | no | `money` (precursor orbs), `fuel-cell`, `hutlamp`, `cavecrystal`, `dark-crystal` models: they give light, and a lamp would fill its own spot map (`Merc2::is_light_emitter`) |
| beach windmill-one | any | no | `windmill-one` (prefix match): all three beach instances (`windmill-one-2` at the cliff, `-11`, `-12`), plus their vista landmark copies. The model is about 29 m tall, so the caster reach keeps only fragments of its base; at 15.7 h the fitted sun reaches the cliff wheel through the rock (#259). General rule for tall casters: #290 |
| merc eyes | 54 | no | drawn on top of the eye sockets, would double the head |
| generic | 11, 18, 50, 53, 56 | yes | no alpha test; HUD geometry skipped |
| generic alpha / water, sprites, particles | 46, 59, 62, 66 | no | translucent, or eco and effects (glowing plasma: no shadow) |
| background (tfrag, tie, shrub, incl. grass and foliage) | 5-30 | no | their shadows on the world are in the bake; see the finding for foliage onto actors |

## Culling

GOAL culls actors against the view frustum in `dma-add-process-drawable`. With shadows on,
`pc-shadow-caster-visible?` also accepts an actor whose bounding sphere, swept along the
vanilla shadow direction (`*time-of-day-context* current-shadow`) by the margin, touches the
frustum. Such actors are drawn off screen and cast, with `*pc-shadow-only-draw*` set: they do
not get `was-drawn`, do not change their LOD, do not queue matrix-engine joint updates and do
not touch the level's closest-object or texture masks, so AI and game state see the vanilla
result. Actors beyond the level's draw distance, or not spawned, still pop in.

Verified with `sandover/shots/shadows-gameplay.yaml`: 600 frames at beach-start and
jungle-start with a stage hook that prints every actor's position; off and on match.

## Rig captures

```sh
cd sandover/tools
SANDOVER_ROOT=<worktree> uv run sandover-capture run <worktree>/sandover/shots/shadows-v1.yaml \
    --bin <staged> --goal "(pc-set-shadows 1 1.0)" --out-dir $SANDOVER_DATA/experiments/p2-shadows/v1/sun
```

A worktree run points `SANDOVER_ROOT` (goalc, `(mi)`) and therefore gk's project path at the
worktree, which holds real copies of `out/jak1`, `iso_data` and `decompiler_out/jak1`
(gitignored; no links). gk then reads the worktree's shaders and GOAL build.

## Cost

The log prints `actor-shadows: ... N lights, pre-pass gpu X ms cpu Y ms` every 240 frames (GPU
from a timer query around the pre-pass, CPU wall time of the second DMA walk and draws). The perf
harness (`pc-set-perf-timing`, `kb/howto/perf.md`) times the pre-pass as `actor-shadows` (sun map
plus up to two 512² local-light maps) and the local lighting inside `merc` (four lights per merc
fragment, no extra pass). Local lights off for an A/B: `(pc-set-shadow-option 7 0)`.
