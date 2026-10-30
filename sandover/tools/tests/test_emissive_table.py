import struct
from pathlib import Path

import numpy as np
import pygltflib
import pytest

from sandover.materials import emissive_table as et


def _write_unbake(path: Path, trees: list[tuple[int, int, np.ndarray]]) -> None:
    """Minimal ``.unbake`` with only E populated, matching ``sidecar.read``'s layout."""
    hdr = b"SOVU" + struct.pack("<7I", 2, 43, len(trees), 0, 0, 0, 0)
    glob = bytes(64 * 8)
    data_start = len(hdr) + len(glob) + 32 * len(trees)
    tbl, blocks, pos = bytearray(), [], data_start
    for kind, idx, e in trees:
        rec = np.zeros((len(e), 4), np.uint32)
        rec[:, 3] = np.clip(np.rint(e * 255), 0, 255).astype(np.uint32) << np.uint32(16)
        blk = rec.tobytes()
        tbl += struct.pack("<4I2Q", kind, idx, len(e), 0, 0, pos)
        blocks.append(blk)
        pos += len(blk)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(hdr + glob + bytes(tbl) + b"".join(blocks))


def _write_glb(path: Path, root_name: str, indices: list[int], tex_name: str) -> None:
    """One root node (``root_name``) with one child mesh, one primitive, one textured material."""
    g = pygltflib.GLTF2()
    g.scene = 0
    g.scenes = [pygltflib.Scene(nodes=[0])]
    g.nodes = [pygltflib.Node(name=root_name, children=[1]), pygltflib.Node(mesh=0)]
    g.meshes = [pygltflib.Mesh(primitives=[pygltflib.Primitive(indices=0, material=0)])]
    g.materials = [pygltflib.Material(
        pbrMetallicRoughness=pygltflib.PbrMetallicRoughness(
            baseColorTexture=pygltflib.TextureInfo(index=0)))]
    g.textures = [pygltflib.Texture(source=0)]
    g.images = [pygltflib.Image(name=tex_name)]
    blob = np.asarray(indices, dtype="<u2").tobytes()
    g.buffers = [pygltflib.Buffer(byteLength=len(blob))]
    g.bufferViews = [pygltflib.BufferView(buffer=0, byteOffset=0, byteLength=len(blob))]
    g.accessors = [pygltflib.Accessor(bufferView=0, componentType=5123, count=len(indices), type="SCALAR")]
    g.set_binary_blob(blob)
    path.parent.mkdir(parents=True, exist_ok=True)
    g.save_binary(str(path))


@pytest.fixture
def level_files(tmp_path: Path) -> tuple[Path, Path]:
    levels_dir, lighting_dir = tmp_path / "levels", tmp_path / "lighting"
    _write_glb(levels_dir / "jungle" / "jungle-background.glb", "jungle-tfrag-0", [0, 1, 2], "tex-a")
    _write_unbake(lighting_dir / "jungle.unbake", [(0, 0, np.array([0.0, 0.2, 0.05]))])
    return levels_dir, lighting_dir


def test_scan_level_takes_max_e_over_the_primitives_indices(level_files):
    levels_dir, lighting_dir = level_files
    out = et.scan_level("jungle", levels_dir, lighting_dir)
    assert out == {"tex-a": pytest.approx(0.2, abs=1e-3)}


def test_scan_level_empty_when_glb_or_sidecar_missing(tmp_path: Path):
    assert et.scan_level("nope", tmp_path, tmp_path) == {}


def test_build_table_merges_across_levels(tmp_path: Path):
    # 26/255 and 76/255: exact quantized steps, so no rounding ambiguity in the assertion.
    levels_dir, lighting_dir = tmp_path / "levels", tmp_path / "lighting"
    _write_glb(levels_dir / "jungle" / "jungle-background.glb", "jungle-tfrag-0", [0, 1, 2], "shared")
    _write_unbake(lighting_dir / "jungle.unbake", [(0, 0, np.array([0.0, 26 / 255, 0.0]))])
    _write_glb(levels_dir / "beach" / "beach-background.glb", "beach-tfrag-0", [0, 1, 2], "shared")
    _write_unbake(lighting_dir / "beach.unbake", [(0, 0, np.array([76 / 255, 0.0, 0.0]))])
    table = et.build_table(levels_dir, lighting_dir, ["jungle", "beach"])
    assert table["shared"] == pytest.approx(76 / 255, abs=1e-6)


def test_write_and_load_table_roundtrip(tmp_path: Path):
    path = tmp_path / "emissive-table.json"
    et.write_table(path, {"a": 0.5, "b": 0.0})
    assert et.load_table(path) == {"a": 0.5, "b": 0.0}


def test_load_table_missing_file_is_empty(tmp_path: Path):
    assert et.load_table(tmp_path / "nope.json") == {}
