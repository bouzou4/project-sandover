# Bug reports (Jak 1)

Players save a bug report in the game and attach its zip to a GitHub issue
(`.github/ISSUE_TEMPLATE/bug_report.yml`); `sandover-bug import` turns the zip into a critic-run
folder, so triage and `sandover-capture from-snapshot` work on it like on an F9 snapshot.

## In the game

| Trigger | When the frame is taken |
|---|---|
| F7 (any build, not only debug) | the next rendered frame, menus and HUD included |
| Pause > Options > Sandover > SAVE BUG REPORT | the row closes the menu; the frame 20 frames after the last menu frame |
| `(sandover-bug-report)` over the REPL or as a rig form | the next rendered frame |

On the rig, `--goal "(set! *rig-stage-hook* (lambda () (sandover-bug-report) (none)))"` saves a
report at the shot (after `--goal "(define-extern sandover-bug-report (function none))"` when the
rig's goalc compiles another checkout). Both forms are `presets.BUG_REPORT_FORMS`.
The game must run this branch's GOAL: `--bin <bin>` boots gk from the bin's `build.json` root
(unless `SANDOVER_GK_PROJ_PATH` is set); a game without `bug-report.gc` dies on the call.

"BUG REPORT SAVED" (or "BUG REPORT FAILED, SEE THE LOG") shows at the top left for about four
seconds once the files are written; the log says `bug report saved: <zip>`. One report is written
at a time: a request while one is being written is ignored with a warning.

Code: `game/graphics/bug_report.{h,cpp}` (engine), `goal_src/jak1/pc/features/bug-report.gc`
(state and menu), `kernel-defs.gc` (`pc-bug-report-request`, `-poll`, `-save`).

## Bundle

```
<bug-reports>/<id>/screenshot.png      the frame, SDR, HUD included (no EXR even with FP16 on)
<bug-reports>/<id>/preview.png         same frame scaled down to at most 1920 wide (wider frames only)
<bug-reports>/<id>/report.json         the sidecar (below)
<bug-reports>/sandover-bug-<id>.zip    screenshot.png and report.json under sandover-bug-<id>/, stored uncompressed
```

`<id>` is the local time `YYYYMMDD-HHMMSS` (`-2`, `-3` on a clash). `<bug-reports>` is
`<run dir>/bug-reports` when `SANDOVER_RUN_DIR` is set (play sessions), else
`<gk --config-path>/bug-reports` (`%APPDATA%\Sandover\bug-reports` in an installed build,
`$SANDOVER_DATA/config/slot<n>/bug-reports` on the rig), else `<user config>/OpenGOAL/jak1/bug-reports`.

`report.json` (schema: `sandover/tools/sandover/bug_report.schema.json`) is a superset of the F9
sidecar (`critic-run.md`): the same `level`, `continue`, `jak_pos`, `cam_pos`, `cam_rows`, `fov`,
`hour`, `mood_weights` and triage fields, plus `jak_quat`, `build` (id and commit from `build.json`
next to `gk`, else the compiled revision), `system` (OS version, CPU threads, GL vendor, renderer
and version string, which carries the driver version), `display` (mode, window, game resolution,
render scale, upscaler, MSAA, AA mode, vsync, FP16, HDR requested and presenting, peak, paper
white and black nits), `settings` (the Sandover settings in effect, written by GOAL just before
the save, with the file text; `pc-settings.gc` values) and `log_tail` (last 200 log lines).

Privacy: every string is passed through `bug_report::sanitize`: the run, data, config, install,
project, AppData, temp, OneDrive and home folders become `<run>`, `<data>`, `<config>`, `<install>`,
`<project>`, `<appdata>`, `<temp>`, `<onedrive>`, `<home>`; each component of the home folder
(`D:\Profiles\Org\jdoe.CORP` gives `Profiles`, `Org`, `jdoe.CORP`) and the user, machine and domain
names become `<profile>`, `<user>`, `<host>`, `<domain>`; any other drive or UNC path becomes
`<path>/<file name>`, taken whole (to the closing quote when quoted, else to a character no path
has, so `D:\My Games\Jak ISO\x.iso` keeps none of its folders); `/home/<name>` and `/Users/<name>`
become `<user>`; `OneDrive - Org` and `SharePoint - Org` become `- <org>`. The tools mirror the
rules (`sandover.bug_report.sanitize`); `tests/test_bug_report.py` checks that the patterns are
the C++ ones and runs a sample set through them. `sandover-bug validate` fails a bundle that still
holds a drive path, a `Users\<name>` or `home/<name>` folder, an organisation folder, or this
machine's user or host name.

The render thread's share (readback, flip, report) is logged as `bug report frame: N ms`. On the rig
at 3840x2160 it was 61.4 ms, 17.8 ms of it the report (settings, sanitizer, JSON): a one-frame hitch.

## On the tools side

```sh
cd sandover/tools
uv run --no-sync python -m sandover.bug_report validate <zip-or-folder>
uv run --no-sync python -m sandover.bug_report import <zip-or-folder> [--runs-dir DIR]
uv run --no-sync python -m sandover.rig.capture from-snapshot $SANDOVER_DATA/runs/bug-<id>/0001.json
```

`import` validates the sidecar, prints a summary and every problem (missing or mistyped field,
absolute path left in), and refuses only when the level, continue, camera, Jak position or hour
is missing. It writes `$SANDOVER_DATA/runs/bug-<id>/` (`-2` if it exists; nothing is overwritten):
`0001.png`, `0001.json` (the report with `index` 1 and the triage fields) and `bundle/` (the
original `report.json`, `preview.png`, and `sandover.gc` from the reported settings). The sidecar's
`toggles` load that `sandover.gc` over the defaults and push it
(`(sandover-settings-reset! (-> *pc-settings* sandover))`,
`(sandover-settings-read (-> *pc-settings* sandover) "<run>/bundle/sandover.gc")`,
`(sandover-settings-reapply)`, `(sandover-settings-update)`), so `from-snapshot` re-shoots with
the player's settings; `--no-toggles` gives the vanilla look as usual. List values that older
builds wrote quoted (`(upscaler "bilinear")`) are unquoted in the copy, so any build reads it.
