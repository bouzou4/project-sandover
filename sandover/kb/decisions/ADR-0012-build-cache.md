# ADR-0012: Build cache (sccache, then ccache) is opt-in
Status: amended    Date: 2026-09-23    Deciders: project lead

## Context
`sandover/Taskfile.yml`'s `configure` task wraps the compiler with `sccache`
unconditionally. Measured on this toolchain, a cold build is 1.8x slower with sccache than
without it, and ninja's incremental builds never call the compiler launcher at all once
the build tree is warm — so sccache adds cost on the common paths (cold configure, warm
incremental build) and only pays off on a cache hit after a clean/reconfigure.

## Options (with cost/risk)
**Always on (current Taskfile behavior).** Cost: every cold configure pays the 1.8x
penalty even when no cache hit is possible yet (e.g. first build ever, or after a
dependency bump that invalidates the cache). Risk: low functionally, but a silent
performance regression with no easy opt-out.
**Opt-in via an environment variable (chosen).** Cost: one extra step
(`SANDOVER_SCCACHE=1`) to get caching on repeat cold configures (e.g. bisecting, CI-style
clean rebuilds). Risk: low — default behavior matches the common case (incremental ninja
builds, which never call the launcher anyway).

## Decision
`sccache` is opt-in only. Setting `SANDOVER_SCCACHE=1` before `task configure` adds
`-DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache` to the CMake
invocation; leaving it unset configures without a launcher. Job count stays computed at
run time as (logical threads − 2) in either case — unaffected by this decision.

## Consequences
- `sandover/Taskfile.yml`'s `configure` task branches on `SANDOVER_SCCACHE`.
- Anyone doing repeated clean/reconfigure cycles (bisecting, cache-warming) sets
  `SANDOVER_SCCACHE=1` deliberately; default `task configure` stays fast for the common
  single cold-build case.
- Source: local build timing on this toolchain (`sandover/local/machine.md`); no published
  number is claimed here beyond the 1.8x ratio measured locally.

## Amendment (2026-09-23): ccache replaces sccache as the recommended opt-in launcher

### Context
A real build (1885 compiles) with `SANDOVER_SCCACHE=1` got 0 cache hits. Diagnosis:
sccache hashes the absolute source/include paths on the compiler command line, and this
project's worktrees (a worktree folder of your choice per branch) each have a different absolute path
than the primary checkout and each other, so no two builds ever produce the same hash —
sccache cannot hit across worktrees on this project no matter how warm the cache is.
sccache has no option to normalize that away.

### Decision
`ccache` replaces sccache as the recommended launcher. ccache's `base_dir` option
rewrites absolute paths under a common ancestor to relative paths before hashing, so
identical relative source/include layouts hash identically regardless of which worktree
they live under. Verified with a throwaway CMake project (Ninja + clang++, no response
files for compiles, matching this project's `-I<abs>` / absolute-source compile-command
shape) built once under one absolute path and once under another:
- `CCACHE_BASEDIR` unset (sccache-equivalent behavior): 0/8 hits, cross-path build.
- `CCACHE_BASEDIR` = the common ancestor of the primary checkout and
  your worktree folders (normally the parent directory of `$SANDOVER_ROOT`),
  `CCACHE_NOHASHDIR=true`,
  `CCACHE_SLOPPINESS=pch_defines,time_macros,include_file_mtime`: second build hit
  4/4 objects (100% direct hits).

Settings (see `sandover/kb/howto/build-and-extract.md` for the exact commands):
- `base_dir` = the common ancestor of the primary checkout and every worktree (the
  parent directory of `$SANDOVER_ROOT`) — must be set, since it's the mechanism that
  makes cross-worktree hits possible at all.
- `hash_dir = false` (`CCACHE_NOHASHDIR=true`) — otherwise the current working directory
  (which differs per worktree) still leaks into the hash.
- `sloppiness = pch_defines,time_macros,include_file_mtime` — this project has no
  precompiled headers today, so `pch_defines` is a no-op safety margin; `time_macros`
  keeps any `__DATE__`/`__TIME__` use cacheable; `include_file_mtime` avoids spurious
  misses from worktree checkouts giving headers different mtimes than the cached ones.
- `cache_dir` — must be one fixed location visible to every worktree (not derived from
  each checkout's own root the way `SCCACHE_DIR` is), or nothing will ever be shared
  regardless of `base_dir`. Set as a user-scope env var, not computed by the Taskfile.
- `compression = true` — already ccache's default.

`sandover/Taskfile.yml`'s `configure` task adds
`-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache` when
`SANDOVER_CCACHE=1`. The sccache branch (`SANDOVER_SCCACHE=1`) stays for anyone who wants
a single-checkout launcher without setting up ccache's user-scope env vars; ccache wins if
both are set. The 1.8x cold-cost-vs-no-launcher tradeoff from the original decision still
applies to ccache and is why it stays opt-in rather than default.
