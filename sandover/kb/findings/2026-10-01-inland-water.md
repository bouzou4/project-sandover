# Inland water: draw path facts and the rule

The decision is to clear inland water. Code read at master `811f54d28`; V = verified in
code or data, I = inferred.

## Draw path of every `water-anim` surface (V)

- Which renderer: `bones.gc:1240` sends a ripple effect to mercneric (Generic2) only while the
  draw control has a `ripple query`; otherwise Merc2 with the game's vertex update
  (`pc-merc-vtx-update`). Only `dark-eco-pool` (and its subclass `helix-dark-eco`) and
  `sunken-water` allocate a query (`water-vol-method-22`). So every clear surface draws through
  Merc2 except the sunken pools (Generic2).
- Which bucket: the art's `texture-bucket` res picks the foreground sink
  (`process-drawable.gc:364-368`, 4 -> 2). Every clear water art has 4 (water sink:
  MERC_WATER_LEVEL0/1 58/61, GENERIC_WATER_LEVEL0/1 59/62); the mud art 0 (MERC_TFRAG_TEX);
  dark eco art has no res (pris). SSAO's depth copy is taken at the end of GENERIC_PRIS (56,
  `OpenGLRenderer.cpp:1666`), so it holds the bed under every clear surface and is readable
  while they draw.
- Water mode: a Merc2 model in the water sink is flagged by GOAL (`bones.gc:920-947`) and drawn
  alpha blended with no depth write, base and envmap draws alike (`Merc2.cpp` alloc_normal_draw,
  try_alloc_envmap_draw). The envmap layer is a second draw (`SRC_0_DST_DST`: added, weighted by
  the destination alpha the base draw wrote).
- Ripple normals reach Merc2: `ripple-apply-wave-table` writes the normal x and z bytes and the
  position y byte of each vertex in the merc fragments (`mips2c/jak1_functions/ripple.cpp:471-473`),
  which the Merc2 mod-vertex unpack reads (`Merc2.cpp` model_mod_draws). Generic2 has no normal
  attribute.
- The game's tint is `color-mult`, which multiplies every light colour of the draw
  (`drawable.gc:483`, `551-563`), so the merc vertex colour is already "mood light x tint": the
  vanilla colour and alpha are the deep-water limit with nothing else to pass in.

The generated list (`python -m sandover.inland_water --report`): 24 clear surfaces (jungle river,
two rolling lakes, training pond, village1 fountain and three paddy tiers, village2 bucket, both
purple cave pools, 13 sunken/sunkenb pools), 11 Misty mud pools, 10 dark eco pools (left to the dark eco pool change),
3 lava surfaces (left alone).

## The rule

See `InlandWater.h`. Clear: alpha = game alpha x (1 - exp(-ray / depth_m)), ray = the view ray's
length from the surface to the bed (SSAO depth copy, farthest MSAA sample); on Merc2 under a sky,
a Schlick fresnel reflection of the mood environment at the fog colour's brightness (the ocean's
`water_reflection` recipe), capped, plus a GGX glint on the ripple normal, in place of the game's
envmap layer. Without a sky (maincave, darkcave, sunken) the envmap layer stays and nothing is
reflected or glinted. Generic2 (sunken) gets the depth term only. Mud: game colour x 0.8, rough
reflection capped at 0.15, a broad sun lobe; opaque.

## The two purple cave pools

`cave-water-2` (maincave) and `cave-water-3` (darkcave) are `cave-water`, clear water with
`color-mult (0.2, 0.1, 0.3, 0.75)` (`maincave-obs.gc:77`). Both levels have no sky, so they get
only the depth term: the game's purple and envmap stay where the water is deep, the shallow rim
lets the bed through. No colour is added from the dark crystal below (nothing in the entity data
gives a light colour or strength for it beyond the crystal-light rows, which belong to the light rule).
The rule only lowers the pool's alpha: each pixel becomes a mix of the game's own water colour
and the rock behind it, and the envmap layer (weighted by that alpha) weakens with it. Nothing is
added, so the pool is never brighter than the brighter of those two; in the unlit Dark Cave both
are near black (bed under 22 m of water at the centre, unlit rock at the rim).
