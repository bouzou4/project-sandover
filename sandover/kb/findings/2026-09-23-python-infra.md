---
title: Are uv, filelock, and MkDocs Material solid buy-don't-build picks for the Python tooling stack?
date: 2026-09-23
workstream: tooling evaluation
status: verified
sha: 76e8eec
---
## Answer (<=5 lines)
All three keep. uv is actively developed (daily releases, 90k stars) and covers workspaces plus pinned PyTorch CUDA indexes on Windows, but `--torch-backend=auto` is still preview/experimental so pin indexes explicitly instead. filelock uses real Windows kernel locks (`LockFileEx`, mandatory, byte-range) that the OS releases automatically on process exit, including a hard kill, so stale locks are not expected in the target setup. MkDocs Material is in maintenance mode (feature-frozen since Nov 2025, end-of-life Nov 2026) but all former Insiders features, including Mermaid and the new search, are now in the free MIT edition, so it still fits a one-off offline knowledge base.

## Picks
| item | license (SPDX) | Windows | last release / last commit | maintenance | verdict | gotcha |
|---|---|---|---|---|---|---|
| uv (astral-sh/uv) | MIT OR Apache-2.0 | native, first-class target | 0.12.18 / 2026-09-22 (release), last commit 2026-09-23 | very active: 90k stars, 2920 open issues, releases ~daily | keep | `--torch-backend=auto` is explicitly experimental/preview and unsupported inside `uv add`/`uv sync`/`uv run`; pin CUDA index explicitly in `pyproject.toml` via `tool.uv.index` + `tool.uv.sources` markers instead |
| filelock (tox-dev/filelock) | MIT | native (`LockFileEx` byte-range, kernel-enforced/mandatory) | 4.0.1 / 2026-09-19 (release), last commit 2026-09-22 | small but active: 975 stars, only 2 open issues | keep | `SoftFileLock` fallback had a TOCTOU symlink race (CVE-2026-22701), fixed in 3.20.3 — make sure the pin is >= that; use the OS-lock `FileLock` class, not `SoftFileLock`, for the build/instance mutex |
| MkDocs Material (squidfunk/mkdocs-material) | MIT | native (pure Python static site generator, no OS-specific code path) | 9.7.7 / 2026-07-17 (release), last commit 2026-09-15 | feature-frozen since Nov 2025 ("maintenance mode"), security/bugfix support committed through Nov 2026, only 1 open issue on the repo | keep | project is winding down in favor of Zensical; fine for a private, low-churn internal KB but re-evaluate before investing in deep customization, since no new features will land and support ends Nov 2026 |

## Evidence
- uv license is dual MIT OR Apache-2.0 (GitHub API reports Apache-2.0 as `license.spdx_id` but both LICENSE-MIT and LICENSE-APACHE exist in the repo): https://github.com/astral-sh/uv/blob/main/LICENSE-MIT , https://docs.astral.sh/uv/reference/policies/license/
- uv repo stats via `gh api repos/astral-sh/uv`: pushed_at 2026-09-23T04:06:12Z, 90078 stars, 2920 open issues; latest release tag `0.12.18` published 2026-09-22T23:00:51Z.
- uv supports pinning PyTorch to a specific CUDA index per-platform with `[[tool.uv.index]] explicit = true` plus `tool.uv.sources` marker expressions (e.g. `marker = "sys_platform == 'win32'"`), documented for Windows CUDA builds: https://docs.astral.sh/uv/guides/integration/pytorch/
- `--torch-backend=auto` auto-detects the installed CUDA driver and picks the matching PyTorch index, but is flagged as an experimental/preview flag that "may change without warning" and does not work with `uv add`/`uv lock`/`uv sync`/`uv run`: https://github.com/astral-sh/uv/issues/12994 , https://docs.astral.sh/uv/guides/integration/pytorch/
- `uv add --index` can fail to resolve PyTorch CUDA wheels on Windows for some Python versions, a known open issue: https://github.com/astral-sh/uv/issues/11532
- uv workspaces (`tool.uv.workspace` table, one lockfile/one venv across member packages under e.g. a `tools/` directory, `{ workspace = true }` sources) are a first-class, documented feature: https://docs.astral.sh/uv/concepts/projects/workspaces/
- Known Windows-specific uv issues logged: antivirus flags the installer as malicious on some systems, inconsistent PATH configuration after install, and a reported file-locking crash on Windows CI runners: https://github.com/astral-sh/uv/issues/10079 , https://github.com/astral-sh/uv/issues/10014 , https://github.com/astral-sh/uv/issues/2767
- filelock repo stats via `gh api repos/tox-dev/filelock`: license MIT, pushed_at 2026-09-22T14:38:03Z, 975 stars, 2 open issues; latest release `4.0.1` published 2026-09-19T01:08:18Z.
- filelock's Windows implementation uses `LockFileEx` on a byte range, described in its own docs as kernel-enforced (mandatory), versus the advisory `fcntl.flock()` used on Unix: https://py-filelock.readthedocs.io/en/latest/index.html
- Filelock maintainer, on what happens when a process holding a lock is killed: "if the app is killed there's little we can do" beyond relying on OS-level handle cleanup; graceful termination releases the lock cleanly: https://github.com/tox-dev/filelock/discussions/116
- Windows OS-level file locks taken via `LockFileEx`/`msvcrt` are tied to the file handle and are released by the kernel when the handle closes, including on process termination — this is the standard Win32 behavior filelock relies on for its "native, most reliable" Windows backend: https://py-filelock.readthedocs.io/en/latest/index.html
- SoftFileLock TOCTOU symlink race, CVE-2026-22701, patched in filelock 3.20.3: https://windowsforum.com/threads/toctou-race-in-python-filelock-softfilelock-cve-2026-22701-patch-3-20-3.402500/
- filelock docs recommend the maintained library approach (filelock or portalocker) over ad hoc PID files specifically because they handle stale-lock edge cases; portalocker is the drop-in alternative if filelock's Windows path ever misbehaves, also cross-platform and MIT-style licensed: https://portalocker.readthedocs.io/en/latest/portalocker.html
- MkDocs Material repo stats via `gh api repos/squidfunk/mkdocs-material`: license MIT, pushed_at 2026-09-15T15:34:06Z, 27484 stars, 1 open issue; latest release `9.7.7` published 2026-07-17T16:20:51Z.
- Project lead announced Material for MkDocs entered maintenance mode on 2025-11-05, focusing future work on a successor called Zensical; critical bug fixes and security updates continue through November 2026, after which maintenance is not guaranteed: https://squidfunk.github.io/mkdocs-material/blog/2025/11/11/insiders-now-free-for-everyone/ , https://docsio.co/blog/mkdocs-material
- As of release 9.7.0, all former Insiders-only features (instant previews, Mermaid diagrams, the rebuilt search with rich previews/tokenization, tags, admonitions, breadcrumb navigation, social cards, footnote tooltips, etc.) are included in the free MIT-licensed edition; the sponsorware/Insiders program itself is being discontinued and the Insiders repo will be deleted: https://squidfunk.github.io/mkdocs-material/blog/2025/11/11/insiders-now-free-for-everyone/

## Implications for the design
- Keep uv as the environment/package manager for the `tools/` workspace; lay it out as a uv workspace root with one member package per tool, sharing a single lockfile and venv.
- Do not rely on `--torch-backend=auto` for CUDA wheel selection in scripted/CI-style flows (`uv sync`, `uv run`) — it is preview-only and unsupported there. Pin the CUDA index explicitly per-platform in `pyproject.toml` (`tool.uv.index` with `explicit = true` + `tool.uv.sources` markers) so a Windows box with a 12 GB NVIDIA GPU always resolves the matching cu1xx wheel rather than falling back to a CPU build.
- Use filelock's default `FileLock` (OS lock), not `SoftFileLock`, for the "one build at a time" / "one game instance at a time" mutexes; the Windows kernel releases the lock automatically if a build or the game process is killed, so no manual stale-lock cleanup logic is needed. Pin filelock >=3.20.3 (well below the current 4.0.1) to pick up the SoftFileLock CVE fix even though SoftFileLock isn't the primary code path.
- MkDocs Material is fine for an internal, offline-buildable knowledge base site (`mkdocs build`/`mkdocs serve` run fully local, no network calls needed at build time beyond initial `pip`/`uv` install of the package and any Google Fonts the theme fetches, which can be disabled/self-hosted for offline use). Because the project is feature-frozen and heading toward EOL in Nov 2026, avoid building deep custom plugins against it; treat the currently-free Mermaid/search/tags feature set as the stable baseline to design against, and budget a future migration look at Zensical.

## Follow-ups
- Confirm `mkdocs.yml` config disables remote font/CDN fetches (self-host fonts) to guarantee a fully offline `mkdocs serve` on the target workstation.
- Decide the CUDA index pin (e.g. cu126 vs cu128) once the exact PyTorch version needed for the DFR/tonemap tooling is chosen, and encode it in `tools/pyproject.toml`.
- Re-check squidfunk/mkdocs-material and the Zensical project status closer to Nov 2026 EOL before renewing the knowledge-base investment.
