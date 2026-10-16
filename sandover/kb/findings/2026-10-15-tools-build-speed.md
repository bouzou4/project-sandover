---
title: Why a fresh worktree's first engine build is slow, and what the tooling can cut
date: 2026-10-15
issue: tools/build-speed
status: verified
sha: cb82ba027
---
## Answer (≤5 lines)
Configure time was ~280 serial try-compiles (SDL most of them). A committed initial-cache seed
skips them. A plain configure is an estimated 6 min on a quiet machine and took 51 min beside
another build; a seeded one took about 1 min, proven equivalent. Compile time is not a cache path
problem: ccache hits 80.6 % across worktrees, and all 236 misses are SDL (its precompiled header).
The first build stays at 4-5 min here: ccache hits top out at ~9-12/s on the project disk.

## Evidence
Scratch worktrees `tools-build-speed-a`, `-b` at cb82ba027, gk target, `-j 14` (threads - 2),
`build-b` lane, another author's build in the `build` lane during every run.

| Run | Recipe | Configure s | First gk build s | ccache hits | Load |
|---|---|---|---|---|---|
| r1 (a) | author recipe: preset + `ccache` via scoop shim, no seed | 3055 | 638 | 979/1215 (80.6 %) | CPU 33 % configure, 49 % build |
| r2 (b) | seed + real ninja/ccache exes, with `--profiling-output` | 185 | 318 | 979/1215 (80.6 %) | 35 % / 45 % |
| r4 (b) | shipped recipe (`sandover.engine build`) | 60 | 245 | 979/1215 (80.6 %) | 24 % / 46 % |
| r3 (a) | seed + `CMAKE_DISABLE_PRECOMPILE_HEADERS` (SDL cold) | 56 | 277 | 979/1213 | 32 % / 48 % |
| r5 (a) | seed + no PCH, warm | 61 | 308 | 1213/1213 (100 %) | 55 % / 36 % |

- The build column is not a recipe comparison. r1 and r4 hit and missed the same objects; r4 ran
  after r1-r3 had read the same cache entries (all its hits direct, r1 had 102 preprocessed), and
  the shim saving is ~1215 x 0.1 s / 14 = ~9 s. Read 638 -> 245 s as an ordering effect.
- Configure attribution (r1 log): 273 try-compiles, each ninja + clang compile + lld link; by hand
  one costs 0.5 s compile + 0.75 s link unloaded (273 x 1.25 s = ~6 min), ~11 s each during r1.
  Scoop shims add ~0.1 s per spawn (`ninja --version` 180 ms via shim, 88 ms direct). Seeded
  configure (r2 profile): 7 uncached try-compiles left, compiler identification, 38 s of
  generation under load.
- Seeded configure equals a plain one: `python -m sandover.buildseed verify` on the r1 and r2
  trees prints `equivalent` (cache values, generated headers, compile_commands.json, build.ninja;
  paths, tool locations and custom-command ids normalised). gk.exe from r1, r2 and r3 differ only
  in the PE link timestamp (2 bytes at 0x80).
- Misses: ccache's debug hash input for an SDL file differs only in `pch_hash`; the
  `cmake_pch.h.pch` built in each worktree embeds that worktree's paths. The 236 misses are SDL's
  234 sources using it plus its 2 header compiles.
- ccache already path-normalises worktree objects: `CCACHE_BASEDIR` makes ccache run clang with
  relative paths, so worktree gk.exe holds `..\..\..\game\...` for `__FILE__` (Assert.h) and no
  absolute paths; the uncached primary build holds 166. `-ffile-prefix-map` is not needed.
- Hit cost: serial hit ~250 ms (log: 0.5 s hashing, 0.29 s writing the 0.7 MB object, 0.39 s
  updating the stats file, under load). Throughput at -j1/4/8/14: 1.7/6.3/8.4/8.9 hits/s with the
  cache on the project disk; 3.4/11.1/17.5/19.0 with the cache and objects on an NVMe drive (56
  files). `CCACHE_NOSTATS` and `CCACHE_INODECACHE` moved it within noise. r5: object steps median
  2.25 s each at -j14; 95 openal-soft C++ module scans (`.ddi`, never cached) median 2.4 s.
- `git worktree add` itself took ~25 min per worktree (23,755 files) on that disk.

## Seed safety (review of PR #399)
The seed applies nothing for an existing build tree, a non-Release build type, a non-clang C
compiler, or another CMake, clang (`X.Y.Z`), MSVC toolset or Windows SDK version; nor when the
top-level `CMakeLists.txt` or `CMakePresets.json` changed. Each group is keyed on its defining
CMake files plus the try-compile sources they name (curl `CMake/CurlTests.c`, cubeb
`compile_tests/oss_is_v4.c`). `tests/test_buildseed.py` runs the seed under `cmake -P` for each case.

## Rejected
- Disabling SDL's precompiled header: gk.exe stays byte-identical, but SDL3.dll differs in 10 call
  displacement bytes (function order), and r5 (100 % hits) was no faster than r4.
- Warm build-tree template per master commit: `.ninja_deps` and the SDL `.pch` hold the template's
  absolute paths, so a copied tree would skip rebuilding dependents of an edited header or load the
  template's headers; not provably correct, and hits are not the bottleneck.
- ccache stats/inode-cache tweaks: no measurable gain.
- Halving `-j` while the other lane is busy: unmeasured; `-j` stays logical threads - 2.

## Implications for the design
- Seeded configure holds a build lane ~1 min instead of 15-50 min, which was most of today's lane
  queueing (configures held the lock 18+ min, a capture and a kernel author waited behind them).
- Configure and compile write only the worktree's own build tree and the ccache dir (which locks
  itself); neither needs a lane for correctness, only to bound CPU/disk contention. Proposal, not
  implemented: configure outside the lanes under a one-at-a-time `configure` lock that perf's
  quiet mode also takes. Lane cap and rig rules unchanged.
- Every lock hold now appends start/lock/seconds/pid/purpose to `$SANDOVER_DATA/locks/holds.log`
  and warns past 10 min; `python -m sandover.engine holds --min 10` lists them.

## Follow-ups
- User decision: ccache dir and worktrees on an SSD; an antivirus exclusion for build trees and
  the ccache dir.
- Refresh the seed after a top-level, CMake, clang, toolset or third-party CMake change (the
  configure log asks for it; the test fails when stale).
