# In-game click-to-inspect for emissive review

Date: 2026-09-24. How-to: `kb/howto/inspect.md`. Shared overrides contract: `kb/howto/materials.md`
(`emissive-overrides.csv`), also written by the viewer's inspect panel.

## Result

- Ctrl + click (or `pc-inspect-pick` from a script) reads an RGBA32UI ID target (draw record,
  provoking vertex, UV) plus the colour and emissive mask at the pixel, and resolves the draw,
  texture, `materials.csv` class, hit triangle (barycentrics in UV space, no camera ray), world
  position, texel, the sidecar E of the nearest vertex and its eight palette slots.
- A verdict button appends a row to the shared overrides file; `sandover-unbake export-sidecar`
  folds it into E, and the next load of the level shows it in the click report and in the mask.
- Inspect off matches the previous build: FLIP mean 0 on `beach-lamp-night` (bit-exact) and
  0.00003 on `village1-hut-night` (repeat noise is 0.0001), `emissive` manifest.

## Verification (rig, `sandover-capture one`, bloom on so the mask is live, hour 23)

| Click | Reported |
|---|---|
| beach-start, lamp panel | tie village1 tree 0 draw 19, `vil-lamp` (tpage village1-vis-tfrag, combo 0x018e0013, class special), texel alpha 0x40, E 0.404 from the night slots (source 9), weight 1.00, mask 0.404; slot 6 luma 158-200 at weight 1 |
| beach-start, lamp post | draw 18, `vil-lamp-wood` (wood), E 0 on most of it, 0.08-0.22 near the panel |
| village1-hut, lamp by the fence (86 m) | same `vil-lamp` draw, E 0.416, mask 0.416 |
| village1-hut, eco particle (particles on) | the surface behind: `vil1-grassclump-01` shrub or `vil1-jng-leafyground` tfrag at 322 m, sidecar E 0, while the pixel is cyan (206 255 255) with mask 0.68-1.0 |
| Jak | merc common, model `eichar-lod0`, `bam-cotton-pants` (organic) |
| sky | "nothing wrote the ID target" |

The beach lamp is village1 geometry (its tie tree), which is why its texture is `vil-lamp`, not
`bch-lamp`. Particles and other sprites do not write the ID target by design; a high mask value
over a surface whose sidecar E is 0 is the particle's own mask write (additive sprites, #150).

Override flow: marking the beach lamp panel emissive wrote
`0x018e0013,village1-vis-tfrag,vil-lamp,village1,tie/village1/0/0/19,emissive,...`; the exporter
reported `village1 ... overridden 416` (beach 0: the lamp is village1's); after installing, the same
click reported E 1.000, source still the night slots, and mask 1.000. Offline, marking `bch-lamp`
emissive and `vil-lamp-wood` not emissive changed only record word w bits 16-27 (192 vertices on
beach, 1006 on village1; the low 16 bits and the other words identical).

## Limits

- Tie wind instances and envmap second passes do not register (wind reports "a draw of shader N
  that does not register"). Draws of shaders without the snippet inside buckets 5-62 would leave
  undefined ids; none were seen.
- The hit triangle is searched among triangles ending at the provoking vertex; a hit off those
  (or with degenerate UVs) falls back to the provoking vertex and says so.
- Verdicts are per texture across all levels (the shared contract keys on `combo_id`); `draw_id`
  is context only. Marking `vil-lamp` also marks every other lamp using it, which is the intent here.
- Cost not measured (off path makes no GL calls; on: one RGBA32UI target, 236 MB at 2560x1440
  MSAA 4, and a uniform per draw).
- The rig's `one` command renders at a different internal size than the manifest runs, so the
  logged pixel coordinates differ between runs; the view fractions do not.
