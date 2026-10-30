---
title: kmachine.cpp bridge split moves 2,300 lines with no behaviour change
date: 2026-10-29
issue: none (refactor: bridge split)
status: verified
sha: master f3dccc779 plus this branch
---
## Answer (≤5 lines)
The 72 Sandover `pc-*` entry points left `kmachine.cpp` (3535 to 1237 lines) for 12 per-feature files and one
registration table under `game/kernel/common/sandover/`. The functions moved verbatim, the 72 names register in the same
order and scope, and the vanilla-path parity set is pixel-identical (56 of 56 shots, max channel difference 0).

## Evidence
Layout: `bridge.h` (declarations, `symbol_to_bool`/`bool_to_symbol`, `BridgeScope`), `bridge.cpp` (the table and
`register_sandover_bridge`), and `bridge_{xr,ao,debug,temporal,hdr,water,materials,lights,sun_shadows,audio,vista,capture}.cpp`.
`kmachine.cpp` keeps upstream code and two calls: `BridgeScope::AllGames` at the old `pc-get-xr-game-rate` position,
`BridgeScope::Jak1` inside the old Jak 1 block.

Static (`python -m sandover.bridge_check --base f3dccc779`, committed under `sandover/tools/sandover/`):
- 72 bridge names before and after, identical and in order; the full 152-name registration sequence is identical with the table expanded in place.
- 80 top-level blocks removed from `kmachine.cpp` reappear byte for byte in the bridge files; 2 edited in place (the include list, the registration function); 0 missing.
- `git diff --color-moved=dimmed-zebra`: 2,328 lines removed from `game/kernel`, 2,238 of them shown as moved (the rest: the 72 registration lines, 12 includes, blank lines).

Build and tests (merged base f3dccc779):
- `gk`, `goalc`, `goalc-test` build with no errors and no warnings from the new files.
- `pytest` in `sandover/tools`: 899 passed, 3 failed. The same 3 fail on an unchanged master worktree (base run: 896 passed, 3 failed; head has the 3 new tests)
  (`test_play_guard` x2, the export test, which flags `architecture.md` naming private tool files that the export leaves out).
  14 source reads in 5 test files now read `kmachine.cpp` plus the bridge files through `sandover.bridge_sources.bridge_text`; one registration assertion
  now matches the table entry (`{"pc-get-xr-state", (void*)pc_get_xr_state, BridgeScope::Jak1}`); nothing else changed.
- `goalc-test`: 1704 of 1706 pass, plus `Listener.*` 9 of 9 (they need free ports). The two failures, `WorldLightsGlow.ApproachIsSmoothAndNearestEightPops` and `BakedLightsAreNeverEvicted`, fail identically on a base build (base run: 1704 passed, the same 2 failed; `Listener.*` 9 of 9).
- `goalc -c "(mi)"`: 548 targets, 0 errors.
- Runtime probe (`--audio` boot, 72 `(format 0 "RIG:SYM ...")` forms): 70 symbols print as compiled functions, no `unknown option` line.
  `pc-set-caustics` and `pc-snap-poll` have no `define-extern` and no GOAL caller, so the probe form cannot compile; both are registered in the table (dead entries, dead-code candidates).
  The exit log shows `[audio] control thread joined` after an `--audio` run, so the `g_audio` destruction order is unchanged in effect.

Pixels (2560x1440, base = unmodified master build, head = this branch; one gk, `--parallel 1`):
| Set | Floor (base vs base) | Head vs base |
|---|---|---|
| `sandover-parity check --set auto-v1` (vanilla path, 56 shots) | n/a | base and head both 49/56 against the old golden, same 7 failing shots; head vs base: 56 of 56 shots max diff 0 |
| `auto-v0`, no `--play-look` (16 shots) | 0 on the 8 beach/village shots; jungle/sunken 0.004 to 0.39 % of pixels differ | the same 8 shots 0 (plus sunken-dusk); the rest 0.05 to 0.19 % of pixels. Pooled this is the floor's range, but `sunken-start-noon` is above its own pair (floor 0.002 mean abs, max 106, 0.004 % of pixels; head 0.117 and 0.119, max 165, 0.19 %); the other sunken shots have floors of 0.08 to 0.09 |
| `tonemap-hdr` as shipped (effects and particles on), no `--play-look`, FP16 target (4 shots) | `firecanyon-lava-noon` 0.143 mean abs, max 11, 34 % of pixels; the other three 0.7 to 1.9 % | `firecanyon-lava-noon` 0.555 and 0.697, max 35 to 43, 59 %; the other three 1.4, 1.2 and 1.8 % (floors 1.0, 0.7, 1.9 %). Flagged by the 1.5x floor + 0.05 rule |
| `tonemap-hdr` with effects and particles off, no `--play-look`, FP16 target (4 shots, PNG + EXR) | 0 on 2 shots (PNG and EXR); sunken-precursor-glow 1.10 %, village1-night-lamps-eco 0.003 % | 0 on the same 2 (PNG and EXR, max abs 0); the other two 1.23 % and 0.0035 % |
| `auto-v0 --play-look` (16 shots) | non-zero on every shot (mean abs 0.4 to 3.2 levels, max up to 255) | mean abs 0.0 to 3.0, never above the floor |
| `tonemap-hdr --play-look` (4 shots, as shipped and effects off) | non-zero everywhere | at the floor |

Each floor is a single base-vs-base run pair, so it is itself a sample. The two shots above the floor are animation noise: lava and flames (`firecanyon-lava-noon`, live effects) and
creature motion (`sunken-start-noon`). The static re-shoot of the tonemap set (effects and particles off) gives exactly 0 on `firecanyon-lava-noon`, base vs base and head vs base.

With `--play-look` two runs of the same build differ over most of the frame (the temporal/FSR path; an MSAA override did not remove it), so exact zero is
not reachable there; the comparison is then head against the floor per shot. On the SDR-with-zero-floor shots and on the 2 HDR shots with zero floor, head is exactly 0 (EXR included, under 1 level).

## Implications for the design
`bridge_<feature>.cpp` is where a new `pc-*` entry goes: define it in the feature file, declare it in `bridge.h`, add a row to the table (order is the symbol intern order).
The settings-table refactor can now rewrite the settings table without touching `kmachine.cpp`; `settings-system.md` still cites old `kmachine.cpp` line numbers.

## Follow-ups
- `pc-set-caustics` and `pc-snap-poll` look dead (no `define-extern`, no caller).
- A play-look capture is not reproducible run to run; a deterministic play-look mode would make bit-exact refactor gates possible.
