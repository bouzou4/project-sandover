# Build & extract how-to (Jak 1)

How to go from a clean clone plus a legally-owned disc image to a booted Jak 1 on
Windows. Covers Stage 0 / issues #4 and #5. Machine-specific facts (real paths, disc-image
hash, drive letters, disk sizes, timings) are deliberately **not** in this file — see
`sandover/local/machine.md` (gitignored).

Placeholders:
- `$SANDOVER_ROOT` — the repo root (this checkout).
- `$SANDOVER_DATA` — project data dir, defaults to `$SANDOVER_ROOT/sandover/data`.
- `$SANDOVER_ISO` — your Jak 1 disc image (USA, `VER=1.00`, black label / `ntscv1`). Use
  your own dump; never share or commit it.
- `$SANDOVER_CACHE_BASE` — the parent directory of `$SANDOVER_ROOT` (so it also
  contains your worktree folders when they live next to the checkout). Used as
  ccache's `base_dir` below.

All commands assume a shell with the project toolchain on `PATH` (see
`sandover/kb/howto/toolchain.md`).

## 0. Point the tools at the checkout

The engine locates the repo through the `OPENGOAL_PROJ_PATH` environment variable:

```powershell
[Environment]::SetEnvironmentVariable('OPENGOAL_PROJ_PATH', "$SANDOVER_ROOT", 'User')
```

Without it, `setup_project_path()` in `common/util/FileUtil.cpp` falls back to looking for
a sibling `data/` folder next to the exe, or the literal substring `jak-project` in the
exe's own path — and `decompiler.exe` has no `--proj-path` flag to override it, so a
checkout named anything else used to fail with "Failed to initialize project path". The
env var removes that constraint entirely; **do not** create a `jak-project` directory
junction, the old workaround. `sandover/Taskfile.yml` also exports the variable itself,
so the tasks below work even in a shell where it isn't set.

## 1. Extract the disc image

```sh
7z x "$SANDOVER_ISO" -o"$SANDOVER_ROOT/iso_data/jak1"
cat iso_data/jak1/SYSTEM.CNF
```

Expect something like:
```
BOOT2=cdrom0:\SCUS_971.24;1
VER=1.00
VMODE=NTSC
```

`SCUS-97124` + `VER=1.00` + NTSC is the USA black-label disc → decomp config `ntscv1`. A
different serial/VER means a different `task set-decomp-*` in the next step.

`iso_data/` is gitignored — confirm with `git status` that extracting it adds nothing to
the working tree.

## 2. Select game + decomp config

```sh
task set-game-jak1
task set-decomp-ntscv1   # or set-decomp-ntscv2 / -pal / -ntscjp / -ntscko
```

This writes `scripts/tasks/.env` (gitignored). `git status` should be clean afterward.

## 3. Build the C++ tools (Release)

Use the sandover tasks, which pin the preset, the compiler cache and the job count:

```sh
task -t sandover/Taskfile.yml configure
task -t sandover/Taskfile.yml build
```

- `configure` runs `cmake --preset=Release-windows-clang` (Ninja generator, `clang`/
  `clang++` with `lld-link` — the GNU-style driver, which LLVM's Windows build
  auto-detects the VS/MSVC toolchain for, so no VS Developer shell is needed) and, if
  `SANDOVER_CCACHE=1` or `SANDOVER_SCCACHE=1` is set, wires the corresponding launcher in
  as `CMAKE_C_COMPILER_LAUNCHER` / `CMAKE_CXX_COMPILER_LAUNCHER` (ccache wins if both are
  set). Neither is on by default (ADR-0012). It also passes `-C sandover/cmake/configure-seed.cmake`
  when the file exists (below).
- `build` runs `ninja -j (logical threads - 2)`. The job count is computed from
  `NUMBER_OF_PROCESSORS` at run time — never hardcode it.
- Release is the project default (RenderDoc works fine with it); RelWithDebInfo is a later
  option if deeper native debugging is needed.
- Both steps are CPU/IO heavy — run one at a time, nothing else building concurrently.

**sccache:** verified working with this clang/Ninja toolchain on Windows — 1425 compile
requests, 0 compilation failures, 0 unsupported-compiler calls, 0 cache errors. It is
kept. Note the trade-off: on a cold cache it makes a full build roughly 1.8x slower than
the same build without it (every translation unit is preprocessed and the result written
to the cache), but a subsequent full rebuild hits the cache 100% and takes about 45% of
the cold-cache time, i.e. faster than an uncached build — worth it as soon as you rebuild
from scratch more than once, e.g. when switching branches. `SCCACHE_DIR` points at
`$SANDOVER_DATA/sccache` so nothing lands on the system drive.

**sccache gotcha:** the sccache *server* reads `SCCACHE_DIR` only when it starts, and it
lingers after a build. If a server is already running with a different cache directory,
the task's env is ignored. `SCCACHE_DIR` is therefore also set as a user environment
variable; after changing it, run `sccache --stop-server` so the next invocation picks the
new location up. Check with `sccache --show-stats | findstr "Cache location"`.

**sccache never hits across worktrees:** sccache hashes the absolute source/include paths
on the compiler command line. Every worktree folder has a
different absolute path than the primary checkout and than every other worktree, so no
two of them ever produce the same hash — 0 hits out of 1885 compiles were observed this
way. sccache has no setting to normalize that out. If you build from more than one
worktree, use ccache instead.

**ccache (recommended for multi-worktree work):** install per-user with
`scoop install ccache` (already on `PATH` via the project's scoop shims once installed).
Set `SANDOVER_CCACHE=1` before `task configure` to wire it in as the compiler launcher.
Unlike sccache, ccache's `base_dir` option rewrites absolute paths under a common
ancestor to relative before hashing, so identical relative source layouts under different
worktrees hash identically and hit. This only works if the following are set as **user**
environment variables first — the Taskfile deliberately does not compute or override
them, since a per-checkout default (the way `SCCACHE_DIR` works today) would give every
worktree its own cache directory and defeat the whole point:

```powershell
[Environment]::SetEnvironmentVariable('CCACHE_DIR', '$SANDOVER_DATA\ccache', 'User')
[Environment]::SetEnvironmentVariable('CCACHE_BASEDIR', '$SANDOVER_CACHE_BASE', 'User')
[Environment]::SetEnvironmentVariable('CCACHE_NOHASHDIR', 'true', 'User')
[Environment]::SetEnvironmentVariable('CCACHE_SLOPPINESS', 'pch_defines,time_macros,include_file_mtime', 'User')
[Environment]::SetEnvironmentVariable('CCACHE_COMPRESS', 'true', 'User')
```

Substitute the real paths for `$SANDOVER_DATA` and `$SANDOVER_CACHE_BASE` (placeholders
don't expand in `[Environment]::SetEnvironmentVariable`; real values in
`sandover/local/machine.md`).

- `CCACHE_DIR` — one fixed location every worktree can see; point it at the primary
  checkout's data dir, not a per-worktree path.
- `CCACHE_BASEDIR` — the common ancestor of the primary checkout and every worktree
  (the primary checkout and your worktree folders both live under this). This is what
  makes cross-worktree hits possible; without it, ccache behaves like sccache (0 hits
  across worktrees).
- `CCACHE_NOHASHDIR=true` — otherwise the current working directory (which differs per
  worktree) still leaks into the hash and defeats `CCACHE_BASEDIR`.
- `CCACHE_SLOPPINESS` — `pch_defines` is a no-op safety margin (no precompiled headers in
  this project today); `time_macros` keeps `__DATE__`/`__TIME__` use cacheable;
  `include_file_mtime` avoids spurious misses from worktree checkouts giving headers
  different mtimes than the ones in the cache.

Env vars are read once per process; open a new terminal after setting or changing them.
Unlike sccache, ccache has no lingering server, so there's no `--stop-server` step.
Verify with `ccache -s` (`ccache -z` to zero the counters before a build you want to
measure). See ADR-0012's amendment for the cross-worktree hit numbers this was verified
against before being wired in.

**Gotcha:** the LLVM scoop package registers itself with a persistent PATH addition rather
than a shim. A shell opened *before* the install won't see `clang`/`clang++`/`clang-cl`
even though scoop reports them installed — open a genuinely new terminal. Same for
`7z`/`cmake`/`ninja`/`task`/`nasm`/`uv`/`sccache`.

**Configure seed:** a plain configure runs ~280 try-compiles (SDL most of them, then curl,
openal-soft, cubeb, openxr), each a ninja + clang compile + lld link: an estimated 6 min on a quiet
machine, 51 min beside another build. `sandover/cmake/configure-seed.cmake` holds their cached
results for `cmake -C`, so a fresh configure skips them (about 1 min). It holds check names, values
and file hashes only. It applies nothing, and the checks run as usual, when:

- the build tree already has a `CMakeCache.txt` (its own results stay; `task configure` and
  `sandover.engine` pass `-C` only for a new tree and only with `Release-windows-clang`);
- the build type is not `Release` or the C compiler is not `clang`;
- the CMake version, the configured clang's version (`clang version X.Y.Z`; vendor and commit
  suffixes are ignored), or the MSVC toolset and Windows SDK versions clang searches differ from
  the ones it was made with;
- the top-level `CMakeLists.txt` or `CMakePresets.json` changed (they set the flags and
  third-party options every check sees).

Otherwise each group of checks applies only while the CMake files that define it, and the
try-compile sources they name (e.g. curl's `CMake/CurlTests.c`), hash the same; a changed group
runs its checks and the configure log says how many and asks for a refresh. Refresh from a
configure run without it:

```sh
cmake --preset=Release-windows-clang -B <scratch build dir>   # no -C
uv run --no-sync python -m sandover.buildseed harvest <scratch build dir>
uv run --no-sync python -m sandover.buildseed verify <plain build dir> <seeded build dir>
```

`harvest` writes the seed into the source tree the scratch build was configured from. `verify`
compares the cache values, generated headers, `compile_commands.json` and `build.ninja` of two
configures of one commit with their paths normalised; it must print `equivalent`.
`tests/test_buildseed.py` fails while the seed is stale for this checkout.

**ccache across worktrees:** with `CCACHE_BASEDIR` set, every object hits across worktrees
except SDL's: its 234 sources use a precompiled header whose bytes differ per worktree, so they
and the 2 header compiles miss (236). ccache passes the rewritten relative paths to clang, so
`__FILE__` in a ccache build reads `..\..\..\game\...` (an uncached build embeds the absolute
path).

**Relocating the checkout:** a CMake build tree bakes in absolute paths, so after moving
the repo you must delete `out/build` and re-run `configure`. `iso_data/`, `decompiler_out/`
and `out/<game>/` survive a move untouched.

## 4. Extract assets

```sh
task -t sandover/Taskfile.yml extract
```

Runs `decompiler.exe` against `./iso_data` → `./decompiler_out` with
`decompile_code: false, levels_extract: true` (asset/level extraction only — no GOAL code
decompilation, no game compilation). On success you get per-level stats and a
`Decompiler has finished successfully in N.NN seconds.` line.

## 5. Compile the GOAL code and boot the game

Upstream's flow is two terminals: the REPL in one, the game in the other.

```sh
task -t sandover/Taskfile.yml repl     # then type (mi)
task -t sandover/Taskfile.yml boot     # in a second terminal, after (mi) succeeds
```

In a headless/scripted context, driving the interactive REPL's stdin is awkward (replxx
expects a real console); two options that both work:

- **Non-interactive one-shot:** `goalc -c "(mi)"` — runs the command and exits. Good for
  automation.
- **Real REPL:** start `goalc --user-auto --game jak1` with stdin fed from a file
  containing `(mi)` and stdout/stderr redirected to files you can tail, since the REPL
  keeps running afterwards as the live listener.

Wait for `Successfully built all N targets in N.NNs` with no errors before booting. A
successful `(mi)` populates `out/<game>/` with `.DGO`/`.CGO` files, `fr3/` level data and
`iso/` text assets.

### Confirming a real boot

In the `gk` process's stdout, look for:
- An `OpenGL context renderer: ...` line naming the actual GPU.
- `Elapsed time for level = ...` and `NOTICE: loaded <level>-vis, ...` / `link finish:
  <level>-vis` for the intro level (beach, for `-boot -fakeiso`).
- Overlord DGO load lines (`[OVERLORD] FS Open ...DGO`) for the intro/beach DGO.

And separately, that the process is still alive and responding some seconds later — the
window should show the title/intro, not a crash dialog.

### Shutting down cleanly

Stop the game process first, then the REPL/compiler process. Terminating by PID is fine
for a scripted session; from an interactive REPL, `(exit)` is the normal path.

## Fixed ports

- Game runtime listener: **8112**
- Compiler nREPL: **8181**

Only one build at a time; several game instances run at once on slots (see `sandover/tools/sandover/locks.py` and `kb/howto/rig-capture.md`).

## How to launch

`gk` and `goalc`'s listeners (game runtime, nREPL, JSON debug server) bind to
`127.0.0.1` by default, so Windows Firewall no longer prompts for them. Set
`OPENGOAL_LISTEN_ALL` (any non-empty value) before launching if you need them reachable
from another machine on the network. If you previously added firewall allow rules for
`gk.exe`/`goalc.exe` to silence these prompts, they can now be removed.

## Directory summary

| Directory | Produced by | Committed? |
|---|---|---|
| `iso_data/` | step 1 (7z extract) | never (gitignored) |
| `decompiler_out/` | step 4 (`extract`) | never (gitignored) |
| `out/build/` | step 3 (cmake/ninja) | never (gitignored) |
| `out/<game>/` | step 5 (`(mi)`) | never (gitignored) |
| `$SANDOVER_DATA/sccache/` | step 3 (compiler cache, `SANDOVER_SCCACHE=1`) | never (gitignored) |
| `$SANDOVER_DATA/ccache/` (via user-scope `CCACHE_DIR`) | step 3 (compiler cache, `SANDOVER_CCACHE=1`) | never (gitignored) |

Timings, disk usage and the detected serial/version for this machine's run:
`sandover/local/machine.md`.
