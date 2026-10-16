# ADR-0001: Repo and worktree strategy
Status: accepted    Date: 2026-09-23    Deciders: maintainer

## Context
Sandover modernizes Jak and Daxter's lighting on top of `open-goal/jak-project`. We need
a repo/branching model that (a) keeps the maintainer's copyrighted game data private and out of
any upstream-facing surface, (b) lets us pull upstream fixes without risking an accidental
push or issue/PR to `open-goal/jak-project`, and (c) supports engine spikes (lightbench,
SSAO, stereo) that may need to build and run concurrently with other work.

Everything runs on a single workstation with one GPU. The repo and all game data live on
one working drive (`$SANDOVER_ROOT`), which is a spinning disk rather than an SSD; the
system drive is near capacity and stays untouched. A CMake build of jak-project is
multi-GB and a full rebuild is CPU- and I/O-heavy. Git worktrees have no `out/`, `iso_data/`, or
`decompiler_out/` data of their own, and each one needs its own multi-GB CMake build
directory. One build or extraction, and one running game instance (fixed ports 8112 game,
8181 nREPL), can happen at a time on this box. (Actual hardware and drive layout:
`sandover/local/`, gitignored.)

## Options (with cost/risk)
**(a) One worktree per spike, each with its own full build.**
Cost: every concurrent spike doubles (or triples) build time and disk churn on a single
spinning disk; the available core count is not enough to run two multi-GB C++ builds
without both regressing badly, and seek contention makes it worse than the core count
alone suggests. Risk: easy to silently balloon disk usage across N worktrees × N build
dirs; no natural serialization point, so the "one build at a time" rule has to be enforced
by convention (fragile) rather than layout.

**(b) Single checkout, serialized by a lock (chosen as default).**
Cost: engine work is fully serialized — feature branches take turns via a build lock, so
long builds block other engine spikes. Risk: low; this is the simple, predictable case
and matches the resource reality (one build/extraction, one game instance at a time).
Benefit: only one `iso_data/`, `decompiler_out/`, `out/` on disk; no data duplication; no
risk of two builds racing for the same ports.

**(c) Hybrid: primary checkout plus at most one extra worktree pointed at the primary
project root (chosen as the escape hatch).**
Cost: still limited to one *extra* concurrent build (two builds total, which is already
borderline for this box), plus the discipline of always launching the worktree's
`gk`/game process against `$SANDOVER_ROOT` (via `OPENGOAL_PROJ_PATH` or `--proj-path`) so
it reads the primary checkout's game data instead of expecting its own copy. Risk:
someone forgets the override and the worktree either fails to find data or (worse) an
contributor tries to populate its own `iso_data/`/`decompiler_out/`, duplicating tens of GB.
Mitigated by naming convention and by this ADR being the documented rule. Benefit:
unblocks genuinely parallel, independent engine spikes (e.g., one contributor on SSAO
while another works stereo) without duplicating game data or risking port collisions,
as long as it stays capped at one extra worktree.

## Decision
- **One private repo** holds the full upstream history. `origin` is that private repo
  (read/write). `upstream` is `open-goal/jak-project`, fetch-only in practice: we
  periodically run `git merge upstream/master` into `master`, but we **never push to
  `upstream`** and **never open issues or PRs on `open-goal/jak-project`**.
- The **primary checkout, `$SANDOVER_ROOT`**, owns the only copy of game data
  (`iso_data/`, `decompiler_out/`, `out/`, all gitignored) and hosts the primary CMake
  build. Engine work happens on feature branches inside this checkout by default,
  serialized by the build lock — one build at a time. This is the default for all engine
  spikes (lightbench, SSAO, stereo) unless explicitly justified otherwise.
- **Parallel engine work is the exception, not the rule**: at most **one** extra worktree
  at a time, created next to the primary checkout as `../sandover-wt/<branch>`, with its
  own build dir (`out/build` inside the worktree). It always runs with
  `OPENGOAL_PROJ_PATH` (or `--proj-path`) set to `$SANDOVER_ROOT`, so the game process
  reads the primary checkout's `iso_data/`/`decompiler_out/`/`out/` data rather than
  expecting its own. Before creating a second worktree, the first one must be removed or
  the exception re-justified.
- **Python/tooling work** under `sandover/tools` (rig, unbake, judge, gallery, metrics —
  no CMake build required) may use `isolation: worktree` freely; this doesn't touch the
  build-lock budget.
- **Locks**: implemented as file locks via the `filelock` Python package at
  `sandover/data/locks/{build,game}.lock`. Every tool that builds or launches must take
  the relevant lock before proceeding. Fixed ports: game `8112`, nREPL `8181` — a second
  concurrent game instance is refused rather than racing for these ports.

## Consequences
- Engine spikes default to sequential feature branches in the primary checkout; expect
  queueing when multiple contributors want to build at once. This is acceptable on a single
  workstation with one spinning working disk — it's cheaper than duplicated multi-GB
  builds.
- The one-extra-worktree escape hatch must always point the project root at
  `$SANDOVER_ROOT`; any contributor creating a worktree records the worktree in the relevant howto or finding note
  so the rule doesn't get relearned by trial and error.
- No contributor may create a second extra worktree while one already exists under
  `../sandover-wt/`; check for that directory (or ask) before creating one.
- `filelock`-based build/game locks are mandatory infrastructure — any new tool that
  builds, extracts, or launches the game must take the corresponding lock, checked in
  code review, not just convention.
- Because `upstream` is fetch-only by policy (not by remote URL restriction — the git
  remote itself is push-capable), contributors must never run `git push upstream ...` and must
  never use `gh` against `open-goal/jak-project`; this is the project's policy, enforced by
  convention, not by tooling.

## Amendment (2026-09-23)
The single-game-instance rule is withdrawn. `gk` accepts `--port` and `--config-path`, and `goalc` accepts `--port` and `--debug-port`, so concurrent instances run on per-slot ports with their own config dirs, launched from a per-build copy of the binaries. The build lock now only serializes builds. GPU cost measurements must still be taken with no other instance running.
