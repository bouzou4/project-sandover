# Glossary

Short definitions for OpenGOAL / Jak 1 terms used throughout the KB. All entries are
**unverified** — Stage 1 (the mood facts work) must confirm or refute each one against source
and record the result in `sandover/kb/facts/`.

- **tfrag** (terrain fragment): a chunk of static background geometry (ground, buildings,
  rocks) baked with vertex-lit color from the mood/palette system. No per-vertex normals
  in the packed format. *unverified*
- **tie** (tie-instanced environment / "instanced" background geometry): repeated static
  props (trees, lamp posts, crates) placed by instance, sharing a mesh but each with its
  own palette color indices; some fragments ("generic"/envmapped) carry `s8` normals.
  *unverified*
- **shrub**: small background foliage/clutter geometry, similar in spirit to tfrag but a
  distinct draw path; no normals. *unverified*
- **merc** (Merc2 / "MERCurial"?): the skinned/animated character and object mesh format
  (bones, blend shapes), lit per-vertex by up to 3 directional lights + ambient rather
  than baked. *unverified*
- **itimes**: the interpolation weights (`update-mood-itimes`) that blend the 8 mood
  palette slots together each frame — effectively a light-layer mixer. *unverified*
- **mood**: the time-of-day / weather lighting system (`mood.gc`, `mood-tables.gc`) that
  drives palette snapshots, effect lights (flames, lamps, lightning, etc.), and the
  characters' key light. *unverified*
- **palette slot**: one of 8 indexed color layers per level whose baked vertex colors are
  blended by `itimes` to produce the final lit color; some slots are sun snapshots, others
  are effect lights (fire, lamps, lava, caustics, lightning). *unverified*
- **continue point**: a named checkpoint/spawn location (`level-info.gc`) with a saved
  camera position/rotation, used to resume play or as a rig camera anchor. *unverified*
- **bucket**: an ordered renderer stage/queue (`buckets.h`) that groups draw calls (e.g.
  opaque vs transparent); bucket 30→31 is the opaque/transparent boundary used as an
  injection point for post-processing like AO. *unverified*
- **FR3**: the compiled level geometry format produced by the decompiler
  (`TFRAG3_VERSION`), containing tfrag/tie/shrub draw data, textures, and palettes.
  *unverified*
- **tpage** (texture page): a group/atlas of related textures identified by an ID, used to
  key `combo_id` and to organize ripped textures on disk. *unverified*
- **combo_id**: `tpage << 16 | idx`, a unique identifier for a texture used to associate
  per-texture data (e.g. material class) across tools. *unverified*
- **nREPL**: the network REPL protocol goalc exposes on port 8181 (8-byte header framing,
  no return value over the socket) used to drive the running game/compiler from scripts.
  *unverified*
- **goalc**: the GOAL compiler, a persistent process that compiles and can send code to a
  running game (`gk`) via nREPL/listener. *unverified*
- **gk**: the game kernel / runtime executable ("Game Kernel"?) that boots and runs the
  actual game, driven by flags like `-debug`, `-level`, `--config-path`, `--proj-path`,
  `--port`. *unverified*
