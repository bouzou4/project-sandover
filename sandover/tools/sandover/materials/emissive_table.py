"""``sandover-materials emissive-table``: per-texture E, precomputed from the sidecars.

For every level background glb, reads its `.unbake` sidecar (`sandover.unbake.sidecar`) and
walks the glb's tfrag/tie trees, taking each primitive's material's texture and the max E
over the vertices its triangles actually draw. The result -- texture name -> max E over every
draw of it anywhere in the game -- is what the Browse tab's "emissive" class and the viewer's
inspect panel both want, and is too slow to compute per click (a full pygltflib parse plus a
sidecar read per level), so it is cached once under
`$SANDOVER_DATA/materials/emissive-table.json` and reused until re-run.

Shrub trees are skipped: `sidecar`'s vertex-order guarantee only covers tfrag/tie (see its
module docstring), and shrub sits behind a separate per-instance transform this table does
not attempt to resolve.

Keyed by texture name (the glTF texture/image name, i.e. the PNG stem) rather than
(tpage, name): the viewer's own texture index resolves textures by stem alone (first match
wins, see kb/howto/viewer.md), and this table joins against that and against the reviewer's
`class_members` the same way.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path

import numpy as np
import pygltflib

from sandover.materials.sheets import materials_dir
from sandover.unbake import sidecar
from sandover.unbake.glb import SKIP_TREES, _read
from sandover.unbake.levels import LEVELS

EMISSIVE_THRESHOLD = 0.1  # matches the sidecar report's "clearly emissive" histogram bucket


def table_path() -> Path:
    return materials_dir() / "emissive-table.json"


def _material_texture_name(g: pygltflib.GLTF2, material_idx: int | None) -> str | None:
    if material_idx is None or not (0 <= material_idx < len(g.materials)):
        return None
    pbr = g.materials[material_idx].pbrMetallicRoughness
    tex_ref = pbr.baseColorTexture if pbr else None
    if tex_ref is None:
        return None
    tex = g.textures[tex_ref.index]
    name = tex.name
    if not name and tex.source is not None:
        name = g.images[tex.source].name
    return name or None


def scan_level(level: str, levels_dir: Path, lighting_dir: Path) -> dict[str, float]:
    """Texture name -> max E over this level's tfrag/tie draws; {} if either file is missing."""
    glb_path = levels_dir / level / f"{level}-background.glb"
    sc_path = lighting_dir / f"{level}.unbake"
    if not glb_path.is_file() or not sc_path.is_file():
        return {}
    g = pygltflib.GLTF2().load(str(glb_path))
    blob = g.binary_blob()
    parsed = sidecar.read(sc_path)
    tree_e = {}
    for t in parsed["trees"]:
        rec = t["records"]
        tree_e[(t["kind"], t["fr3_tree"])] = ((rec[:, 3] >> 16) & 0xFF) / 255.0

    out: dict[str, float] = {}
    scene = g.scenes[g.scene or 0]
    n_tfrag = 0
    for root_idx in scene.nodes:
        root = g.nodes[root_idx]
        name = root.name or ""
        if any(s in name for s in SKIP_TREES):
            continue
        if "-tfrag-" in name:
            key = (0, n_tfrag)
            n_tfrag += 1
        elif "-tie-" in name:
            key = (1, int(name.rsplit("-", 1)[1]))
        else:
            continue  # shrub or other: no vertex-order guarantee, see module docstring
        e = tree_e.get(key)
        if e is None:
            continue
        for c in root.children:
            node = g.nodes[c]
            if node.mesh is None:
                continue
            for p in g.meshes[node.mesh].primitives:
                if p.indices is None:
                    continue
                tex_name = _material_texture_name(g, p.material)
                if not tex_name:
                    continue
                idx = _read(g, blob, p.indices).astype(np.int64).ravel()
                idx = idx[(idx >= 0) & (idx < len(e))]
                if not len(idx):
                    continue
                m = float(e[idx].max())
                if m > out.get(tex_name, -1.0):
                    out[tex_name] = m
    return out


def build_table(levels_dir: Path, lighting_dir: Path, levels: list[str] | None = None) -> dict[str, float]:
    """Texture name -> max E over every tfrag/tie draw of it, across all levels scanned."""
    out: dict[str, float] = {}
    for lvl in levels or list(LEVELS):
        for name, e in scan_level(lvl, levels_dir, lighting_dir).items():
            if e > out.get(name, -1.0):
                out[name] = e
    return out


def write_table(path: Path, table: dict[str, float]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(table, indent=1, sort_keys=True), encoding="utf-8")


def load_table(path: Path) -> dict[str, float]:
    if not path.is_file():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="sandover-materials emissive-table", description=__doc__.splitlines()[0])
    root = Path(os.environ.get("SANDOVER_ROOT", "."))
    data = Path(os.environ.get("SANDOVER_DATA", root / "sandover/data"))
    ap.add_argument("levels", nargs="*", choices=list(LEVELS), help="default: all 21")
    ap.add_argument("--levels-dir", type=Path, default=data / "unbake/riproot/decompiler_out/jak1/levels")
    ap.add_argument("--lighting-dir", type=Path, default=sidecar.scan_dir(data / "lighting"),
                    help="default $SANDOVER_DATA/lighting/raw when the export wrote it, else lighting")
    ap.add_argument("--out", type=Path, default=None, help="default $SANDOVER_DATA/materials/emissive-table.json")
    args = ap.parse_args(argv)

    table = build_table(args.levels_dir, args.lighting_dir, args.levels or None)
    out = args.out or table_path()
    write_table(out, table)
    over = sum(1 for e in table.values() if e > EMISSIVE_THRESHOLD)
    print(f"{len(table)} textures, {over} over the {EMISSIVE_THRESHOLD} threshold -> {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
