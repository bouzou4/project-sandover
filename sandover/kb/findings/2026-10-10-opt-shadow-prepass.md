# World map prepass: unchanged cascades kept, per-frame CPU, load hitch (2026-10-10)

Items (a)4, 5, 6, 9 and 10 of `2026-10-10-perf-profile.md`. Everything is behind perf-opt bit 30
(`PERF_OPT_WORLD_PREPASS = PERF_OPT_CORE + 2`, default on); with the bit off the prepass runs the
master path. No setting, tier or look moves. Measured 2026-10-15.

Summary: on a still camera with a frozen clock (every rig and perf spot) the whole world map
prepass is skipped, 2.1-3.7 ms of frame at the seven spots. In motion (camera or clock moving,
i.e. gameplay) the saving is nil: every cascade still redraws. The load hitch was not the world
map. The 50-66 ms pan frames are vista neighbour draws and first-use costs, not the prepass and
not the tile lights.

## Setup

Base: the staged master build at #392 (c33058c53), project root the main checkout. Branch: the
same commit plus this change, project root a staged copy with the branch's `game/` and the main
checkout's `out/jak1` (same GOAL). 3840x2160, 2x MSAA plus SMAA (the shipped default since #390,
both arms), every row at its top tier, `perf-max.yaml`, 45-frame windows, solo GPU, base and
branch back to back in one build-lane hold, no other gk during either run. `wait.ok` matches per
spot in both arms; at `snow` and `snow-switch-field` the vista wait timed out in both arms.

Windows per spot: `static` (the rig camera, game clock frozen), `pan-2deg` (the camera yaws 2
degrees per frame), `pan-notile` (the same pan with the tile lights off) and `opt30` (bit 30 off,
static). The pan restarts from the rig camera each time, but the kept window starts after a
discarded one whose length depends on the frame rate, so pan views are not matched across runs:
compare pans inside one run (see "Motion").

## Look

Paused-frame A/B, bit 30 on (A), off (B), on again (A2), at the seven profile spots: six spots
are bit-exact in the PNG (max 0, no pixel differs); their EXR is at the A/A2 floor (village2
max_abs 6.1e-5 in both A/B and A/A2, the rest 0). `snow` differs inside one 130x250 px box at the
top right, a patch of rock lit by a drifting glowing particle: A/B max 12 on 433 px, A/A2 max 12
on 42 px, B/A2 max 11 on 45 px.

The rig's own verdict (`ab-verdicts.json`) is **fail** for `snow` (the A/B share is 10x the
floor's) and, on the sun-step run below, for `snow-1206`. I override both: the amplified A-B,
A-A2 and B-A2 maps show the same glow-lit rock patch and no shadow edge, and B-A2 (bit off
against the later bit on) is the closest pair of the three (snow 45 px, snow-1206 23 px against
A-A2 42 and 222 px), so the first A is the outlier and the difference is paused-state motion,
not the bit.

| spot | A/B max (8-bit) | px over 2 | A/A2 floor max |
|---|---|---|---|
| sunken, beach-shore, village1-hut-lamps, village2-night-lamps, jungle-river-0800, snow-switch-field | 0 | 0 | 0 |
| snow | 12 | 433 (0.005 %) | 12 (42 px) |

Sun steps: `sun-step.yaml` shoots snow 12.0 -> 12.1 h, village2 03:09 -> 03:15 and jungle 08:00
-> 08:06 at fixed cameras in one instance, so a cascade kept across a sun move would show. The
same paused A/B on it: village2 and jungle bit-exact; snow max 10-16 in the same particle box
(rig verdict `floor` for snow-1200 and `fail` for snow-1206, overridden as above: B vs A2 23 px,
A vs A2 222 px). The two bins shot separately differ only on
water, Jak, the fish and moving props (live clocks between runs), not on any static shadow edge.

## Item (a)4: redraw only cascades that moved

Each cascade remembers the clip matrix it was drawn with, the map texture and size it was drawn
into, and a caster generation (bumped when a level's caster cache is built or released, or the map
is reallocated). A cascade whose clip is
bitwise equal and whose generation matches already holds this frame's map, so it is not cleared
or drawn. This is exact: receivers read the same clip the cascade was drawn with, so a skipped
cascade is the same depth image the redraw would produce.

It only hits when the camera and the sun are both still. The box depth (`cz = f.dot(centre)`)
is not texel-snapped and the sun direction follows the live game hour, so in play (camera
moving or clock running) every cascade still redraws every frame. The rig freezes the clock and
holds the camera, so static spots show the whole prepass as saved; that is an upper bound, not
the gameplay gain. Snapping `cz` or stepping the sun would give hits in motion but moves the
shadows; that is a second-wave item, not this one.

Cache hits (`world-cascades`, cascades redrawn per frame, 3 = none kept):

| window | redrawn per frame | hit rate |
|---|---|---|
| static, 27 of 28 windows | 0.00 | 100 % |
| static, snow last window | 1.93 | 36 % (the vista loader was still streaming there in both arms; level caches coming and going bump the generation; not confirmed) |
| every pan window, all spots | 3.00 | 0 % |

Static (game clock frozen, camera still), ms:

| spot | base frame | branch frame | delta | bit 30 off-on, same session (frame / GPU) | actor-shadows GPU base -> branch |
|---|---|---|---|---|---|
| beach-shore | 30.24 | 27.79 | 2.45 | 2.36 / 2.41 | 2.82 -> 0.32 |
| sunken | 18.35 | 15.38 | 2.97 | 2.95 / 2.99 | 3.40 -> 0.22 |
| snow | 19.88 (noise 1.39) | 16.83 (noise 2.16) | 3.05 | 2.07 / 1.83 | 3.17 -> 0.87 |
| village1-hut-lamps | 27.20 | 24.55 | 2.65 | 2.52 / 2.48 | 2.68 -> 0.26 |
| village2-night-lamps | 30.46 (noise 1.43) | 28.68 | 1.78 | 2.22 / 2.23 | 2.51 -> 0.32 |
| jungle-river-0800 | 23.28 | 20.59 | 2.69 | 2.41 / 2.45 | 2.67 -> 0.21 |
| snow-switch-field | 20.68 (noise 7.59) | 15.97 | 4.71 | 3.71 / 3.77 | 4.27 -> 0.17 |

The off-on column (bit 30 cleared and set again inside one session, same view) is the clean
number; the base-to-branch delta agrees within the noise bands. What is left in `actor-shadows`
on the branch (0.17-0.32 ms) is the actor sun and spot maps. The new `world-shadows` GPU group
reads 0.00 on static frames.

## Item (a)5: per-frame CPU of the prepass

- Tree bounding sphere: one padded sphere around all of a tree's vis nodes, tested before
  `cull_check_all_slow`; a tree is skipped only when every node would be culled (exact; trees
  with a group drawn without a node are never skipped).
- Wind TIE: instance matrices as a per-instance `mat4` attribute and one
  `glMultiDrawElementsIndirect` per draw and cascade (base instance indexes the matrix), in place
  of one `glUniformMatrix4fv` + `glDrawElements` per instance; same groups, offsets and 20 m cull.
- Depth func and clear depth stay two live `glGet` calls: reading them from `SavedState`, which
  re-queries the driver only every 256 captures, was unverified and saved under 0.1 ms (review).
- The pass's own timer query is sampled every 16 frames.

Motion, like for like: one session per spot, a warm-up pan, then the same pan with bit 30 on,
off and on again (same views, warm caches, three cascades redrawn every frame in all three):

| spot | frame ms on / off / on | `world-shadows` GPU on / off / on | `world-cpu-ms` on / off / on |
|---|---|---|---|
| sunken | 14.46 / 14.83 / 14.56 | 1.85 / 1.89 / 1.92 | 1.48 / 1.56 / 0.55 |
| village1-hut-lamps | 27.02 / 26.71 / 27.25 | 1.88 / 1.83 / 1.93 | 0.88 / 0.99 / 0.91 |
| jungle-river-0800 | 31.73 / 31.65 / 31.55 | 3.03 / 2.85 / 2.84 | 0.82 / 0.96 / 0.95 |
| snow-switch-field | 25.67 / 25.94 / 25.69 | 3.55 / 3.68 / 4.18 | 0.52 / 0.51 / 0.54 |

In motion the bit saves nothing measurable: frame -0.4 to +0.3 ms, GPU within ±0.2 ms, render
thread under 0.1 ms. The prepass CPU was already under 1.6 ms at these spots; the per-instance
wind draws and the two `glGet` calls were a small part of it. The prepass GPU cost in motion
(1.8-4.2 ms in `world-shadows`) is the real gameplay cost of the world map's casters, and the
cascade cache does not touch it while the camera or the clock moves.

## Item (a)6: load hitch

With the bit on at load, the loader uploads geo-0 tfrag tree indices into
`LevelData::tfrag_index_data` (only while a world map consumer is on: background, actors or
water; one tree's indices in one upload with its last vertex chunk, so a large tree's indices
land on that step) and the caster cache binds them (it falls back to its own copy when they are
missing) instead of uploading every tree's indices again. The branch logs each build:
`world-shadows: <level> caster cache built in X ms`. Over 20 level and neighbour builds in the
sun-step run: 0.3-2.8 ms each, 0.0 MB of indices re-uploaded (shrub index chunks are still
built there; they dominate the remaining time).

What the profile's "about 1 s" is: the first world-map frame of an instance is about 1 s of
render thread in both builds (base 1018.6 ms, branch 963.4 ms), but the world prepass is only
part of it. On the base its first `pass cpu` line, which covers both level builds of that
frame, is 5.2 ms; on the branch `world-cpu-ms` is 18.3 ms because the branch timer starts before
`ensure_target` and so covers the 192 MB map allocation (with the bit on or off; rendering is
unchanged). The rest of the frame is the PBR map uploads for three levels (about
375 ms logged), the 4K SSAO, emissive, bloom and SMAA target allocations and driver shader
recompiles, all the same in both builds. So the world map was not the cause of the 1 s, and
the gain from item 6 is a few ms per level load, not a visible hitch.

## Items (a)9 and (a)10: the 50-66 ms pan frames

The `PERF-CNT peak-*` lines give, per window, the slowest CPU frame's split (render, wait-dma,
ee, present, world-cpu, vista-cpu). In the timing runs the pan peaks were village2 64.2 ms
(render 63.9, vista-cpu 58.0, world-cpu 0.7) and jungle 58.2 ms (render 57.7, vista-cpu 2.1,
world-cpu 0.8). `pan-notile` ran second and did not hitch, which does not separate the tile
lights from first use, so a probe ran the pans in the other order (tile lights off on the first,
cold pan, then two warm pans with them on):

| spot | cold pan, tile lights off: peak (render / vista-cpu / world-cpu) | warm pans, tile lights on: peak |
|---|---|---|
| village2-night-lamps | 57.7 (57.1 / 52.4 / 0.9); the discarded window before it 63.8 (62.8 / 1.0 / 0.8) | 35.3, 34.4 |
| jungle-river-0800 | 58.2 (57.4 / 2.1 / 0.9) | 35.7, 35.4 |

- Not the tile-light readback (item 10): the hitch is there with the tile lights off and gone
  on warm pans with them on.
- Not the world prepass: `world-cpu` is 0.7-1.4 ms in every peak frame.
- Vista neighbour draws: single render frames of 49-58 ms inside `vista-cpu` when a neighbour
  comes (back) into view. At jungle it recurs on every return of the camera to the rig view after
  a pan (two base windows right after warm pans peaked at 59.0 and 57.4, vista-cpu 52.7 and 49.2),
  so it is not a one-off. Belongs to the `opt/vista` work.
- First use: one-off 58-64 ms render frames with `vista-cpu` and `world-cpu` both small (village2
  63.8, jungle 58.2) on the first pan only. Not attributed by today's counters; the log shows
  driver "vertex shader recompiled based on GL state" warnings on first draws. Second wave.

## Deferred

- Cache hits in motion (texel-snapped box depth, stepped sun): moves shadows, so it is a look
  item for the maintainer. Until then the motion gain of item 4 is nil.
- Shrub index chunks are still uploaded per caster cache build (the remaining build time).
- Profiles: `sandover-perf` now measures every window with bit 30 off (`OPT_RUN`), so static
  spots read what play costs; the `static-cache` toggle shows the static-only saving.
- A still camera with the clock running (idle play) was not measured; from the code the sun
  direction follows the hour every frame, so the clip moves and the cascades redraw.
