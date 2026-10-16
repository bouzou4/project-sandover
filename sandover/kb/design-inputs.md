# Design inputs (running list of agreed items)

Items agreed in conversation that are not yet covered by an ADR. Each is meant to move into an ADR or a how-to.

| Topic | Decision or input | Where it lands |
|---|---|---|
| Human gate | One gate only: the design-doc review. Progress goes to the board and the Status issue; pings only when action is needed. | Process |
| Rendering | No deferred, no TAA; light pre-pass for point lights; MSAA stays; temporal only inside SSAO (ADR-0013). | SSAO, lighting |
| SSAO composite | After the actor buckets with a stencil mask; bucket-30 depth feeds the light pre-pass. | SSAO |
| FP16 target + tonemap | Part of the lighting phase behind a toggle; the final `do_pcrtc_effects` pass is the tonemap point. | Lighting |
| HDR desktop output | Optional late item via a DXGI swapchain and GL interop; ships with an in-game calibration screen (paper white, peak, black level test patterns) saved into the preset. Not trusting OS monitor metadata. No HDR in VR. | Output options |
| Vista | Early spike, late implementation (ADR-0014). Fog handling is an art call: expose as preset axis (push fog out vs vista-own fog). Stand-ins for the same place must be hidden. | Post-stereo phase |
| Review viewer | three.js viewer with orbit, texture-set switching, point light; lightbench turntable for engine-exact frames (ADR-0015). Needs merc→glTF export (stereo checks). | Human loop |
| Texture baseline | Existing 4x pack is the private baseline; normalize alpha to 128-opaque; newer upscalers compared by the pick loop on style replication (ADR-0016). | Asset phase |
| Material classes | Pilot suggests a 6th class for character/organic-non-plant textures. | PBR phase |
| Audio | Optional post-stereo item: per-sound world positions exist; HRTF via OpenAL Soft or Steam Audio; Windows Spatial Sound for Atmos/DTS decoders the user owns; VR listener from headset pose. | Optional phase |
| VR target | Any OpenXR headset via SteamVR or another runtime; no device-specific assumptions; budgets from the runtime's recommended swapchain size. | Stereo/VR |
| Temporal prerequisites | Keep previous-frame view-projection; note merc previous-frame positions; DLSS/path tracing not planned on OpenGL. | Architecture |
| Determinism | Fixed-step capture mode, fixed camera per shot, camera logged in sidecars so lightbench and the rig match. | Rig |
| Multi-instance | Game instances run concurrently via port slots and per-build binary copies; GPU cost measurements solo. | Tooling |
| Build | Jobs = logical threads − 2 at run time; sccache opt-in; never re-run configure. | Tooling |

## Review decisions (2026-09-23)

| # | Decision | Outcome |
|---|---|---|
| 1 | Lighting scope | Option 2: detail relighting plus the moving sun. Start with whatever is easiest; run a visual experiment with the moving sun afterwards; the sun can be disabled if it does not look good. |
| 2 | Vista fog | Not decided from text: build the unified-fog experiment and show captures of the three options before choosing. |
| 3 | Sixth material class | Yes: add a character/organic class. |
| 4 | Night-slot lighting | Experiment: show the fitted moon arc before a final yes/no; must not block other work. |
| 5 | Frame-time target | Not a concern yet; measure and report, no gate. |

Additional direction from the review:

- **Options are a communication device, not the product.** Presenting five options exists so the maintainer can say "I prefer D" instead of iterating "change X" five times. The end product is one opinionated look, not a set of shipped presets. Presets remain an internal mechanism for comparisons; the final config is a single look.
- **Give context with options.** When presenting choices, explain briefly what differs and why one might look better, so the pick is informed; the maintainer may explain their preference back.
- **Jak 2/3 builds are checked at the end**, by bisect if needed, not on every PR. Remove the per-PR "Jak 2/3 still compile" check.
- **Spatial audio is in scope, not optional:** surround (5.1/7.1 multichannel) plus spatial output (Windows Spatial Sound for Atmos/DTS:X endpoints) plus binaural HRTF for headphones. Promote spatial audio to a real phase.
- **HDR calibration:** copy established industry practice for in-game calibration screens (paper white, peak brightness, black level test patterns), no invention.

## Art direction for materials (2026-09-23)

- **Target look:** stylized and cartoonish, faithful to the original art direction. Reference: current-generation Ratchet & Clank (clean, stylized PBR with readable materials), not a photoreal remake aesthetic. Materials must not fight the original's shapes and colours; detail normals stay subtle and class-driven.
- **Fur:** plan a shell-rendering fur shader for characters like Daxter and the lurkers (layered shells on the merc path, alpha-tested, instanced draws; GL 4.1 compatible). Treat as its own PBR-phase item with its own presets and a cost budget; the pick loop judges it against the vanilla character.

## Findings from viewer reports

- Merc's per-vertex `COLOR_0` is a constant art-time tint multiplied onto separately-computed runtime lighting, not baked lighting itself (opposite of tfrag/tie); character albedo/relighting/PBR work must keep this tint term, and the viewer should default characters to tinted preview — see `kb/findings/2026-09-23-merc-vertex-color-semantics.md`.

## Experiment verdicts (2026-09-23, from the maintainer)

| Experiment | Verdict | Consequence |
|---|---|---|
| Moving sun | Differences are extremely hard to see; the sun is a minority of the baked light. | Drop the moving sun as a feature. Keep the sidecar (normals, ambient, sun share, visibility) as infrastructure for SSAO, PBR and shadows. |
| Moon arc | Ambient-only night is indistinguishable from the bake. | Decision 4 closed: no moon variants. |
| Vista fog | Single far level drawn at full detail while nearby neighbours stay as low-res stand-ins; black chasms where no level exists; distant geometry lit black by the viewer level's palette. | Single-far-level vista is not a feature. Reframe as neighbour fidelity: keep every adjacent level resident (more loader slots), draw each with its own mood weights and all draw types including water and transparents, delete the stand-ins for anything drawn for real, one fog model; horizon skirts stay. Sequenced after stereo. Unified fog code merged as an off-by-default mode. |
| SSAO rounds | Revision-1 pick B (heavy shading) with faceting artifacts; crease threshold only slightly better. | Revision 3 uses true per-vertex normals from the sidecar via a normal target. |

Proposal on the table (awaiting the maintainer): replace the sun controller with actor shadow maps from the fitted sun and the dominant nearby light, consistent with the bake through the sidecar's sun share and visibility; the world is a receiver only.

## Neighbour-load policy (2026-09-24, from the maintainer)

Load nearby neighbours only where a vista exists; indoor levels (Lost Precursor City, Spider
Cave, the citadel interior) load nothing beyond vanilla. Derive it from data, not a hand list:
a level is outdoors when its own bsp carries stand-ins for other levels, and a neighbour is a
candidate only when it is among the current level's stand-ins. Cap residents by memory, ordered
by stand-in screen coverage from the level's standard views. Tracked on #156.

## Emissive lighting (2026-09-25, from the maintainer)

Nearby surfaces should receive light from emissive particles and fixtures. Screen-space tile
lights first (automatic, from the emissive buffer). Then a light list: static emitters from the
sidecar (offline centroids of emitter-scored texels) and dynamic emitters published by GOAL
(eco vents, orbs, cells, spawners), so off-screen sources light the scene without being drawn.
Eco colour must survive brightness: hue-preserving clip (#190). Tracked on #191.

## Material contrast (2026-09-25, from the maintainer)

The "subtle, faithful" constraint on materials is lifted: the maintainer does not feel material
contrast in the class shading + maps result ("the PBR results are just not there"). Materials
need a real response model: metallic workflow, a per-mood environment to reflect, a
roughness-shaped lobe, and ambient response to normal maps, presented as faithful / bold /
cinematic looks. Stylized remains the target; subtle does not.

## Settings and HDR calibration (2026-09-25, from the maintainer)

All enhancement settings, including HDR calibration, must eventually be reachable from the
game's own settings menu. Until then, HDR calibration defaults come from the Windows display
profile (MHC2 tag from the Windows HDR Calibration app) and DXGI capabilities, with an in-engine
override. The tonemap must stay toony: identity below white, a short shoulder only for
overbright values, luminance-based with chroma preserved.

## Collectible light (2026-09-26, from the maintainer)

| Topic | Decision or input | Where it lands |
|---|---|---|
| Precursor orbs | Orbs emit no light into the world, neither as world lights nor as screen-space tile lights; their vanilla glow and bloom stay. Power cells keep emitting. Why: reviewers read collectibles as icons, not lamps, and orbs already glow in vanilla, so pools of light under every orb clutter the ground (yellow pools on sunlit sand at midday). Knobs: `emits` in `entity_types.json` (world lights) and `lights` per texture in `[actor_emissive.textures]` (tile lights). | Emissive lighting |

## Vista landmarks (2026-09-26)
Picked B on landmarks8: real neighbour levels with unified fog, haze and the Misty/basin fits are the look; vanilla stand-ins are out. The settings menu default (neighbour levels on) and the play forms already match. Open: the far training island reads crisper than Misty (its fit is weaker) and the horizon seam line (#249).

## Precursor tower highlight (2026-09-26)
Chosen: the original highlight (variant B), colour blended from the time-of-day slots. The maintainer liked the highlight itself; only its dusk orange was the flaw. No moon lobe, no sheen.

## Eco and emitters: vanilla look, HDR brightness (2026-09-26)
User: "what we want is the same look as vanilla but brightness gets interpolated to the HDR color space + our real time lighting effects (bloom + emissive)". So: the hue and shape of every eco effect, torch and lantern is vanilla's; our contribution is headroom (cores whiten like vanilla's per-channel clip, halos keep vanilla's hue: the blue eco halo is cyan-blue, not violet), plus bloom and the emissive lights they cast. Anyone touching eco compares hue against the vanilla frame (kb/facts/eco.md once it exists) before brightness. Lantern glass (village2) may not even be lit correctly; treat as its own bug.

## Parallax coverage (2026-09-26)
Parallax occlusion mapping stays on stone and masonry, cave rock included: "it looks great on rock, I explicitly said stone + masonry". Never narrow it to masonry. Open: seams along triangle edges (offset basis discontinuity), fix without reducing depth or coverage.

## HDR10 by default (2026-09-26)
User: "HDR10 should be the default color space if an HDR monitor is detected". HDR output defaults to PQ (HDR10, BT.2020, metadata from the display) everywhere: engine, settings menu, play script. scRGB stays a choice. Detection is unchanged: HDR is presented only when Windows HDR is on for the game's display, SDR otherwise. `sandover.gc` version 1 files that hold the old scRGB default read as PQ.

## Fog density: at least vanilla, everywhere (2026-09-27)

User, on the finalboss fog round: "fog still doesn't look as dense as C [vanilla]... it also shouldn't only apply to final boss fog -- all fog should be stronger to match the original game I keep saying this". The unified fog and haze must never read thinner than the original game's fog at the same level, hour and distance. Per-level tweaks are not the answer; the global fog model matches vanilla's density and colour curve, and neighbour levels follow the same rule.
