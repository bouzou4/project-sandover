---
title: One table describes the sandover-settings fields; the settings file and the menu behave as before
date: 2026-10-29
issue: none (refactor: settings table)
status: verified
sha: master 666b0f333 plus this branch
---
## Answer (≤5 lines)
`*sandover-fields*` (`pckernel-impl.gc`) now holds one row per `sandover-settings` field, and the reset, boot-state, key reader and
writer loops run over it. The Sandover page binds its rows through a second table, `*sandover-menu*` (`progress-pc.gc`), and 15 of
its 20 greying lambdas became one function reading the row's parent column. The settings files are byte-identical to master's
for every case tried (74 files), in both directions of a write with one build and a read with the other; the page's rows, pointers,
list values and greyed states are identical in four settings states.

## What moved
| Before | After |
|---|---|
| `sandover-settings-reset!`, `sandover-boot-state!`: 28 `set!` lines each | one loop each (`sandover-field-set!`), the default and boot columns |
| `sandover-settings-read-key`: a `case-str` of 28 clauses | `sandover-field-find` + `sandover-field-read`, by kind |
| `sandover-settings-write`: 28 `format` lines | `sandover-field-write` per row, in table order |
| `sandover-option-pointer`, `sandover-option-list`: a `case` on the text id | a lookup in `*sandover-menu*` |
| 20 `option-disabled-func` lambdas | 15 `(sandover-row type name)` rows reading `sandover-row-disabled?`; 5 explicit lambdas (contact shadows, HDR output, HDR calibration, upscaler, render scale) |
| 7 `def-progress-carousell` lists of the Sandover page | rows of `*sandover-menu*` (the table holds the list; `sandover-option-list` returns it) |

Kept as they were, and why:
- `sandover-settings-update` (the pushes): its form order is pinned by `test_settings_forms.py`, and several fields push several forms.
- `sandover-settings-reset-vr!` is `reset!` plus one line (render scale 2); the VR default is one value, not a column.
- The audio-device row of the page (a pointer into the audio menu state, not a settings field) and the dynamic upscaler list.
- The row offsets are literals in the table: `offset-of` is not a constant in a static structure (goalc reads it as an enum name). `test_field_offsets_match_the_type` checks them against the type, in the type's order.
- The menu columns (text id, list values, parent) are a second table: `pckernel-impl.gc` loads before the text ids and carousels exist, and a static table cannot name a runtime global such as a carousel defined later.
- The greying stays a lambda per row because `option-disabled-func` is a `(function symbol)` with no argument, so a single function cannot know which row calls it; each row's lambda passes its text id.

## Evidence
Static and tests:
- `goalc -c "(mi)"`: 0 errors (549 targets on the base tree, 548 incremental on the head; the same one warning, `PC_KERNEL_VERSION` redefined, on both).
- `pytest` in `sandover/tools`: base 892 passed, 1 failed, 13 skipped; head 939 passed, the same 1 failed, 13 skipped
  (the export test, which fails on base too). The 47 new tests are the table tests below. After merging master 315fd4ef6 (which fixes that failure): 953 passed, 13 skipped, 0 failed.
- `test_settings.py` and `test_presets.py` read the table (`sandover.settings_table`) where they regexed the GOAL source, with the same expected values;
  `test_settings_forms.py` passes unchanged (`MASTER_PLAY`, `MASTER_REAPPLY`, `NEW_REAPPLY`), its evaluator now runs the table loops
  (`dotimes`, static tables, field pointers) and `format`. New there: reset and boot state against master's explicit values (with types), the writer
  against the Python mirror's text for five value sets (and the VR file's MSAA key), out-of-list values written as the old `else` names,
  the reader for 40 key and value cases (version 3 SSAO tier shift, quoted names, clamps, no-match keeps the value, the device cut at 127 bytes, unknown keys skipped).
- `goalc-test` (one binary, built from master, run against each tree): base 1715 passed, 2 failed (`WorldLightsGlow.ApproachIsSmoothAndNearestEightPops`, `BakedLightsAreNeverEvicted`, the known ones); head 1715 passed, the same 2 failed (the first head run, taken beside a rig run, also failed the game-booting tests on busy ports; solo they pass).

File round trip in gk (11 seeded cases: no file, v4 defaults with a VR file, v1, v2, v3 medium and high SSAO, a full non-default v4 pair, a VR file alone, unreadable files,
a quirks file with quoted names, out-of-range values, an unknown key with nested data and a version 5 header, another with negative nits and a 140-byte device name),
each booted on the base build and on the head build, then `(commit-to-file *pc-settings*)`:
- the written `sandover.gc`, `sandover-vr.gc` and `pc-settings.gc`: 74 files, byte-identical between base and head;
- the in-game state dump of every field of both profiles after load: identical;
- in one boot, 13 files read with `sandover-settings-read` after a reset and written with and without the MSAA key, plus the reset of both profiles written: identical;
- cross: the head build reading the files the base wrote, and the base reading the head's: states and the 33 re-written files identical.

Menu walk (village1-hut, the Sandover page up, four settings states: defaults, vanilla rendering, the three parent switches off, a mixed state):
- every row of `*sandover-options*`, the calibration and audio pages: text id, option type, the `value-to-modify` offset in `sandover-settings`, and the greyed result of its function
  (160 lines per build), plus the list values of every row: identical between base and head, and between two base runs. (The two calibration buttons without a field print the pointer 0 relative to the settings address, which differs with the heap layout.)
- screenshots of the page (top and scrolled) in the four states: two base runs of the same build already differ over most of the frame (the page animates), and head is inside that spread in every state; the dump above is the exact proof.

Pixels (2560x1440, `--parallel 1`; the `auto-v0` and `tonemap-hdr` runs pin the wave clock with `(pc-set-water 90 100000000)`; base = master 666b0f333 build, head = this branch; base twice for the floor):
| Set | Result |
|---|---|
| `auto-v0`, no play look (16 shots) | the 8 beach and village shots are exactly 0 on all three runs; jungle and sunken have floors of 0.007 to 0.13 mean abs and head stays inside them (0 shots above the floor) |
| `auto-v0 --play-look` (16 shots) | play look is not repeatable run to run; 13 of 16 shots at or under the floor; the 3 above (beach dusk and night, jungle night) are inside the spread of same-build pairs: three more runs of those shots gave base vs base 0 / 0 / 80 % of pixels, head vs head 0 / 53 / 76 %, base vs head 1.4 / 53-59 / 75-80 % |
| `tonemap-hdr` with effects and particles off (4 shots, PNG and EXR) | head equals base1 exactly on all four (EXR max abs 0); effects-off is not fully deterministic between base runs (sunken 0.91 %, village1 0.003 %) |
| `tonemap-hdr` as shipped (effects on, 4 shots) | head differs by 1.2 to 2 % of pixels (stars and eco lightning); an identical build differs from itself by the same amount (base code in the head's staging, two runs: 1.1 to 1.8 %), so it is run-to-run noise |
| vanilla path, `auto-v1` set (56 shots, forced to shoot, base twice, head once) | 34 shots are exactly 0 on every run; the other 22 have non-zero floors (0.02 to 2.6 % of pixels) and head is at them; four shots sit up to 0.07 points above the floor (jungle-dusk 0.139 % vs 0.067 %), at the level of the code-size-padded base control (0.162 %) (citadel dusk, jungle dusk, night and noon) |
| the same vanilla set on base code with head's code size (a static array padded to the same ENGINE.CGO size) | the same per-shot magnitudes as head, so a shift in object layout alone moves these shots as much as the refactor does |

`sandover-parity check --set auto-v1` reused the vanilla frames of earlier runs for both builds when given `--build <dir>` (its capture does not pass `--bin`), so its 49 of 56 against the old golden (the same 7 shots) is not evidence; the forced captures above are.

## Implications for the design
A new field is a deftype line, a row in `*sandover-fields*` at its place in the file, the push in `sandover-settings-update`, and for a menu row a text id and a row in `*sandover-menu*` (`settings-system.md`, "The settings table").
Anything that adds code or static data moves the heap layout, and the rig shows that as a few hundredths of a percent on animated shots: a bit-exact gate on animated shots needs deterministic effects, not only a pinned water clock.

## Follow-ups
- A deterministic play-look mode (also in the bridge-split note) would make the animated shots exact.
- `sandover-parity check` should pass `--bin` to its capture so a directory build is shot, not reused.
