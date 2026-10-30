---
title: Per-level catalogue and day/night verdicts (Volcanic Crater, Spider Cave)
date: 2026-09-23
status: verified
sha: 64d72d103
---
## Answer (≤5 lines)
Spider Cave (maincave/darkcave/robocave), Lava Tube, Citadel and the jungle temple have no
day/night cycle: fixed slot-0 snapshot, `:sky #f` (verified). Volcanic Crater does cycle its
background palette over slots 0-3 and draws the sky (verified), but freezes the character key
light on the crater floor (verified); whether the palette change is visible is open. No wiki
or speedrun source states either way (external search, snippets only).

## Evidence (path:line@sha / URL, short excerpts)
- Catalogue table with sky flags, mood funcs and palette tables: `sandover/kb/facts/levels.md` §1
  (`goal_src/jak1/engine/level/level-info.gc@64d72d103`).
- Caves: `goal_src/jak1/engine/gfx/mood/mood.gc:1226-1303@64d72d103` — `(set! (-> arg0 times 0 w) 1.0)`
  then `(update-mood-quick arg0 0 0 0 arg2)`; no `update-mood-palette` call.
- village3: `mood.gc:1359@64d72d103` `(update-mood-palette arg0 arg1 arg2)`;
  `mood-tables.gc:3556` palette-interp = `*firecanyon-palette-interp-table*`
  (`mood-tables.gc:349-380`: hours 0-6 slot 3, 7-11 slot 1, 12-17 slots 1/2, 18-23 slots 3/2,
  unset `snapshot2` = slot 0).
- village3 light override: `mood.gc:1366-1393` constants for `mood-lights-table` 0-3;
  `mood.gc:1441-1457` lerp to a fixed dim red with direction (0,1,0), weight
  `clamp((cam.y - 184320) * 4.4389e-6)` (45 m → 100 m camera height).
- sunken is indoors with `:sky #f` yet cycles slots 0-1 (misty table, `mood-tables.gc:3378`).
- External: TV Tropes (search snippet) "several locations in the game feature a day-and-night
  cycle" (no list given); final boss "takes place just before dawn".
  <https://tvtropes.org/pmwiki/pmwiki.php/VideoGame/JakAndDaxterThePrecursorLegacy>.
  Direct wiki fetches were blocked; wiki content came from search snippets.
- Dark Cave crystals: <https://jakanddaxter.fandom.com/wiki/Explore_the_dark_cave> (snippet:
  "hitting these with a melee attack will cause them to discharge a strong green light for a
  short while, lighting the area surrounding you"). Code detail in `facts/levels.md` §4.

## Implications for the design
- An unbaked sun applies only to the palette-cycle levels. Quick levels need effect lights
  only; their slot 0 is a fixed ambient/key bake.
- Volcanic Crater: treat as a sun level for background, but keep the hardcoded character
  light when matching characters to background.
- sunken: sun slots exist but its light-group is replaced by depth-lerped colours (mood facts), so
  character/background consistency there needs special handling.
- Dark Cave: crystal pools need point lights, and AO must not darken the black base
  (`facts/levels.md` §4).

## Follow-ups
- Unbake palette rip: measure slot 0-3 colour spread in village3 to settle "visible" vs "not".
- Fill in `village2`/`swamp` palette-interp table names (left as mood facts rows).
- Runtime check of village3 camera height on the crater floor against the 45 m threshold.
