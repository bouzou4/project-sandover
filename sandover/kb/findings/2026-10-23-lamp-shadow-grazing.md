# Lamp shadows from a light at floor height (beach eco vent, 2026-10-23)

Report: play run 20261023-1042, entry 0002, beach at hour 12, Jak on the grass beside the open green
eco vent above the beach: "Jak shadowing here looks crazy". Next to his short noon sun shadow, a
broad dark wedge ran from his body across the grass, away from the vent.

## What draws the wedge

Toggles at the bundle camera (Jak's position and heading from the bundle, vent opened by marking
`ecoventrock-5` complete, the player's settings), luma in a box on the wedge
(x 1720-1840, y 1480-1620 at 3840x2160) against the frame with background lamp shadows probed off
(shadow option 44 bit 8):

| Arm | Wedge box luma ratio |
|---|---|
| play look | 0.798 |
| local lights off (option 7 = 0) | 0.983 |
| world sun map off (options 32, 33 = 0) | 0.984 |
| landing disc off (option 5 = 0) | 0.981 |

(The last two arms happened to capture with the local light faded out, so only the first and the
probe arm say which term it is; the bit 8 probe removes exactly the background lamp term.)

The world map, the disc and the actor sun map are not it. The wedge is the background receivers'
lamp shadow (`shadow_lamps_removed`, `actor_shadow_frag.glsl`).

## Mechanism

The light with the lamp map is world-light row 71: the vent pad's static emissive glow
(class strong, gate SLOT, param 8), placed at the pad's centre, 0.2 m above the grass and 4.7 m
from Jak. Being a light the bake is assumed to hold, it gets a 512² spot map aimed at Jak, and every
background fragment the map sees as blocked loses

`share = clamp(2 * L(p) / lum(baked), 0, 0.5)`

of its baked light, with `L(p) = lum(power) * window / (1 + d² / r0²)` (power 0.71, r0 5 m). That
estimate is the light arriving at a surface *facing* the lamp. The grass does not face it: a light
0.2 m above the ground reaches grass 5-15 m away at 1-2 degrees, so N.L is 0.01-0.04 and the light
it brings is almost nothing. The formula ignored the angle, so it removed the full 50% cap. And
because the light sits below Jak's knees, his shadow from it is a grazing projection that runs
away from the pad to the end of the map's reach: a long, sharp, half-dark wedge in full noon sun.

## Fix

The lamp share is multiplied by the receiver's N.L toward the light, `max(dot(g, l), 0) / (|g| |l|)`,
with `g` the geometric normal from the position's screen derivatives (`ws_dpdx`, `ws_dpdy`, taken
before any discard), turned toward the viewer (`pbr_view_v`): the visible face, two-sided shrubs
included, so a lamp behind a wall or floor removes nothing on the side the camera sees. A lamp
overhead keeps its shadow on the floor (N.L near 1); a floor lamp no longer darkens the floor it
only grazes. Applies to tfrag, shrub and envmapped tie receivers; merc actors already light and
shadow by N.L.

A cap from the bake's residual (light neither median ambient nor fitted direct) was tried first and
did nothing here: the beach bake does hold extra light around the vent (residual about 0.5 within
a few metres), so the problem is the projection, not the light's existence.

A smoothstep on N.L (full shadow above about 17 degrees) was tried to keep more of the stylized
lamp shadow; on the undulating beach grass it left facet-shaped pieces of the wedge wherever a
triangle tilts toward the pad (luma ratio 0.88 on the wedge), so plain Lambert stays.

## Proof

Wedge mask: the pixels the old frame shows more than 15% darker than the same frame with
background lamp shadows probed off (option 44 bit 8), inside x 1580-1880, y 1400-1640 at 3840x2160
(20.4k pixels). Mean luma there, frame over its own probe frame, same camera, same staged master
binary, shaders swapped:

| Frame | Ratio |
|---|---|
| before (master shaders) | 0.725 |
| after, capture 1 | 0.976 |
| after, capture 2 | 1.008 |

With the one-sided N.L (viewer-facing normal) the re-shoot gives 0.960 and 1.004.

Test: ratio >= 0.95 fails on the old frame and passes on both new ones. The lamp map was on and at
full fade in every shot (`local-lights:` line: row 71, fade 1.00, map).

Other shadow spots, one probe each, master shaders against the fix:

- village1 hut at night (`local-light-highlights.yaml` hut-lamps-night camera, two lamp maps on
  wall and hanging lamps 3.5-4 m above the floor): the crate's and Jak's lamp shadows on the floor
  stay; lamp-shadow pixels 229k / 857k before (two repeats; the lamps' state still ramps, so the
  count moves between repeats), 213k / 48k after, mean depth 0.66-0.69 before, 0.70-0.73 after.
  Those floors see the lamps at N.L 0.4-0.6, and the share is still at its 0.5 cap there.
  One-sided re-shoot: 465k / 235k pixels, depth 0.71-0.72; the crate shadow stays.
- village1-edge-1500 and beach-arch-1315 (`world-sun-shadows.yaml`, sun shadows): ground luma
  ratio 1.000 and 0.999 against master.

Cost: one normalize-and-dot per lamp per background fragment with a lamp map in reach; not timed
separately (no measurable change expected).

## Rig note

Rig instances on slots whose `display-settings.json` has `display_mode` 0 (slot3, slot4 on this
run) intermittently rendered no background (tfrag, tie and sky missing) in this play look, with
master shaders as well; slot2 (`display_mode` 2) never did. The proofs above are from good frames
only; the cause is not established.
