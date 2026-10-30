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

Stereo and XR: every group and counter is split by eye. The plain names are eye 0's (the left
eye, or mono); eye 1's scopes log as `e1:<name>` and its counters as `e1-<name>`. `eye` /
`e1:eye` is one eye's whole bucket dispatch and `eye-cpu-ms` its render-thread time. Buckets
that belong to no group are timed as `bucket:<id>` (`bucket:9` is tie level 0). XR adds CPU
lines `xr-wait` (xrWaitFrame + xrBeginFrame), `xr-acquire` (swapchain acquire and wait),
`xr-end` (release + xrEndFrame; on SteamVR it waits for the GPU) and `xr-copy-dma` (taking a
new game chain).

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
| `ssao-marks` | the late stencil marks: depth copies after the sky and around the deferred mid ocean, and the marks pass after bucket 30 (`StencilMarks.h`); the merc prop buckets' copies stay in `merc` |
| `bg-prepass` | background depth prepass: the depth-only replay of each level's tfrag and tie run and of each shrub bucket before their shaded draws (`BgPrepass` in gfx.h); the shaded draws stay in `tfrag`, `tie`, `shrub` |
| `emissive-lights`, `bloom` | tile lights (GPU part only; the readback sync shows in `render`), bloom |
| `fsr3`, `fsr3-reactive` | FSR 3.1 temporal AA (`pc-set-aa 0 1`, the default): the temporal upscale before the text buckets; the scene copy before the sprite bucket and the reactive mask after it |
| `dlss` | DLSS (`pc-set-aa 0 2`): input copy, the D3D11 evaluate with the interop unlock and lock, output blit |
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
| `fsr3` | `fsr3-prepare` (clears, prepare inputs), `fsr3-luma` (luma pyramid, shading change pyramid and pass), `fsr3-reactivity` (prepare reactivity, luma instability), `fsr3-accumulate`, `fsr3-rcas` (and the debug view) |
| `dlss` | `dlss-inputs` (colour, depth to R32F, velocity into the shared textures, rows flipped), `dlss-eval` (unlock, evaluate, lock: GL's view, so D3D's work and the interop wait), `dlss-output` (flipped blit into the display target) |

`present-flip` is a GL timestamp span: it cannot see the work D3D queues for the copy and
Present, so read that cost from the CPU `present` time (and the frame interval), not from it.

Counters: `bg-mfrags` (fragment shader runs of the tfrag, tie and shrub buckets and the vista
draws, millions per frame; only while shadow probe 16 is set, `(pc-set-shadow-option 44 16000000)`,
so take it from its own window: compare with the frame's pixels for the background overdraw;
`bg-tfrag-mfrags`, `bg-tie-mfrags`, `bg-shrub-mfrags`, `bg-vista-mfrags` split it, and
`bg-pre-mfrags` counts the depth prepass's runs, not in `bg-mfrags`), `bg-prepass-cuts` (prepass
runs ended early by a blended depth writer or a depth write under ALWAYS),
`shadow-maps` (actor shadow maps rendered, sun + spots), `vista-levels` (neighbours
drawn), `vista-hiz-pct`, `vista-hiz-sync-ms` and `vista-hiz-leak` (`vista.md` "Occlusion"), `ocean-mid-patches` (tessellated mid ocean source triangles), `ocean-near-cpu-ms` (render thread
in the near ocean's water setup), `ocean-rim-cpu-ms` (its rim pass), `ocean-rim-rebuilt` (1 when
the rim was rebuilt, 0 when the last one was reused), `world-cpu-ms` (render
thread in the world map prepass), `world-cascades` (world map cascades redrawn this frame; with
perf-opt bit 30 an unchanged cascade is kept, so a still camera with a frozen clock reads 0 and
the prepass costs nothing there; the harness keeps the bit off, see Optimizations),
`dlss-d3d-ms` (the DLSS evaluate on its D3D11 device, timestamp queries read a few frames late)
and `dlss-sync-cpu-ms` (render thread in the interop unlock, the evaluate submission and the
lock); `dlss-eval` minus `dlss-d3d-ms` is the interop's GPU-side overhead. Per window,
`peak-<name>` counters (n=1) give the slowest CPU frame's split: its CPU times (`peak-frame`,
`peak-render`, `peak-wait-dma`, `peak-ee`, `peak-present`, ...) and counters (`peak-vista-cpu-ms`,
`peak-world-cpu-ms`, ...), so a single hitch can be told apart from the mean.
`ocean-mid-mfrags` and `ocean-near-mfrags` are the fragment shader runs of the mid and near
textured draws, in millions per frame (pipeline statistics query, read a few frames late). They
are set only with the SSAO GPU timing switch on (`(pc-set-ssao-option 2 1)` after the load), which
also turns on other GPU queries, so take them from a probe run, not a timing run. With bit 26 on,
the mid count is the pixels the mid ocean covers; off, it includes the overdraw.
`tie-mfrags` is the same count over every tie bucket (millions per frame, same switch).

Material path costs (a probe, not a toggle): `(pc-set-pbr-param 41 MASK)` switches terms of the
background material shading off (bits in `pbr_common_frag.glsl` at `pbr_probe`: parallax, self-shadow,
maps, environment, glints, lobes, the whole response, early returns along `pbr_background`), e.g.
`--custom p64 "(pc-set-pbr-param 41 64)" "(pc-set-pbr-param 41 0)"`
(`kb/findings/2026-10-23-pbr-tie-cost.md`).

Background depth prepass: `(pc-set-ssao-option 5 MASK)`, default 7, Sandover path only (vanilla
never runs it). Bits: 1 tfrag + tie depth per level, 2 depth per shrub bucket, 4 the prepassed
draws shade without depth writes (only when the run was not cut); probes, not exact: 8 ignore the
cut, 16 both levels in one run. Measure it with `--custom bgp-off "(pc-set-ssao-option 5 0)"
"(pc-set-ssao-option 5 7)"` (`kb/findings/2026-10-23-background-prepass.md`).

CPU: `frame` (present to present), `render` (render thread submitting the frame), `wait-dma`
(render thread waiting for the game's chain), `ee` (game thread from its vsync wake to its
sync), `ee-wait` (game thread blocked in sync and vsync), `present` (the present call).

## Hitch log

`(pc-set-perf-timing -N)` (or `SANDOVER_HITCH_LOG=N` in gk's environment, active from the first
frame, before any nREPL form) is the hitch log: no GPU queries, one
`PERF-FRAME f=<n> frame_ms=.. render_ms=.. blackout=0|1` line per frame, and for a frame of N ms
or more (interval or render thread) `PERF-HITCH f=<n> name=<part> ms=..` lines with its split:
the CPU times, every counter set that frame, and `cpu-<group>` (render thread ms inside each
group's scopes). `blackout` is 1 while the screen is black (a load, or the pre-roll below).
Attribution counters, render thread ms of one-off work in that frame: `hitch-shader-ms` and
`hitch-shaders` (programs built), `hitch-upscaler-ms` (FSR 3.1 program build, FSR 4 context),
`hitch-pbr-ms` / `hitch-pbr-decode-ms` (material map upload, decode left to first draw),
`hitch-target-ms` (render target allocation), `hitch-loader-ms` (loader stages, the blocking
blackout load included), `hitch-recompiles` (driver "recompiled" warnings; NVIDIA wording only).

Time the driver spends building a pipeline for a program's first draw in a new state does not
show as a counter: it lands in the group that drew (`cpu-merc`, `cpu-ocean-mid`, ...) or, when
the GPU is the one waiting, in the next sync (`cpu-emlights-track`, `cpu-present-swap`).
`SANDOVER_HITCH_FINISH=1` (dev) puts a `glFinish` at every group scope end so each wait lands in
the group that caused it (`kb/findings/2026-10-30-warm-up.md`). In XR every gap of more than
50 ms between frames reaching the runtime logs `[xr] submit gap <ms> ms` (keep-alive frames
included), and a render stall bridged by empty frames logs `[xr] keep-alive: <n> empty frames
over a <ms> ms render stall`.

The AMD GL driver keeps its pipeline cache per executable path, so every newly staged binary
copy starts cold; measure first-use hitches on a fresh copy, warm numbers on a second run of
the same copy. Time hitches with only one game instance on the machine and no build running:
another instance sharing the GPU added frames of 50-70 ms that a solo run of the same build does
not have.

## Pre-roll after a blackout load

When a blackout ends and a level finished loading during it (the loader's count of initialized
levels changed since the screen went black), the following frames are drawn as usual but
presented black, so the first draws of the new level (driver pipeline builds, material map
uploads) happen under the loading screen. The pre-roll ends after 3 frames in a row come in
under max(50 ms, 1.5x the median of the last 32 lit frames before the blackout; 50 ms with
fewer than 16, as at boot), or after 500 ms of wall time past the first black frame, whichever
comes first. The first frame carries the load's first draws (9-12 s cold at boot) and is always
black; the frame running when the cap passes still finishes black. A blackout that loads
nothing (a respawn, a warp inside the loaded levels) has no pre-roll. The log has
`pre-roll: <n> black frames after the blackout load, <ms> ms (calm < <ms> ms)` (`, capped` when
the cap ended it) or `pre-roll: none, the blackout loaded no level`.

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

Antialiasing (opt-in, `--set aa`): the base is the shipped default, temporal AA (FSR 3.1) at
native size with the scene at one sample. `aa-msaa` switches the engine to MSAA at the run's
`--msaa` (`pc-set-aa 0 0`) with the effects still on; its marginal is what temporal AA costs over
MSAA. The FSR 3 passes are the `fsr3` group.

Render scale (opt-in, `--set scale`): `rs-77`, `rs-67`, `rs-59`, `rs-50` and `rs-33` set
`render-scale` under temporal AA in place of native; the window and `--msaa` stay the run's, so a
4K run measures 4K output. `fb` in the tables is then the render size. A marginal is what native
costs over the scaled frame (positive: the scaled frame is cheaper).

Shadow cost splits: shadow option 44 (`shadows.md`) as `--custom` pairs, e.g.
`--custom p-onetap "(pc-set-shadow-option 44 1000000)" "(pc-set-shadow-option 44 0)"`. A probe
that skips a lookup also changes what later terms do (a PBR sun term runs where the map would have
said shadow), so its marginal is a lower bound on that lookup's cost.

Opt-in probes (not looks; they exist to split a cost): `water-tess` turns the water
tessellation off (one segment per edge, near and far: `pc-set-water` 93 `TESS_SEG_M` and 8
`SUBDIV`), so its marginal is what the tessellation costs; `local-lights` sets shadow option 7
to 0 (local lights on the characters and their spot maps; a no-op on builds without the local-light spot maps).
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
`gk_proj_sha`). sandover-perf compiles GOAL for types only and never writes `out/`, so a root
whose `goal_src` differs from its `out/` (a branch with GOAL changes) runs the old compiled GOAL
silently: compile it first with a writing `(mi)` (one `sandover-capture run` with `SANDOVER_ROOT`
set to that root does it). The engine timers give a mean, minimum and maximum per window, no
percentiles.

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
so compare runs made back to back. Check the adapter line before trusting a number: gk logs
`GPU adapter: GL context on luid ..., PCI bus ...` after the renderer line in `gk.out.log`;
compared runs must share the PCI bus, and the `window on display` line must say
`(the GL adapter)` (`kb/findings/2026-10-23-gpu-selection.md`). `fb` in the tables is the size
actually rendered: the window cannot grow past the desktop, so a 3840x2160 request on a 4K desktop
may render smaller.
