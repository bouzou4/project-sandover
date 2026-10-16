# Frame profile how-to (Jak 1)

Where the frame time goes, per renderer group and per Sandover effect, at the play settings.
Engine side: `PerfTimers.h` (GL_TIMESTAMP scopes plus CPU and game-thread times). Tool side:
`sandover-perf` (`sandover/tools/sandover/rig/perf.py`) with the spots in
`sandover/shots/perf.yaml`.

## Engine timers

`(pc-set-perf-timing N)` logs one window every N rendered frames; `0` (the default) turns it off.
While off, every scope is one branch and no GL call. While on, each scope writes two timestamps;
results are read six frames later, so the CPU never waits on the GPU.

```
PERF-GPU w=12 name=ssao in=buckets avg_ms=1.2034 min_ms=1.1 max_ms=1.4 n=60/60
PERF-GPU w=12 name=vista:beach in=vista avg_ms=0.8120 min_ms=0.7 max_ms=1.0 n=60/60
PERF-CPU w=12 name=frame avg_ms=13.1 min_ms=12.2 max_ms=15.0 n=60
PERF-CNT w=12 name=shadow-maps avg=3.00 min=3 max=3 n=60
PERF-END w=12 frames=60 gpu_frames=60 fb=3840x2160 msaa=4 present=hdr-dxgi
```

GPU `avg_ms` is the per-frame sum of a group's scopes averaged over every collected frame
(`n` = frames in which the group ran). `in` names the enclosing group. Named scopes
(`<prefix>:<label>`) are created at run time, one per vista neighbour level. `PERF-CNT` lines
are per-frame counts averaged over the frames that set them.

| Group | What |
|---|---|
| `frame` | GPU span from the frame's first command to the end of present |
| `setup` | frame setup and clears |
| `buckets` | the whole bucket dispatch, Sandover passes included; everything below is inside it |
| `sky`, `sky-blend` | sky bucket; tfrag trans + sky blend buckets |
| `tex` | texture upload and animation buckets |
| `tfrag`, `tie`, `etie`, `shrub`, `generic`, `merc` | vanilla background and foreground renderers, both levels summed |
| `fur` | fur shell draws, inside `merc` |
| `eyes`, `shadow`, `depth-cue`, `sprite`, `direct` | eye, stencil shadow, depth cue, particles, debug/HUD text buckets |
| `ocean-texture`, `ocean-far`, `ocean-mid`, `ocean-near` | ocean texture, far band, mid (the deferred water-shaded mid included), near |
| `water-depth` | depth copy under the water surface |
| `actor-shadows` | shadow map prepass |
| `vista`, `vista-alpha` | neighbour levels |
| `ssao-targets`, `ssao-depth`, `ssao`, `ssao-view` | normal/emissive target clears, debug depth copy, AO, debug view |
| `emissive-lights`, `bloom` | tile lights (GPU part only; the readback sync shows in `render`), bloom |
| `smaa` | SMAA 1x before the HUD buckets (any mode with SMAA, the default 2x + SMAA included) |
| `upscale` | render scale below 100%: the upscale to the display size before the text buckets |
| `blit-display`, `pcrtc` | display copy; resolve, tonemap and post |
| `imgui`, `present` | debug UI; HDR DXGI present or GL swap |

Sub-scopes (need not add up to their parent: state changes and uncovered draws stay in the
parent only):

| Parent | Sub-scopes |
|---|---|
| `ssao` | `ssao-depth-lin` (depth copy, linear z, mips), `ssao-ao`, `ssao-blur`, `ssao-composite` |
| `emissive-lights` | `emlights-resolve` (colour resolve), `emlights-tiles` (tile + select), `emlights-track` (readback, tracking, uploads: the sync wait lands here), `emlights-light` (full-screen light pass) |
| `ocean-mid` | `ocean-mid-depth` (depth-only pass, perf-opt bit 26), `ocean-mid-tex`, `ocean-mid-envmap` (the tessellated draws) |
| `actor-shadows` | `shadow-clear`, `shadow-merc`, `shadow-generic` (every caster draws into every map, so maps are not split; see the `shadow-maps` counter), `world-shadows` (the world sun map's cascades) |
| `vista`, `vista-alpha` | `vista:<level>`, `vista-alpha:<level>` per neighbour |
| `buckets` | `swamp-mist` (after SSAO, its own group; not in `ssao`) |
| `pcrtc` | `pcrtc-resolve` (MSAA blit), `pcrtc-post` (tonemap, PQ, brightness) |
| `present` | `present-encode` (HDR: interop lock, encode, unlock), `present-flip` (HDR: D3D copy + Present), `present-swap` (GL swap) |
| `smaa` | `smaa-copy` (frame copy, or the 2-sample resolve), `smaa-edges`, `smaa-weights`, `smaa-blend` |
| `upscale` | `upscale-copy` (copy or resolve of the render-size scene), `fsr-easu`, `fsr-rcas`, `upscale-bilinear` |

`present-flip` is a GL timestamp span: it cannot see the work D3D queues for the copy and
Present, so read that cost from the CPU `present` time (and the frame interval), not from it.

Counters: `shadow-maps` (actor shadow maps rendered, sun + spots), `vista-levels` (neighbours
drawn), `ocean-mid-patches` (tessellated mid ocean source triangles), `ocean-near-cpu-ms` (render thread
in the near ocean's water setup), `ocean-rim-cpu-ms` (its rim pass), `ocean-rim-rebuilt` (1 when
the rim was rebuilt, 0 when the last one was reused), `world-cpu-ms` (render
thread in the world map prepass), `world-cascades` (world map cascades redrawn this frame; with
perf-opt bit 30 an unchanged cascade is kept, so a still camera with a frozen clock reads 0 and
the prepass costs nothing there; the harness keeps the bit off, see Optimizations). Per window,
`peak-<name>` counters (n=1) give the slowest CPU frame's split: its CPU times (`peak-frame`,
`peak-render`, `peak-wait-dma`, `peak-ee`, `peak-present`, ...) and counters (`peak-vista-cpu-ms`,
`peak-world-cpu-ms`, ...), so a single hitch can be told apart from the mean.
`ocean-mid-mfrags` and `ocean-near-mfrags` are the fragment shader runs of the mid and near
textured draws, in millions per frame (pipeline statistics query, read a few frames late). They
are set only with the SSAO GPU timing switch on (`(pc-set-ssao-option 2 1)` after the load), which
also turns on other GPU queries, so take them from a probe run, not a timing run. With bit 26 on,
the mid count is the pixels the mid ocean covers; off, it includes the overdraw.

CPU: `frame` (present to present), `render` (render thread submitting the frame), `wait-dma`
(render thread waiting for the game's chain), `ee` (game thread from its vsync wake to its
sync), `ee-wait` (game thread blocked in sync and vsync), `present` (the present call).

## Run

```sh
cd sandover/tools
uv run sandover-perf                      # every spot, every toggle
uv run sandover-perf --only jungle --toggles ssao bloom --frames 120
uv run sandover-perf --no-toggles         # base frames only
uv run sandover-perf --only swamp --toggles swamp   # opt-in: the swamp look
uv run sandover-perf --bin <staged copy> --proj <its project root>  # another build
uv run sandover-perf --toggles ssao+bloom  # both off in one measurement
uv run sandover-perf --set probes --only open-sea-low village1-hut-lamps
uv run sandover-perf --custom opt-x "(pc-set-x 0)" "(pc-set-x 1)"  # a branch-only knob
uv run sandover-perf compare <run A> <run B>   # deltas with noise bands
```

One instance: boots, then per spot shoots the rig frame (PNG kept next to the results), applies
the game's Sandover defaults after the load (`sandover-settings-reset!` +
`sandover-settings-reapply`), sets the window size and MSAA with vsync off and the limiter at
1000 fps, and measures A/B/A: base, effect 1 off, base, effect 2 off, base, ... Each measurement
discards one timer window and keeps the next (the first may hold frames from before the
switch). Toggles set the effect's `*pc-settings* sandover` field. An effect's marginal cost is
the mean of the base windows before and after it minus its off window; `vista` goes last and is
not switched back on, so it is compared with the base before it only. `ssao` includes the tile
lights, `hdr-target` includes the tonemap and HDR output. `hdr-output` compares the DXGI
flip-model present with the GL swap (the `present` group), not the PQ encode, which stays in
`pcrtc` either way. About 21-24 min for the 21 spots with every default toggle, plus boot.
`swamp` (the swamp look: water, wet and mist, engine default on) has no settings field; it sends
`pc-set-swamp` forms and runs only when named in `--toggles`, since it changes nothing outside
the swamp. `shore` (the wet shoreline, engine default on) is opt-in the same way
(`pc-set-swamp 16`); measure it at `beach-shore`: `--only beach-shore --toggles shore`.
`misty-mist` (the Misty Island sea mist; `pc-set-swamp 25`) is opt-in too, measured at a Misty
spot.

Water term costs (a probe, not a toggle): water option 101 switches terms of the mid ocean's
textured shading off past a distance, so `--custom` marginals split its cost. The value is
`mask | (metres << 12)` (times 1000000, as every `pc-set-water` value): 1 world sun map lookup,
2 analytic glint, 4 reflection sun glow, 8 reflection (fog colour instead), 16 foam, 32 texture
ripple normal, 64 crest sparkle (its texture taps), 128 Gerstner normal, 256 hex tiling, 1024 the
whole shading (vanilla mid); 2048 alone shows distance bands instead of the colour (140, 280,
500, 900, 2000 m; near ocean magenta). 1024 and 2048 ignore the distance (1024 skips the
water geometry too). Water options 99/100 with the start above the end clamp the fade to 1 m, a
hard cut. Push the bit 27 fade out of the way while probing
(`(pc-set-water 99 100000000000)`, `(pc-set-water 100 200000000000)`), e.g.
`--custom p-map "(pc-set-water 101 1000000)" "(pc-set-water 101 0)"`.

Antialiasing (opt-in, `--set aa`): the base is the shipped default, the run's `--msaa` (2 in
`perf.yaml`) with the SMAA row on, so 2x MSAA plus SMAA (`msaa2-smaa`). `aa-smaa` and `aa-off`
switch the `antialias` setting to that mode; `aa-msaa4` is 4x MSAA without SMAA (SMAA row off,
`gfx-msaa` 4) and restores the run's MSAA. Each marginal is what the base costs
over the mode (negative when the mode is dearer). The SMAA pass itself is the `smaa` group.
`aa-off` is the 1-sample frame with no antialiasing, so `aa-off` minus `aa-smaa` is the SMAA
pass plus nothing else.

Render scale (opt-in, `--set scale`): `rs-77-fsr`, `rs-67-fsr`, `rs-50-fsr` and the `-bilinear`
variants set `render-scale` and `upscaler` in place of native; the window and `--msaa` stay the
run's, so a 4K run measures 4K output. `fb` in the tables is then the render size. A marginal is
what native costs over the scaled frame (positive: the scaled frame is cheaper).

Opt-in probes (not looks; they exist to split a cost): `water-tess` turns the water
tessellation off (one segment per edge, near and far: `pc-set-water` 93 `TESS_SEG_M` and 8
`SUBDIV`), so its marginal is what the tessellation costs; `local-lights` sets shadow option 7
to 0 (local lights on the characters and their spot maps; needs #297, a no-op before it).
`--set` picks a list from `TOGGLE_SETS` (`default`, `probes`, `new-passes`, `all`); `a+b`
switches several toggles off together as one measurement; `--custom NAME OFF ON` measures any
pair of GOAL forms the same way. A toggle that is not switched back on (`vista`, or a combination
containing it) runs last, and only one
such toggle is allowed per run (a second would be measured against a base with the first off).

Optimizations: `(pc-set-perf-opt MASK)` sets a 32-bit mask, `PERF_OPT_DEFAULT` at boot (every
bit on except those in `PERF_OPT_FAILED`, which failed their gate; `OPT_FAILED` in the tool
matches it). The harness measures every window with the run mask, `OPT_RUN`: the default minus
`OPT_STATIC_ONLY` (bit 30, the world map cascade cache), because a spot freezes the camera and
the game clock, where that cache skips the whole world map prepass that play pays every frame
(2-4 ms at 4K); toggles restore the run mask, and `static-cache` (opt-in) sets the engine
default so its marginal is minus the static saving. C++ reads a
bit with `Gfx::perf_opt(bit)`, a shader through the `perf_opt` uint uniform
(`perf::set_opt_uniform(program)`). A bit no optimization uses does nothing. Four bits per area
(`PerfOptArea` in gfx.h): 0-3 tile lights, 4-7 SSAO, 8-11 vista, 12-15 water, 16-19
present/post/bloom, 20-23 geometry/CPU, 24-27 present extras and the mid ocean (26 depth pass,
27 distance fade), 28-31 renderer core. Toggles (opt-in, `--set opt`): `opt-<area>` switches
the area's bits off, `opt-all` every bit, `opt-bit5` / `opt-bits4-6` any bits; the marginal is
minus what those optimizations save. `--custom` also takes `mask:<n>` (that mask) and
`clear:<bits>` (the run mask minus those) in place of forms. Each spot's windows start with the
run mask. The "no visible change" check is `python -m
sandover.rig.pixdiff A B` on two capture runs: per shot the largest channel difference, the
pixels over the threshold (count and share; default 2, exit 1 above it), EXR differences, and
`--json`.

Another build: `--bin` names a staged copy under `$SANDOVER_DATA/bin`; `--proj` (or
`SANDOVER_GK_PROJ_PATH`) is the project root gk runs from, for its shaders and data. Both are
recorded in the run's meta (`bin`, `bin_build` from the copy's `build.json`, `gk_proj`,
`gk_proj_sha`).

Noise: the spread (max - min) of all base windows at a spot is its noise band, printed per spot
and in `perf.md`; a marginal inside it is marked `~`.

Group ms are GPU-or-submit time: a timestamp span also covers the GPU waiting for the CPU to issue
the group's commands. The tile lights read a candidate row back every frame (a full CPU-GPU
sync), so while they are on the render thread's time is close to the frame interval and a
CPU-bound versus GPU-bound call cannot be read off these numbers; compare the GPU span with the
frame interval (`GPU/frame` column) and the `tile-lights` marginal instead.

Output in `$SANDOVER_DATA/perf/<YYYYMMDD-HHMM>/`, rewritten after every spot so a crash keeps what
was measured: `perf.json` (every window), `summary.json` (one flat record per spot: frame, GPU
groups with parent and noise, counters, CPU, marginals; schema `sandover-perf-summary/1`),
`perf.md` (frame, GPU per group, sub-scopes, counters, marginal cost tables), `<spot>.png`. Console: `PERF <spot> frame ...` at the first base and again with the noise band
at the end of the spot, and `PERF <spot> -<effect> saves ...` per toggle.

## Compare runs

`sandover-perf compare A B` takes run directories, `perf.json`/`summary.json` files, or run names
under `$SANDOVER_DATA/perf`, and prints per spot the frame and GPU span of both, the delta
(B - A, positive = B slower), the band (the sum of both runs' base-window spreads) and a verdict:
`faster`/`slower` outside the band, `~` inside it or under `--floor` (0.05 ms), `?` when a run
has one base window per spot (runs before 2026-09-28). Those runs also have no per-group
noise, so every group change at or above the floor against them reads `?`. Then, per spot, every group outside its
band (`--all`: every group) and the marginals both runs measured. `--json FILE` writes the same
as JSON. Spots only one run has are listed at the end.

Builds: the tool reserves a game slot first (lanes free while it waits), then takes both build
lanes (`build`, `build-b`) before it boots gk and holds them for the whole run, so no game sits
idle while another worktree builds. It logs `PERF: holding
build lane` before the boot line (the wait is `build_wait_s` in the run meta), repeats the
`waiting for ... lock` line once a minute with the elapsed time and holder, and exits with code 3,
no gk booted, when the lanes stay busy past `--lane-timeout` minutes (default 20). A compile on the
same CPU reads as GPU time in submit-bound groups: `merc` at jungle was 0.46-0.50 ms quiet and
0.59-0.90 ms in runs taken while other area builds were running, with `tfrag` and `fur` unchanged.
`--allow-builds` measures anyway (the run meta records `quiet_builds`).

Measure solo (the tool refuses to start while another gk runs); GPU power state moves numbers,
so compare runs made back to back. `fb` in the tables is the size actually rendered: the window
cannot grow past the desktop, so a 3840x2160 request on a 4K desktop may render smaller.
