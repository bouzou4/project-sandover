---
title: The ledge eco "lights under the ledge" because its screen tile sat on the lip; launch lights fix it
date: 2026-10-15
issue: play capture runs/20261015-1220-play/0001 (beach, 16.2 h)
status: verified
sha: b59bd4c22 (base), fix/light-leak
---
## Answer (≤5 lines)
This is not a missing occlusion term: the light was in the wrong place. A tile light takes the
depth of the nearest surface in its 60 px tile, plus 1 m. For an eco 77 m away and 5.75 m behind its
ledge lip, that put the light on the lip, and it lit the cliff face below. The eco's census row never
took over. The fix publishes every particle launch from GOAL and lights entity emitters at their
launch origin, using only their glow groups. The slotted emitter's own tiles give no light. The
under-ledge delta drops to the no-light floor.

## Evidence
- Identification: the ray through the blob pixel (1581, 428) passes 0.5 m from census
  `beach:eco-6` (aid 9985), at 77.6 m. In the level mesh the eco floats 1.8 m over a flat ledge top
  and the lip is 5.75 m in front of it.
- Master burst (`pc-set-emissive-lights 17`): two blue tile lights at 5.9 and 6.3 m from the eco,
  on the lip, at 0.37 and 0.29 luma. The eco gets no world light: it is not among the 16 nearest
  rows, and no tile is within its 5 m radius to claim it.
- Mechanism line: `float z = min(a.zw > 0.0 ? a.zsum / a.zw : a.zmin, a.zmin + near_slack);` in
  emissive_tiles.frag. Sprite depth does not help, because the clamp to the tile's nearest
  surface wins.
- Same-build A/B (`pc-set-emissive-lights 38 0` is the master path). Measure: cliff under the blob
  minus the cliff 400 px left and right, 8-bit RGB.
  - Beach at 100 m (same ray): A +20.2/+23.1/+31.4, B +2.7/+1.9/+1.8. A frame with no emissive light
    there measures the same +2.7/+1.9/+1.8. A draws the lip tiles (5.8 and 7.1 m from the eco, 0.32
    and 0.51 luma); B lists them as candidates, draws none, and lights the eco from its launch.
  - The snapshot: A +3.7/+4.3/+7.6, B -0.8/-1.8/+0.8. The lip tile is intermittent at 77 m:
    a second A run (A') drew none and measures -0.7/-1.5/+1.3.
- Far eco halo (box around the blob at 77 m): A 83.8/107.3/151.0, A' 73.2/102.0/144.9;
  B 65.6/69.9/130.6, B' 69.2/77.7/136.3. At 100 m: A 70.5/83.8/125.8, B 57.8/70.8/102.6.
  - This is real light, not particle noise: A and A' agree, and B and B' agree.
  - In A, the eco's sprite tiles (at the eco, from sprite depth) light the cliff behind it. In B
    those tiles stand for the slotted launch light and give none. The census row (weak x 40) is
    dimmer than those tiles. The row is the light master already uses for an eco within reach.
  - Reported, not tuned.
- Particles off (rig `particles: off`, 100 m view): spawn still runs and the hook reports 58 calls.
  B lights the eco from its launch with no particles drawn (halo 42.0/32.0/29.9 -> 48.4/38.6/44.1,
  under-ledge at the floor); A leaves it unlit.
- Lip strip and grass beside the blob lose 6-9 levels (ledge top 38.5/45.1/23.4 -> 32.5/36.4/16.0).
  They are 5.5-6 m from the eco, beyond its 5 m reach, so only the misplaced tile lit them. The
  eco's own pool, seen 4.5 m away, is unchanged: 62.5/78.3/77.6 in both arms.
- Snow switch (runs/20261008-1728-play/0018): both arms draw one light at the pillar, 1.5 m up. The
  ring tiles are candidates and none is drawn.
- Vents: the open jungle blue vent's light moves 1 m up its column at the same power (ring
  36.3/35.6/46.0 -> 40.5/40.3/56.6; ground ±0.5). The blocked beach green vent never launches and
  its frame is identical within 0.2 levels.
- Broken citadel coils (`trigger` 60 frames after load): `citb-coil-broken` spawns `part-off` every
  frame. No coil row is lit in either arm: GOAL answers 0 for a complete entity, and `part-off` is
  not a glow group.
- Eco pickup (village1 eco-10, Jak moved onto it). Over three frames, 8 and 10 frames apart, the
  eco's light moves 0.09 -> 0.89 -> 1.78 m along the pickup stream at its own power. 70 frames
  later it is gone. Frames: shots/fix-light-leak/crops/eco-pickup-B-p1-p4-lifted.png.
- Cost (PERF-GPU, 4K MSAA 2 + SMAA, median of clean windows, A -> B):
  - emissive-lights 1.27 -> 1.08 ms at the blue vent, 1.26 -> 1.01 ms at the eco close-up,
    0.84 -> 0.81 ms at the blocked vent.
  - The B arm is never slower. The absolute numbers moved with the machine's load between rounds.
  - 15-62 spawn calls per frame; 2-5 of them are entities with a listed glow group.
- Framing: the from-snapshot shots frame differently by slot, not by build or root. Slots 0 and 3
  boot in fullscreen (`display_mode` 2) and slot 1 windowed (0), so the 3840x2054 window gets a
  different projection. Every A/B pair above was shot on one slot.

## Implications for the design
- Emitter state comes from the game's own mechanism: an entity's light follows its glow-group spawn
  calls. The hook sits before the particles-off return and the launcher's visibility test.
- For 30 launch frames after its last launch a row is dark; then the GOAL query answers again. That
  covers a process that stopped running: actor-pause beyond its 80-220 m pause distance, the snow
  bumper beyond 90 m from Jak, money orbs that pause when not drawn. GOAL is asked about
  launch-driven rows too, so the hand-back is current.
- Launches without an entity (Jak and enemy effects) are not lit by this path; their tiles stay.

## Follow-ups
- The launch offset is rotated as `vector-orient-by-quat!` does (checked numerically against its VU
  code); the proof vents are upright, so a tilted vent has not been seen in a frame.
- Group offsets beyond the proof emitters are 0 (`launch` entries in entity_types.json); tune them per
  group.
- Far ecos read dimmer than master's tile light: decide whether the eco row's power
  should match its sprite tiles.
