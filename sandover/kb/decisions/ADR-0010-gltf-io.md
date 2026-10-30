# ADR-0010: glTF color-set I/O split
Status: accepted    Date: 2026-09-23    Deciders: project lead

## Context
The unbake tool must round-trip up to 8 vertex color sets (`COLOR_0`..`COLOR_7`) per mesh
without breaking the vertex-to-palette-entry mapping. The tooling evaluation found that trimesh's glTF loader
only reads/writes `COLOR_0` — `COLOR_1..COLOR_7` are never referenced in its loader code
and are silently dropped, not even preserved as custom attributes. trimesh's default
`trimesh.load()` also merges vertices unless `process=False` is passed, which would
independently scramble the same mapping.

## Options (with cost/risk)
**trimesh for all I/O.** Cost: would need color sets reattached to trimesh's arrays after
the fact just to work around the `COLOR_0`-only loader — more code than avoiding trimesh
for color data entirely. Risk: high — silent data loss (`COLOR_1..7` dropped) is the kind
of bug that only surfaces downstream in a broken unbake fit.
**pygltflib for all accessor I/O, trimesh for geometry helpers only (chosen).** pygltflib
is a thin dataclass wrapper over the raw glTF JSON/accessor/GLB structure; it does not
special-case color attribute names, so arbitrary `COLOR_n` accessors pass through
untouched on read and write. Cost: pygltflib's canonical repo is on GitLab, not GitHub —
watch that when scripting update/version checks.

## Decision
pygltflib owns all accessor-level I/O: positions, normals, UVs, and all 8 color sets, plus
GLB binary chunk read/write. trimesh is used only for geometry operations (e.g. computing
vertex normals) on pre-extracted numpy arrays, constructed with `process=False` and never
calling `merge_vertices()`, so vertex order — and therefore color-accessor row alignment —
stays intact.

## Consequences
- No color data is ever routed through trimesh, at any point in the pipeline.
- Write a pygltflib round-trip smoke test (load, mutate one `COLOR_n` accessor, save,
  reload) before committing real assets to the split, to catch silent accessor
  renumbering or buffer-padding issues.
- Source: `kb/findings/2026-09-23-gltf-io.md`.
