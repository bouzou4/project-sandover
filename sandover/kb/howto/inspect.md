# Inspect how-to (Jak 1): click a pixel, see what drew it, mark it emissive

A debug tool for emissive detection and material review. Off by default; Jak 1 only. Code:
`game/graphics/opengl_renderer/InspectRenderer.{h,cpp}`, shader snippets
`shaders/inspect_{vert,frag}.glsl`. Exporter side: `sandover/tools/sandover/unbake/overrides.py`.
Results of the first run: `kb/findings/2026-09-24-inspect-tool.md`.

## Turn it on

- ImGui: Renderer Debug → **Inspect**: the switch, a button that re-reads `materials.csv` and
  the overrides file, and the number of draws registered last frame.
- GOAL: `(pc-set-inspect 1)` / `(pc-set-inspect 0)`.
- **Ctrl + left click** on the game view (not on an ImGui window). The **Inspect** window
  shows the result. The same summary goes to the game log as one `inspect:` line per click.
- Scripted: `(pc-inspect-pick x y verdict)`, `x`/`y` in millionths of the view from the top
  left, verdict 0 none, 1 emissive, 2 not emissive, 3 clear. Picks queue (up to 16) and are
  served one per frame. With the rig, set it as the stage hook so it runs after the
  settle frames:
  `--goal "(pc-set-inspect 1)" --goal "(set! *rig-stage-hook* (lambda () (pc-inspect-pick 500000 400000 0) (none)))"`.
  A goalc built from an older `kernel-defs.gc` needs
  `(define-extern pc-set-inspect (function int none))` and
  `(define-extern pc-inspect-pick (function int int int none))` first.

## What a click reports

| Field | Source |
|---|---|
| pixel, colour | the render framebuffer at the click (after SSAO, bloom and sprites) |
| emissive mask | the R8 mask at the pixel, or "off" when neither the SSAO mask nor bloom runs |
| draw | renderer (tfrag, tie, shrub, merc, generic), level, geometry LOD, fr3 tree, draw index; merc also the model name |
| texture | name, tpage, `combo_id` (`tpage << 16 \| index`), size; generic: the pool name for its tbp |
| material class | `materials.csv` row for the `combo_id` (class and tags) |
| UV, texel | the interpolated texture coordinate at the pixel, the texel it lands on (wrap or clamp as the draw sets it) and that texel's RGBA from the level's texture data |
| triangle | tfrag and tie: the triangle ending at the provoking vertex whose UV triangle contains the hit UV, with barycentrics; world position (m) and distance from the camera; nearest vertex = largest barycentric |
| sidecar E | tfrag and tie at LOD 0: the nearest vertex's stored E, its source (palette slot 0-7, static, night slots), the source's runtime weight now, and their product |
| slot table | the nearest vertex's eight palette colours, luminance, role (from `unbake/levels.py`, plus "night"/"emitter" from the sidecar masks), current weight, and luminance × weight |

The hit triangle is found without a camera ray: the ID target carries the provoking vertex id
and the UV, and the CPU searches that vertex's triangles in the draw's index data. If no UV
triangle contains the hit (degenerate UVs, or the hit is off the provoking vertex's
triangles), the panel says so and reports the provoking vertex instead.

What does not register: tie wind instances and envmap second passes (they keep the base
draw's id; wind draws report "a draw of shader N that does not register"), and everything
outside buckets 5-62 or in the shadow bucket: sky, ocean, sprites and particles, HUD. A click
on a particle therefore reports the surface behind it; the emissive mask value at that pixel
is the particle's (additive sprites write the mask), so a high mask value on a surface with a
zero sidecar E is the sign of a particle in front.

## ID target

`GL_RGBA32UI` at colour attachment 3 of the render framebuffer, with the frame's sample
count (about 236 MB at 2560×1440 MSAA 4), attached and cleared before bucket 0 of each
dispatch while the tool is on. Buckets 5-46 and 48-62 draw with it as draw buffer 3; every
other bucket has it set to `GL_NONE`. Off: detached once, and no GL call is made afterwards.
Stereo turns it off.

- `x`: draw record, the index + 1 into this frame's draw table (0 nothing; `0xff000000 |
  shader id` a draw of that program that did not register).
- `y`: provoking vertex id (`gl_VertexID` of the last vertex of the triangle).
- `z`, `w`: the texture coordinate as float bits.

The draw table keeps, per draw, what the lightbench texture-id buffer encodes (renderer, level,
texture index) plus the tree, draw index, draw mode, the frame's time-of-day weights and the
camera position. Readback is a 1×1 blit per attachment (resolving MSAA) and a 1×1
`glReadPixels` of each, only on a click.

## Overrides file

`$SANDOVER_DATA/materials/emissive-overrides.csv` (or `$SANDOVER_EMISSIVE_OVERRIDES`, for test
runs), append-only, one row per button press. The columns and key rules are the contract shared
with the viewer's inspect panel and the reviewer (`sandover.materials.overrides`,
`kb/howto/materials.md`):

```
combo_id,tpage,name,level,draw_id,verdict,note,timestamp
0x00d40025,beach-vis-tfrag,bch-lamp,beach,tfrag/beach/0/0/41,emissive,lantern panel,2026-09-24T02:10:00+00:00
```

- Buttons: **mark emissive**, **mark not emissive**, **clear** (an empty `verdict`), with an
  optional note. The panel shows the verdict in force for the texture.
- A verdict is per texture: the key is `combo_id` (or `name` when `combo_id` is blank) and the
  last row per key wins. `draw_id` (`<kind>/<level>/<geom>/<tree>/<draw>`) records where the
  click landed; it is context only and never narrows a verdict, because every reader keys on
  `combo_id`.
- The exporter applies a verdict to every vertex drawn with a texture of that name in every
  level (the glb names materials by texture name).

## Fold the verdicts into E

```sh
cd sandover/tools
uv run sandover-unbake export-sidecar beach village1 --install      # reads the overrides file
uv run sandover-unbake export-sidecar beach --no-overrides          # the computed E only
uv run sandover-unbake export-sidecar beach --overrides other.csv
```

`emissive` sets E to 1 and keeps the vertex's E source if it had one (a lamp baked into the
night slots stays night-weighted, so it does not glow at noon); a vertex without E gets the
static source (weight 1 at every hour). `not-emissive` sets E and its source to 0. Verdicts
apply after the reviewer's per-texture strengths (`emissive-strengths.csv`), so a verdict wins. Nothing else in the record, the header or the
version changes; the report JSON gains `emissive.overridden` (vertices touched) and the
histogram counts the final E. `--install` copies into `out/jak1/fr3` of
`OPENGOAL_PROJ_PATH`; a running game picks the sidecar up on the next level load.
