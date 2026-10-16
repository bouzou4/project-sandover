# ADR-0002: Data location
Status: accepted    Date: 2026-09-23    Deciders: maintainer

## Context
Sandover generates and consumes several classes of data: extracted game assets
(`iso_data/`, `decompiler_out/`, `out/`), and project data (screenshot/capture sets,
FLIP/parity goldens, unbake fit results, Rerun `.rrd` recordings, FiftyOne texture
datasets, lock files, and the built MkDocs KB site). All of it derives from the maintainer's
copyrighted Jak and Daxter disc image and must stay private under the project guardrails: never in git, never uploaded anywhere except to the vision judge used for texture classification and capture review.

Constraints on this machine (concrete layout, capacities and paths: `sandover/local/`,
gitignored): the system drive is near capacity and must not be written to; one large
working drive — a spinning disk, not an SSD — has room for everything and already hosts
the repo at `$SANDOVER_ROOT`. A NAS is reachable by the maintainer but is explicitly read-only
for us per the guardrails — we may copy from it, never write to it, and never treat it as
a working directory. The disc image is already staged locally at `$SANDOVER_ISO`.

## Options (with cost/risk)
**(a) NAS as working directory over SMB.**
Cost: multi-GB builds, extraction, and capture runs would incur network/SMB latency on
every read and write, which is a poor fit for CMake builds and high-frequency capture
loops (rig shots, Rerun logging). Risk: directly violates the "NAS is read-only for us"
guardrail — writing generated data there would make it a working directory, which is
explicitly disallowed. Rejected.

**(b) The local working drive (chosen).**
Cost: it is a spinning disk, so random access is slower than an SSD, and it is the same
disk that already hosts the CMake build and game data — builds, extraction, and captures
all compete for the same spindle. Risk: low — it's local, has ample free space, and
keeps everything inside the guardrails (nothing leaves the machine except via judging).
Chosen because it's the only option with enough free space that also respects the
NAS-read-only and system-drive-off-limits constraints.

**(c) The system drive.**
Cost/Risk: little free space, and it is explicitly off-limits — a single extraction or a
handful of Rerun recordings could fill it and break the maintainer's other uses of that drive.
Rejected outright; nothing goes there.

## Decision
- All generated and captured data stays **local on the working drive**:
  - Game data: the repo's gitignored `iso_data/`, `decompiler_out/`, `out/` inside
    `$SANDOVER_ROOT`.
  - Project data: captures, goldens, unbake fits, Rerun `.rrd` recordings, FiftyOne
    datasets, lock files, sccache storage, and the built MkDocs site under
    `$SANDOVER_ROOT/sandover/data` (gitignored).
  - `$SANDOVER_DATA` is an environment variable that defaults to
    `$SANDOVER_ROOT/sandover/data`. All tools read data paths from `$SANDOVER_DATA` rather
    than hardcoding a path, specifically so the maintainer can relocate this data (e.g. to a
    NAS share) later without a tooling change.
- The disc image itself stays exactly where it is, referred to only as `$SANDOVER_ISO`,
  and is treated strictly read-only by all tooling (extraction reads it, nothing writes
  to it). Its real path and hash live in `sandover/local/` and are never committed.
- The **NAS is a backup target only**, copied to by the maintainer at their discretion. It is
  never a working directory and is never written to by tooling.

## Consequences
- Working-drive I/O is the shared bottleneck for builds, extraction, and capture
  throughput; if build/capture times become painful, the recommended fix is for the maintainer
  to free space on a faster drive and move the build dir (and optionally `$SANDOVER_DATA`)
  there — no code change needed beyond re-pointing the env var and CMake build dir, since
  paths are already parameterized.
- The system drive must stay untouched by any sandover tooling; if tool-side artifacts
  (caches, logs, agent tooling state) grow large enough to threaten its headroom, they
  should be relocated to the working drive — this is a housekeeping note, not a sandover
  data-path change.
- Any future NAS relocation of `$SANDOVER_DATA` must preserve the read-only-for-tools
  rule during migration: the maintainer copies data to the NAS, verifies with a hash, and only
  then tools may be pointed at a *new* local mount/cache — tools never perform the NAS
  write themselves.
