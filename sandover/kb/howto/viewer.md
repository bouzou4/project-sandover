# 3D review viewer how-to

Local three.js page for checking any ripped glTF model or level chunk: pick a model from a
searchable catalogue, orbit/zoom/pan, swap texture sets, toggle unlit / baked-lighting /
PBR preview, drag a point light in the viewport, save screenshots and flag render bugs.
ADR-0015.

## Run

```sh
cd sandover/tools
uv run sandover-viewer \
  --textures vanilla=<decompiler_out/jak1/textures> pack=<texture_replacements dir> \
  [--models <rip root>] [--model <path/to/model.glb>] \
  [--port 8765] [--out <screenshot dir>] [--reports <report dir>] [--lighting <sidecar dir>]
  [--emissive-maps <emissive-maps dir>] [--pbr-maps <pbr-maps dir>]
```

`--lighting` (default `$SANDOVER_DATA/lighting`) is the inspect panel's per-level `.unbake`
sidecars for its E lookup (below); `$SANDOVER_DATA/materials/materials.csv` and
`emissive-overrides.csv` are always read/written from their default location, no flag.

Opens a browser tab at `http://127.0.0.1:8765/`. `--textures` takes one or more
`name=dir` pairs; the first name on the command line is the default selection (the first
version sorted the names, so `normalized` won over `vanilla`). `--models` (default
`$SANDOVER_DATA/unbake/riproot/decompiler_out/jak1/levels`, 897 glbs) is indexed at
start-up by reading only each glb's JSON chunk (level, file, size, skinned, primitive
count, kind), served as `/models.json`. `--model` only picks what opens first and may live
outside the root.

Screenshots save to `--out` (default `$SANDOVER_DATA/viewer/`) under a server-generated
timestamp name; the upload must be a PNG (64 MiB cap) and never overwrites a file.

`--host` accepts IPv4 loopback only. Requests whose `Host`/`Origin` is not this server are
refused (403), so another site open in the same browser cannot post screenshots or reports
or read files through DNS rebinding. Every file route goes through a resolve-and-contain
check (`/models/` also requires `.glb`, `/reports/` only serves `shot.png`).

Everything is served from one local process: the static page, the models, and every
texture-set directory. No CDN, no external network access at runtime.

## Page

One left panel, five collapsible groups:

- **Model**: type-to-filter combobox over the catalogue (space-separated terms must all
  match the `level/file` id; arrow keys + Enter, Esc closes), grouped by level, each entry
  with its primitive count and a `skinned` / `prop` / `background` badge, plus a kind
  filter. Switching disposes the old scene's geometry, materials, textures and skeletons
  and loads in place, no server restart. The last model is remembered in `localStorage` and
  wins over `--model` on the next visit. Below: pose (bind pose or any clip in the file),
  model info (meshes, primitives, triangles, envmapped primitives, eye placeholders) and
  **Reset view** (`R`).
- **Textures**: texture set; lists the texture names the set lacks (the embedded copy is
  used for those).
- **Shading**: mode, alpha convention, normals (file/smooth/flat), envmap pass on/off and
  strength, stand-in eyes, wireframe, axes, background.
- **Light** (PBR preview only): the light is a small glowing sprite with a
  `TransformControls` translate gizmo; drag an arrow or plane handle in the viewport. The
  X/Y/Z fields are editable readouts, see "Coordinates".
- **Capture**: **Screenshot** (`P`) and **Flag render bug** (`B`). Flagging opens a note
  dialog, then posts the canvas PNG plus a JSON record (model id, texture set, shading mode,
  alpha mode, camera position/target/fov, light position/intensity/colour, viewport, note)
  to `/report`. The server writes `shot.png` and `report.json` (with a server timestamp)
  into `--reports` (default `$SANDOVER_DATA/viewer/reports/<yyyymmdd-hhmmss>/`); only
  those keys are kept and the note is capped at 4000 characters. The panel lists existing
  reports, newest first, each linking to its PNG. The lead sweeps that directory.

## Coordinates

The rip keeps the game's axes and divides positions by 4096 (`fr3_to_gltf.cpp`
`make_position_buffer_accessor`, `unscale_translation`), so viewer metres x 4096 = game
units and the axes are the game's: **Y up**, and a character faces **+Z** (checked on
`mayor-lod0.glb`: his face is on the +Z side, and the rig camera placed on the actor's +Z
side sees his face in game). **X** is then the character's own left, which is screen right
when you look at its front. The light readouts are metres relative to the model's fitted
centre; the line under them is the absolute model-space position in game units. The axes
helper at the model's feet draws +X red, +Y green, +Z blue, the gizmo's colours. A skinned
model is in its own model space, not level space: add the actor's `trans` (entity JSON,
metres) for world metres.

## Texture name mapping

The rip does not store a texture's tpage in the glTF; each texture/image `name` is just
the PNG stem (e.g. `mayor-flesh`), and the same stem can live under different tpage
directories in different levels. The viewer resolves this by indexing a whole
`--textures` directory recursively at startup: `{stem: relative/path.png}`, first match
wins. Swapping the texture set looks each material's texture up by that `name` in the
newly selected directory's index; a miss falls back to the model's own embedded texture
(glTF images are inlined PNGs) and is listed under Textures.

Stems collide across tpages, and not rarely: the esrgan v1.0.1 `texture_replacements`
tree has 535 stems under more than one directory, 90 of them with different pixels (e.g.
`bab-fur`, `bab-eye`). A colliding stem resolves to whichever path sorts first, so a
review can show another level's variant. The server prints a `WARNING` with the count and
the first ten collisions for each `--textures` set at startup. When the reviewed model's
textures are in that list, pass a `--textures` root scoped to the model's own level
directory.

## Shading modes

- **Unlit texture**: `MeshBasicMaterial`, texture only.
- **Baked**: the engine's `color = vertexColor * texture * 2` (`kb/facts/renderer.md`
  #3/#6), `vertexColors` plus an `onBeforeCompile` hook doubling `diffuseColor.rgb`. The rip
  writes `COLOR_0` as `byte / 255`, so 128 -> 0.502 -> x2 = 1.0. For merc (characters) the
  engine's vertex colour is `rgba * light_color`, lit per frame by three directional
  lights (`merc2.vert`); the rip's `COLOR_0` is only the `rgba` part, so baked mode on a
  character shows the unlit vertex tint, not the in-game lighting.
- **PBR preview**: `MeshStandardMaterial` under the draggable point light. An
  approximation for fast iteration, not the engine's lighting (ADR-0015).

### Derived maps (PBR preview)

`--pbr-maps` (default `$SANDOVER_DATA/materials/pbr-maps`) is `sandover-materials derive-maps`
output (`kb/howto/materials.md` "PBR maps"). When its `index.json` exists, the texture-set
dropdown gains one `<set> + derived maps` entry per set: that set's albedo plus the texture's
derived normal and roughness maps and its metallic scalar, so switching between `pack` and
`pack + derived maps` is the albedo-only vs albedo+maps comparison. Picking a maps entry
switches to PBR preview; the maps are ignored in unlit and baked. The **Derived maps** checkbox
under Shading toggles the same thing, and its slider scales the normal map (`normalScale`,
0-3, applied on release). Metallic textures get a neutral sky/ground gradient as `envMap`
(metal has no diffuse, so the point light alone would leave it black); non-metals get none,
so the albedo-only and maps views are lit the same way. The status bar reports how many
primitives picked up maps.

Routes: `/api/pbr-index` (stem -> tpage, metallic, recipe, flat; first tpage wins on a stem
collision, like the texture-set index) and `/api/pbr-map/<stem>/<normal|rough>`. Normal maps
are glTF convention; the rip has no tangents, so three.js uses derivative tangents and the
page negates `normalScale.y`, as `GLTFLoader` does. `?set=<set>+maps&mode=pbr` in the URL
opens straight into the comparison.

Unlit and baked run in gamma space like the engine: textures sampled as raw bytes
(`NoColorSpace`), output with `LinearSRGBColorSpace`. PBR keeps the sRGB pipeline.

## Normals

Only merc (`MercVertex`) carries a real per-vertex `NORMAL`; tfrag/tie/shrub background
chunks never do (`kb/facts/renderer.md` #5, confirmed against `fr3_to_gltf.cpp`'s
`add_tfrag`/`add_tie`/`add_shrub`, none of which write `NORMAL`, vs. `add_merc` which
does). Loaded as-is, a primitive with no `NORMAL` gets three.js's automatic `flatShading`
fallback (`GLTFLoader`/`WebGLProgram`: flat when `geometry.attributes.normal ===
undefined`) — faceted, hard-edged lighting on every background chunk, visible as hard
edges on low-poly terrain and props in PBR preview and in the envmap pass (both read the
vertex normal; unlit/baked don't).

The **Normals** selector (Shading group) controls what the viewer feeds those two passes:

- **File (else smooth)** (default): use the file's `NORMAL` when present (merc); when
  absent, compute smooth per-vertex normals and use those instead.
- **Smooth**: always the computed smooth normal, even where a file normal exists.
- **Flat**: always faceted (`material.flatShading = true`), even where a file normal
  exists.

The computed smooth normal merges a position-only proxy of the geometry
(`BufferGeometryUtils.mergeVertices`, so UV/color seams don't block the merge) and runs
`computeVertexNormals` on that, then scatters the result back onto the original per-vertex
layout — so the original index buffer, UVs and vertex colors are untouched, only `NORMAL`
changes. Computed once per geometry and cached; recomputed only if the model reloads.

`$SANDOVER_DATA/viewer/compare/smooth-shading.png`: two-up, PBR preview, engine alpha.
Top row `mayor-lod0.glb` (has file `NORMAL`, unaffected by this fix, before == after).
Bottom row `beach-background.glb` (tfrag/tie/shrub, no `NORMAL`): faceted cliff rock
before, smooth after.

## Alpha: the engine convention, and why the mayor lost his hair and vest

PS2 texture alpha runs 0-128, 128 meaning opaque. The rip's PNGs keep those raw values
(`mayor-shirt.png`, `mayor-pants.png`: every texel 128). The merc shader
(`game/graphics/opengl_renderer/shaders/merc2.frag:24,28,46@6a17d2634`):

```glsl
color = vtx_color * T0 * 2;   // rgb and alpha
color.a *= 2;
if (ignore_alpha == 0 && color.w < 0.128) discard;
```

With the PS2 vertex/light alpha at 0x80 (0.5) the effective alpha is `T0.a * 2`: 128 ->
1.0, and texels with raw alpha below ~16/255 (0.064 before doubling) are discarded. The
viewer's **Engine** alpha mode does exactly that in all three shading modes: after
three.js's `color_fragment`, `diffuseColor.a = min(texture.a * 2, 1)` and `alphaTest =
0.128`. Materials the exporter marks `BLEND` (draw mode `ab` on) blend without depth
write; the rest are alpha-tested. The vertex/light alpha is assumed to be 0.5: the rip's
`COLOR_0` alpha is 1.0 on every mayor vertex, so it cannot be read from the file. The
exporter's glTF `alphaCutoff` (`0x26/255`, `fr3_to_gltf.cpp:726`) is the background
renderers' cutoff, not merc's; the viewer ignores it.

**Envmapped effects ignore alpha.** For an effect with an envmap, GOAL sets the effect's
`ignore-alpha` (`goal_src/jak1/engine/gfx/foreground/bones.gc:1327,1337`), Merc2 turns that
into `IGNORE_ALPHA` (`Merc2.cpp:1114`), and the base pass is drawn with no alpha test: its
texture alpha is not coverage but the envmap strength for the second (emerc) pass. The
exporter records this: such a material is named `<tex>-envmap`, has `alphaMode` `OPAQUE`
unless its draw mode blends (`fr3_to_gltf.cpp:733`), carries the envmap texture as
`KHR_materials_specular.specularColorTexture`, and `alpha x 2` in the metallic channel
(`:669`). The viewer treats every material with a `specularColorMap` and no blending as
opaque.

The first viewer applied `alphaTest 0.1` to the raw alpha of every material. That drops
nothing on the 128-alpha cloth, but on the envmapped parts, whose alpha is a strength mask,
it cuts most texels: `mayor-vestbutton` (the whole vest front, 82 % of texels alpha 0),
`mayor-hair-01..03` and `mayor-haircurl` (alpha 0-89). Result: vest gone, hair and mustache
shredded to threads. Proof by toggling only the alpha mode (same camera and pose, envmap
pass and stand-in eyes off): 130 k of 1.44 M pixels change and vest and hair come back
(`$SANDOVER_DATA/viewer/compare/mayor-viewer-legacy-alpha.png` vs
`mayor-viewer-engine-alpha-only.png`). The **Old viewer** alpha option keeps the old rule
for comparison. Other characters with envmapped clothing had the same problem
(`fisher-lod0`: `fman-vest`, `fman-beard-*`; `explorer-lod0`: `explorer-spats`;
`sage-lod0`: `staffrings`).

`mayor-lod0.glb` for reference: 1 mesh, 16 primitives, 3013 triangles, 16 materials in use
(`mat 0`, the untextured default, is unused), 7 envmapped (`mayor-hair-01/02/03`,
`mayor-haircurl`, `programmer_eye_left/right`, `mayor-vestbutton`), 9 `MASK`, no `BLEND`,
no primitive or material `extras`, nothing hidden, 21 blend-shape targets per primitive,
one clip `mayor-idle`. Every primitive the file contains was already loaded; nothing was
missing from the file.

### Envmap pass

When a material has an envmap texture, the viewer adds a second mesh sharing the geometry,
skeleton and morph weights, drawn additively after the base pass: a `MeshMatcapMaterial`
(envmap looked up by view-space normal) scaled by `min(base alpha x 2, 1)` and the
**Envmap** strength slider (default 0.5). The engine's emerc pass computes its own
reflection coordinates (`emerc.vert`) and tints by a per-effect fade colour GOAL derives
from the sun's env colour at runtime (`bones.gc:1316-1323`); neither is in the rip, so the
result shows where the shine goes, not its exact colour.

### Eyes

Merc draws with an `eye_id` bind a texture the eye renderer draws at runtime
(`Merc2.cpp:1109`: `0xefffff00 | eye_id`), composed from the character's eyeball, iris and
lid textures, with blinking. The rip only has the 4x4 grey `programmer_eye_left/right`
placeholder there, hence the grey smudge. With **Stand-in eyes** on (default) the viewer
draws a white eyeball with a centred dark pupil on those primitives; a stand-in, not the
character's iris or eyelids.

## Pose and root motion

The page defaults to the model's first clip, holding the `main` joint's horizontal
translation at the clip's first frame: ripped clips move `main` (under `prejoint`) by
metres (`mayor-idle` walks him 3.5 m along +Z and back) and the game keeps the actor in
place. If the first clip puts parts far outside the bind-pose bounds (`fisher-lod0`: fish
props parked 17 m below the floor), the page starts in bind pose instead.

## Rip layout: every merc glb carries the whole level's vertex pool

`fr3_to_gltf.cpp` `add_merc` writes `level.merc_data.vertices`, the level-wide merc vertex
pool, into every per-model glb, and only the index buffer selects the model's own
vertices. So all skinned glbs from one level directory share the same `POSITION`/`JOINTS_0`
accessor size and range (beach: 51 639 vertices, `JOINTS_0` max 67; citadel: 78 578, max
91), whatever each file's `skin.joints` length. Over the vertices the indices actually
reference, the joints are in range (`barrel-lod0`: 103 vertices, max joint 2 of 3;
`seagull-lod0`: 95, max 9 of 10; `babak-lod0`: 3 545, max 40 of 47), so each file renders
its own mesh. The out-of-range joints sit in unreferenced vertices only. The merc
`POSITION` accessor also lacks the `min`/`max` glTF requires (three.js warns).

Two consequences the viewer handles:

- three.js computes a `SkinnedMesh` bounding sphere over every vertex in the attribute,
  both for frustum culling and for render-order sorting, and throws on an out-of-range
  joint. The viewer sets `frustumCulled = false` and pre-assigns each skinned mesh's
  `boundingSphere`, so three.js never runs that computation.
- Camera fit skins only the vertices the index buffers reference (`getVertexPosition`) and
  takes their 1st-99th percentile per axis, so neither the rest of the pool nor props a
  clip parks far away count.

## Two-up against the game

`$SANDOVER_DATA/viewer/compare/mayor-body-compare.png`: in-game rig capture (left; noon,
effects off, `beach-start` continue so the beach level, which owns the mayor actor, is
displayed; fixed camera 5.5 m on the actor's +Z side), the first viewer (middle) and this
one (right; unlit, engine alpha, envmap pass, stand-in eyes). Raw captures and sidecars in
`compare/ref2/` and `compare/ref3/`. The viewer camera was placed by eye (the game camera's
fov and the actor's exact in-game position are not modelled) and the pose is `mayor-idle`
frame 0, not the game's frame. After the fix the vest front, solid teal hair and mustache
and white eyes match; still different are the lighting (unlit mode has none; the game
lights merc per vertex), eye detail and envmap tint.

## Vendored three.js

`static/vendor/three/` carries three.js 0.186.0 (MIT, `LICENSE` alongside): the split
`three.core.js` + `three.module.js` build, plus `loaders/GLTFLoader.js`,
`controls/OrbitControls.js`, `controls/TransformControls.js` (light gizmo; imports only
`three`) and the two utils GLTFLoader imports (`utils/BufferGeometryUtils.js`,
`utils/SkeletonUtils.js`). `static/index.html` resolves the bare `three` import via an
`<script type="importmap">` pointing at the vendored module — no CDN, no build step. To
update: replace all six files from a newer `three` npm tarball's `build/` and
`examples/jsm/{loaders,controls,utils}/`, keeping the same relative layout.

## Material context: which models use a texture

At startup the server reads every catalogued glb's JSON chunk a second time (cheap: the
same targeted read `read_glb_json` already does for the catalogue, no binary chunk) and
builds `{stem: [{id, kind, triangles}, ...]}` from each glTF's `textures`/`images` arrays,
resolved the same way `GLTFLoader` names a texture (`textureDef.name or sourceDef.name`).
Models are ordered skinned characters first, then props, then background chunks, then by
triangle count ascending within a group (`serve.KIND_RANK`, `serve.build_texture_model_index`);
triangle count comes from accessor `count` fields only, no binary buffer read. The index is
cached as JSON under `$SANDOVER_DATA/viewer/cache/tex-index-<hash of the root path>.json`,
keyed by the rip root directory's own mtime; a restart with an unchanged root reuses the
cache (`[viewer] texture usage index: N stem(s) (cache hit)` on stderr), a changed root
rebuilds it — a second JSON-chunk-only pass over the catalogue, roughly the same cost as
the catalogue scan itself. `GET /api/models-using/<stem>` returns that stem's list, `[]`
for a stem no catalogued model uses.

**URL parameters**: `?model=<catalogue id>&highlight=<stem>` loads that model and highlights
every primitive whose material or envmap texture matches `<stem>` (an additive tint,
pulsing over 1.2 s; `prefers-reduced-motion` falls back to a static tint, and the legend's
"Stop pulse" button freezes it manually). The camera fits to just the highlighted
primitives' bounds instead of the whole model, both on load and on **Reset view** (`R`).
The top-right legend reads "highlighted: `<stem>` (N primitives)", or "(not used by this
model)" if the stem was loaded on a model that turns out not to reference it. Used by the
material spot-check reviewer's Context panel (`kb/howto/materials.md`).

## Inspect panel: click-to-inspect and emissive marking

**Shift+click** a primitive to raycast it (`THREE.Raycaster`, against every primitive the
current model draws, so it works in any shading mode). A panel opens bottom-right and the hit
primitive flashes an additive tint for 0.7 s (`flashPrimitive`, independent of the persistent
highlight/`?highlight=` mechanism above). The panel shows:

- **Texture / material**: the hit primitive's texture name and, from `GET
  /api/material/<name>` (the viewer's own `materials.csv` row, first `tpage` wins on a
  colliding stem -- same convention as the texture index), `tpage`, `combo_id`, `class` and
  `tags`. A texture `materials.csv` has no row for (not yet classified, or not a `sandover`
  texture at all) shows "unclassified" and blank `tpage`/`combo_id`.
- **Texel colour**: the swatch samples the *currently bound* map (so it follows texture-set
  swaps) at the hit UV, alpha doubled (PS2 128 = opaque, same convention as everywhere else).
  Sampled from an offscreen canvas of the texture's image, cached per image.
- **E**: the nearest hit vertex's emissive value from the level's `.unbake` sidecar
  (`sandover.unbake.sidecar`), via `GET /api/vertex-e?model=<id>&tree=<name>&vertex=<i>`.
  `tree` is the clicked primitive's scene-root ancestor name (computed once per model at load
  time, walking up to the direct child of the model's root -- the glb's "tree" in
  `sandover.unbake` terms); the server resolves it to a sidecar tree the same way
  `sidecar.build` numbers them (`sidecar.scan_glb_tree_names`, JSON-only, no binary chunk
  read) and reads that tree's E at the vertex index. Shows **"no sidecar"** for anything that
  isn't a tfrag/tie primitive of a catalogued level chunk with a sidecar on disk, or when the
  vertex index doesn't fit that tree's vertex count (a stale/mismatched sidecar).
- **UV and world position** of the hit.

**Mark emissive** / **Mark not emissive** (`E`/`N` keys) / **Clear** post `{combo_id, tpage,
name, level, draw_id, verdict, note}` to `POST /api/mark`, which appends a row to
`$SANDOVER_DATA/materials/emissive-overrides.csv` (`sandover.materials.overrides` -- exact
column contract shared with the in-game inspect tool on `inspect-tool` and the reviewer's
Browse tab, `kb/howto/materials.md`). `Clear` posts an empty `verdict`, which
`overrides.latest_verdicts` treats as removing any prior verdict for that key. `draw_id` is
the clicked THREE.js object's own name (best-effort context, not a stable cross-tool id --
the column is optional for exactly this reason). The file is append-only; closing the tab
loses nothing, same as the reviewer's queue CSV.

## Bloom preview: `?bloom=1&emissive=<stem>&strength=<f>`

For the reviewer's Emissive tab (`kb/howto/materials.md`, "Emissive review"): the page adds an
emissive bloom driven by one texture's emissive map
(`sandover-materials emissive-maps`), served by `GET /api/emissive-map/<stem>` from
`--emissive-maps` (default `$SANDOVER_DATA/materials/emissive-maps`; the route looks the stem
up in that directory's `index.json`, re-read when it changes, and 404s otherwise).

`static/bloom.js` (`EmissiveBloom`) is a small custom bloom, not three.js's `UnrealBloomPass`,
modelled on the engine's `BloomRenderer` with its defaults (`gfx.h`):

1. **Bright pass** at half the drawing-buffer resolution: the scene is drawn with every
   primitive using `<stem>` swapped to `ramp(E) x max(colour x strength - 0.3, 0)`, where `E`
   is the emissive map sampled at the primitive's UV and `ramp` the engine's 0.1..0.4
   smoothstep; every other primitive draws black (keeping its alpha cut) so it still occludes.
   Helpers (light gizmo, ground, axes) and overlay passes are hidden for this pass.
2. **Blur**: separable Gaussian, radius 6 half-res pixels, sigma = radius / 2 (the engine's
   `bloom_blur.frag` kernel).
3. **Composite**: the normal frame, then the blur added on top (engine bloom strength 1).

It is an approximation: the engine's bright pass reads the *lit* frame colour and the
sidecar's per-vertex E on every emissive surface at once, where this one reads the unlit (or
PBR) viewer colour and one texture's texture-space map; there is no MSAA resolve and no
tonemap. `strength` multiplies the colour before the threshold, so it behaves like an HDR
emission scale. The status line reads `bloom <stem> x<strength> (<n> prims; approximation)`,
with `no emissive map` when the route 404s. The camera frames the primitives using `<stem>`
when there is no `highlight`.

The page also accepts `{type: "sandover-bloom-strength", strength}` via `postMessage` from a
loopback origin, which is how the reviewer's ladder updates the glow without reloading the
iframe.

## Gaps

- Eyes are a stand-in; the engine's eye renderer (iris, lids, blink) is not reproduced.
- The envmap pass is a matcap approximation with a user strength, not emerc's reflection
  coordinates and sun-tinted fade.
- Baked mode on characters shows the rip's per-vertex `rgba` only, not merc's per-frame
  directional lighting; on level chunks it uses `COLOR_0` only, not the 8-slot
  time-of-day blend.
- Clips that park props by moving joints (fisher's fish) show those props where the clip
  puts them; the engine hides them.
- No side-by-side linked-camera view, no sun light, no engine-exact turntable mode — all
  explicitly later/blocked in ADR-0015.
