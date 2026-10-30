---
title: What can the swamp's own data carry for a remaster look (water, wet, mist)?
date: 2026-09-27
status: inferred
sha: 7917433c4 (base; branch swamp-directions)
---
## Answer (≤5 lines)
The swamp water is not the ocean: it is one tfrag texture, `swp-water`, flat at y = 0 and opaque,
so the ocean shading never touched it (0017 had `pc-set-water` on and still matched vanilla). It
always rains in the swamp. Three directions behind `pc-set-swamp`, each swamp-only and off by
default: B water (murk, reflection, rain rings, glint), C wet surfaces and puddles, D ground mist.
No depth absorption (nothing is seen under the opaque water) and no god rays (the world casts
into no shadow map, and the sun is near the zenith at 14.8 h).

## Evidence
- Water: `swamp-background.glb` (decompiler export): `swp-water` sits in the tfrag `normal` tree,
  2,832 triangles, every vertex at y = 0.00 m, xz 253..696 x -2176..-1811 m. Material alphaMode
  MASK, which `fr3_to_gltf.cpp` writes when the draw mode has no alpha blend: alpha-tested,
  opaque. Texture colour olive (mean 95, 85, 17). Under the 0017 camera the only geometry below
  y = -0.2 m is `swp-beachrock` tie, so no visible bed to absorb toward. `water-vol` actors carry
  water-height 0.1 m (`entities/swamp-actors.json`).
- Ocean: swamp's `:ocean` is `*ocean-map-village2*` (`level-info.gc`), and `draw-ocean` lowers it
  to -1 m within 200 m of `*swamp-low-ocean-marker*` (`ocean.gc`); the 0017 camera is 262 m from
  the marker. The frame shows no waves with water shading on, so the ocean does not cover it.
- Rain: `target-powerup-process` calls `update-rain` while the current level is `swamp` and
  `*weather-off*` is false (`powerups.gc`), so "raining" is "in the swamp".
- Classes: `swp-water` is `special` in materials.csv; porous classes cover the bark, roots, planks
  and mud (wood, stone), so C darkens exactly the surfaces the triage called evenly lit.

## Implications for the design
- B and C ride on the class shading (PBR on); D reads SSAO's depth copy (SSAO on). The play look
  has both.
- The water bit is set by texture name in the swamp level only; the vista also draws swamp as a
  neighbour with it, which is harmless (the same surface).

## Follow-ups
- Decided: B, C and D are kept together and become the one look; the other directions are removed.
- If B wins: canopy silhouettes in the reflection would need a screen-space or planar pass.

## Decision (2026-09-27)
B, C and D were kept together. The three are the swamp
look: engine defaults on (gfx.h), riding on Materials (water, wet) and ambient occlusion (mist),
values unchanged from the comparison. `pc-set-swamp` stays for tuning; the one-direction presets are
gone. Code read of the three together: the water branch returns before the wet code (the water
never gets the film), rings are per surface (water or puddle, never both on one pixel), and the
mist pass runs once per frame after SSAO over the already-shaded surfaces, so nothing applies
twice. Confirmation round: `shots/swamp-directions.yaml` (vanilla, A off, B swamp).
