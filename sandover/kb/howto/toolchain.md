# OpenGOAL (jak-project) build toolchain — install notes

Installed per-user, no admin elevation, rooted on the project's working drive so the
system drive stays untouched. Machine-specific values (real paths, drive letters, free
space) are deliberately **not** in this file — see `sandover/local/` (gitignored).

Placeholders:
- `$SANDOVER_ROOT` — the repo checkout.
- `$SANDOVER_TOOLS` — the toolchain root next to the checkout (a sibling folder of your choice), with
  scoop installed at `$SANDOVER_TOOLS/scoop`.

## What was installed

Package manager: [Scoop](https://scoop.sh), installed per-user (no `-RunAsAdmin`, no
`SCOOP_GLOBAL`).

| Env var | Value | Scope |
|---|---|---|
| `SCOOP` | `$SANDOVER_TOOLS/scoop` | User (persisted via `[Environment]::SetEnvironmentVariable`) and current shell |
| `OPENGOAL_PROJ_PATH` | `$SANDOVER_ROOT` | User — lets every OpenGOAL binary find the checkout |

Set `SCOOP` **before** running the installer, so apps, shims, cache and persist all land
under `$SANDOVER_TOOLS/scoop`. The installer adds `$SANDOVER_TOOLS/scoop/shims` to the
**user** PATH.

```powershell
[Environment]::SetEnvironmentVariable('SCOOP', "$SANDOVER_TOOLS\scoop", 'User')
$env:SCOOP = "$SANDOVER_TOOLS\scoop"
Set-ExecutionPolicy -Scope CurrentUser RemoteSigned -Force   # in BOTH pwsh and powershell.exe
irm get.scoop.sh | iex
scoop install llvm nasm task ninja cmake 7zip uv python sccache
```

Packages:

| Tool | Version at install | Notes |
|---|---|---|
| llvm | 23.1.2 | provides `clang`/`clang++`/`clang-cl` and `lld-link` |
| nasm | 3.02 | |
| task (go-task) | 3.53.1 | |
| ninja | 1.13.2 | |
| cmake | 4.4.3 | |
| 7zip | 26.03 | |
| uv | 0.12.18 | |
| python | 3.14.7 | scoop's python, coexists with any Store Python already present |
| sccache | 0.18.0 | compiler cache; see `build-and-extract.md` for how it's wired in |
| dark | 3.14.1 | pulled in automatically as a dependency of python's installer (WiX `dark.exe`) |

`git` is **skipped** intentionally — a pre-existing git on PATH satisfies the build, and a
second copy only creates PATH ambiguity. `gh` is installed separately, outside scoop.

## Other per-user config changes

- `git config --global core.longpaths true` (the OS-wide `LongPathsEnabled` policy is also
  required, but git checks its own setting too).

## Verification (fresh shell, PATH re-read from user env)

```
clang++ --version      cmake --version      ninja --version      nasm -v
task --version         7z                    uv --version         python --version
sccache --version
```

Note that `PATH` and `SCOOP` are read from the user environment by *new* processes only —
a shell opened before the install (or a child of one) will not see them.

## Cache location

Scoop's download cache lives under `$SANDOVER_TOOLS/scoop/cache`; nothing is created under
the user profile on the system drive.

## Gotcha found during install

`Set-ExecutionPolicy -Scope CurrentUser RemoteSigned` has to be run **twice** — once in
PowerShell 7 (`pwsh`) and once in Windows PowerShell 5.1 (`powershell.exe`) — because the
two editions keep separate `CurrentUser` execution-policy registry entries. Setting only
one leaves the other reporting "running scripts is disabled on this system" when invoking
scoop's `.ps1` shim. If scoop commands ever fail that way in a new terminal, re-run:
```powershell
Set-ExecutionPolicy -Scope CurrentUser RemoteSigned -Force
```
in whichever PowerShell flavor is failing.

## Moving or rebuilding the toolchain

Scoop's shims hardcode absolute paths, so a scoop root cannot simply be moved. To relocate:
point `SCOOP` at the new root, run the installer again, re-run `scoop install ...`, then
delete the old root and strip its entries out of the maintainer `PATH`.

## How to update the toolchain later

```powershell
scoop update       # refresh bucket metadata
scoop update *     # update all installed apps
scoop status       # see what's outdated
scoop cleanup *    # remove old versions after an update
```

No admin rights are required for any of the above.
