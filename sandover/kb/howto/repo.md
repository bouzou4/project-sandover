# Repo how-to

## Remotes
- `origin` — the `sandover` working repo. All commits, branches, issues and
  PRs go here.
- `upstream` → `https://github.com/open-goal/jak-project.git` — read-only reference.
  **Never push here, never open issues or PRs here.**

Check with:
```sh
git remote -v
```

## gh defaults
`gh repo set-default` has been pointed at `origin` inside the repo, so plain `gh issue
list`, `gh pr create`, etc. target this repo without `-R`. Verify with:
```sh
gh repo set-default --view
```
If `gh` ever prompts to pick a repo again, choose this repository (`origin`), not
`open-goal/jak-project`.

## Merging upstream changes
Upstream (`open-goal/jak-project`) keeps moving. To pull new upstream commits into
`master` here:
```sh
git fetch upstream
git merge upstream/master
# resolve conflicts (sandover/ paths are new, so conflicts should be
# limited to upstream files we've touched, e.g. .gitignore)
git push origin master
```
Prefer merge over rebase so this repository's history stays a straightforward superset of
upstream's, and `sandover/Taskfile.yml` is a separate file from upstream's `Taskfile.yml`
specifically to avoid merge conflicts there.

## GitHub Project board
A GitHub Project (v2) named "Sandover" should be linked to this repo for issue tracking
(Status/Workstream/Tier/Estimate fields). If the `gh` auth
token lacks the `project` scope, board creation is skipped; run:
```sh
gh auth refresh -s project
```
then re-run the project/board setup.

## Day-to-day
- Work happens on `master` unless a spike needs a branch/worktree (see ADR-0001 once
  written).
- No attribution or credit trailers in commit messages (the project's policy; the AI-assisted
  workflow is disclosed in the README instead).
- Game data (`iso_data/`, `decompiler_out/`, `out/`, captures) never gets committed —
  they're gitignored; keep them local or on the NAS.
