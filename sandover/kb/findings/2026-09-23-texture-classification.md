---
title: Full material classification of the Jak 1 texture rip (six classes)
date: 2026-09-23
issue: none
status: inferred
sha: d2b37b5
---

## Answer (≤5 lines)
All 4002 ripped textures carry one of the six classes in `$SANDOVER_DATA/materials/materials.csv`
(the class schema plus `judge_agreement`). Two independent vision-judge passes agree on 3720/4002
(93.0%). 737 rows are ambiguous (confidence < 0.5) and 1686 are in the FiftyOne
`needs-review` view (disagreement or confidence < 0.6). Agreement between the judges
overstates accuracy: against the texture census pilot, itself an earlier vision pass and not human ground
truth, the labels match 51 of 81 comparable rows (63%). That is judge-vs-judge agreement only.

## Evidence (path:line@sha / URL, short excerpts)
- Tooling: `sandover/tools/sandover/materials/` (`sandover-materials sheets | prompt | status |
  merge | load | spotcheck | apply | summary`); runbook `kb/howto/materials.md`.
- Protocol: 201 contact sheets of 20 textures (ordered by tpage, then name), each cell
  captioned index, size, tpage and name; alpha shown at twice the PS2 value over a checkerboard,
  plus an opaque colour inset for translucent textures (tfrag textures with low alpha looked
  empty otherwise); tiny textures upscaled nearest-neighbour. 40 judge sessions: pass `a` in 20
  batches of about 10 sheets, pass `b` shifted by 5 sheets, so no sheet shares both judges
  (`tests/test_materials.py`). Merge: agreement averages confidence; disagreement keeps the more
  confident class at half its confidence.
- Distribution (`sandover-materials summary`):

| class | textures | share | mean confidence |
|---|---|---|---|
| stone | 319 | 8.0% | 0.62 |
| wood | 451 | 11.3% | 0.66 |
| metal | 1028 | 25.7% | 0.59 |
| foliage | 193 | 4.8% | 0.70 |
| organic | 1225 | 30.6% | 0.57 |
| special | 786 | 19.6% | 0.67 |

- Top judge confusions: organic/special 52, metal/special 47, metal/stone 41,
  metal/organic 27, special/stone 27, metal/wood 23, organic/wood 22.
- By draw kind: `*-vis-pris` 1809 textures, 1129 organic and 395 metal; `*-vis-tfrag` 1446,
  570 metal, 275 wood, 247 special, 243 stone; `*-vis-shrub` 242, 119 foliage; `*-vis-alpha`
  233, 227 special; `*-vis-water` 17, all special.
- Pilot cross-check (`$SANDOVER_DATA/census/pilot/labels.csv`, 100 textures labelled by the
  earlier vision pass in the texture census against the 5 draft classes, not by a human): 19 pilot "other/special" rows are now `organic`, as the sixth
  class intends. Of the other 81, 51 match; 45 of 62 match where the pilot confidence was
  ≥ 0.5. The main split is pilot `stone` → judge `metal` (11), mostly precursor surfaces
  (citadel, finalboss, lavatube, sunken, jungleb, training); three of them are named
  `*-precursor-metal-*`. Precursor surfaces are bronze-like metal, so
  `metal` is correct for them and the pilot `stone` labels are wrong. The 63% is agreement
  between two vision passes, not an accuracy figure.
- `envmap_swatch` (at most 16 px, channel std < 12) flags 284 rows, but 174 of those are flat
  skin tones judged `organic`: the heuristic finds "tiny and flat", not "envmapped".
- Spot-check set: `$SANDOVER_DATA/materials/spotcheck/spotcheck.csv` plus `sheet-001..005.png`,
  100 textures drawn round-robin over the six classes and three confidence bands (< 0.6,
  0.6–0.85, ≥ 0.85): 16–17 per class; 36 low, 34 mid and 30 high; 6 disagreements.
- FiftyOne `jak1-textures` now has `material` (Classification), `material_class`,
  `material_confidence`, `judge_agreement`, `judge_a`, `judge_b`, `envmap_swatch`,
  `material_reviewed`, `material_note` and `sheet_index`, plus the saved view `needs-review`.
  `dataset.count()` returns 0 on this database although iteration yields 4002 (stale collection
  metadata); counts with a field (`count("filepath")`) are correct.

## Implications for the design
- The sixth class does real work: `organic` is the largest class (31%), almost all from `pris`
  pages. Leaving it in `special` would have dropped detail from a third of the rip.
- Precursor surfaces (rust-coloured, carved, named metal or stone) are bronze-like metal, as
  ruled; they stay `metal`, with no separate stone label.
- The 93% agreement comes from two sessions of the same judge on the same sheets, so their
  errors correlate. Treat the spot-check, not the agreement rate, as the accuracy estimate.

## Follow-ups
- Human spot-check of the 100-texture set, then `sandover-materials apply --csv …`; the
  measured error rate decides whether the `needs-review` view needs a full human pass.
- Replace the `envmap_swatch` heuristic with the real signal (the draw's envmap flag from
  the tie/merc data), or restrict it to non-`organic` rows.
- Optional `precursor` tag column in `materials.csv` so bronze-like precursor metal can get its
  own tuning later; re-check any remaining precursor rows labelled `stone`.
- Judge the ambiguous rows again on in-game renders once level membership (`levels`) exists.
