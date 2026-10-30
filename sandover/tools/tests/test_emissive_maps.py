import struct
from pathlib import Path

import numpy as np
import pygltflib
import pytest
from PIL import Image

from sandover.materials import emissive_maps as em


def _quad(u0=0.0, v0=0.0, u1=1.0, v1=1.0):
    """Two triangles covering [u0,u1] x [v0,v1]."""
    a, b, c, d = (u0, v0), (u1, v0), (u1, v1), (u0, v1)
    return np.array([[a, b, c], [a, c, d]], np.float64)


def test_rasterize_quad_constant_e_covers_every_texel():
    out = np.zeros((8, 8), np.float32)
    em.rasterize(_quad(), np.full((2, 3), 0.5), out)
    assert np.allclose(out, 0.5)


def test_rasterize_interpolates_along_u():
    out = np.zeros((4, 8), np.float32)
    e = np.array([[0.0, 1.0, 1.0], [0.0, 1.0, 0.0]])  # corners a=0, b=1, c=1, d=0: E = u
    em.rasterize(_quad(), e, out)
    centres = (np.arange(8) + 0.5) / 8
    assert np.allclose(out[1], centres, atol=1e-5)


def test_rasterize_half_quad_leaves_other_half_empty():
    out = np.zeros((8, 8), np.float32)
    em.rasterize(_quad(0.0, 0.0, 0.5, 1.0), np.ones((2, 3)), out)
    assert out[:, :4].min() == 1.0
    assert out[:, 5:].max() == 0.0


def test_rasterize_wraps_repeated_uvs_into_the_unit_tile():
    out = np.zeros((8, 8), np.float32)
    em.rasterize(_quad(2.0, -1.0, 2.5, 0.0), np.ones((2, 3)), out)  # same half, other tiles
    assert out[:, :4].min() == 1.0
    assert out[:, 5:].max() == 0.0


def test_rasterize_takes_the_max_over_overlapping_draws():
    out = np.zeros((4, 4), np.float32)
    for e in (0.2, 0.7, 0.4):
        em.rasterize(_quad(), np.full((2, 3), e), out)
    assert np.allclose(out, 0.7)


def test_rasterize_splats_a_sub_texel_triangle():
    out = np.zeros((4, 4), np.float32)
    uv = np.array([[[0.60, 0.60], [0.61, 0.60], [0.60, 0.61]]])
    em.rasterize(uv, np.array([[0.9, 0.9, 0.9]]), out)
    assert out.max() == pytest.approx(0.9)
    assert (out > 0).sum() == 1


def test_map_stats_coverage_uses_the_threshold():
    m = np.zeros((10, 10), np.float32)
    m[:5] = 0.05  # below COVERED
    m[5:, :5] = 0.8
    st = em.map_stats(m)
    assert st["coverage"] == pytest.approx(0.25)
    assert st["mean"] == pytest.approx(0.8)
    assert st["max"] == pytest.approx(0.8)


def test_find_anchors_matches_name_and_role():
    tex = {
        "vil-torch-flame": {"source": "bake", "coverage": 0.5, "peak": 0.9, "role": "flames", "tpage": "a"},
        "fire-rock": {"source": "bake", "coverage": 0.9, "peak": 0.9, "role": "night", "tpage": "a"},
        "lava-flow": {"source": "bake", "coverage": 0.3, "peak": 1.0, "role": "lava", "tpage": "b"},
        "lava-glow": {"source": "bake", "coverage": 0.1, "peak": 1.0, "role": "lava", "tpage": "b"},
        "eco-vent": {"source": "luminance", "coverage": 1.0, "peak": 1.0, "role": "", "tpage": "c"},
    }
    a = em.find_anchors(tex)
    assert a["torch"] == "vil-torch-flame"  # fire-rock's glow is not from a flames slot
    assert a["lava"] == "lava-flow"
    assert "eco" not in a  # luminance-only textures are never anchors


def test_default_strengths_relative_to_torch():
    tex = {
        "torch": {"source": "bake", "peak": 0.8, "luma_p95": 0.6},
        "lamp": {"source": "bake", "peak": 0.4},
        "sign": {"source": "luminance", "peak": 1.0, "luma_p95": 0.6},
    }
    ref = em.default_strengths(tex, "torch")
    assert ref["texture"] == "torch"
    assert tex["torch"]["default_strength"] == pytest.approx(1.0)
    assert tex["lamp"]["default_strength"] == pytest.approx(0.5)
    assert tex["sign"]["default_strength"] == pytest.approx(em.LUMINANCE_DISCOUNT)


def test_pick_tpage_prefers_a_level_tpage():
    assert em.pick_tpage(["common", "jungle-vis-tfrag"], {"jungle"}) == "jungle-vis-tfrag"
    assert em.pick_tpage(["common", "zzz"], {"jungle"}) == "common"


def _write_unbake(path: Path, e: np.ndarray, src: int = 5) -> None:
    hdr = b"SOVU" + struct.pack("<7I", 2, 43, 1, 0, 0, 0, 0)
    rec = np.zeros((len(e), 4), np.uint32)
    rec[:, 3] = (np.clip(np.rint(e * 255), 0, 255).astype(np.uint32) << np.uint32(16)) | np.uint32(src << 24)
    data_start = len(hdr) + 64 * 8 + 32
    tbl = struct.pack("<4I2Q", 0, 0, len(e), 0, 0, data_start)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(hdr + bytes(64 * 8) + tbl + rec.tobytes())


def _write_glb(path: Path, uv: np.ndarray, indices: list[int], tex_name: str) -> None:
    g = pygltflib.GLTF2()
    g.scene = 0
    g.scenes = [pygltflib.Scene(nodes=[0])]
    g.nodes = [pygltflib.Node(name="village1-tfrag-normal", children=[1]), pygltflib.Node(mesh=0)]
    g.meshes = [pygltflib.Mesh(primitives=[pygltflib.Primitive(
        attributes=pygltflib.Attributes(TEXCOORD_0=1), indices=0, material=0)])]
    g.materials = [pygltflib.Material(pbrMetallicRoughness=pygltflib.PbrMetallicRoughness(
        baseColorTexture=pygltflib.TextureInfo(index=0)))]
    g.textures = [pygltflib.Texture(name=tex_name, source=0)]
    g.images = [pygltflib.Image(name=tex_name)]
    ib = np.asarray(indices, "<u2").tobytes()
    ib += b"\0" * (-len(ib) % 4)
    uvb = np.asarray(uv, "<f4").tobytes()
    blob = ib + uvb
    g.buffers = [pygltflib.Buffer(byteLength=len(blob))]
    g.bufferViews = [pygltflib.BufferView(buffer=0, byteOffset=0, byteLength=len(ib)),
                     pygltflib.BufferView(buffer=0, byteOffset=len(ib), byteLength=len(uvb))]
    g.accessors = [pygltflib.Accessor(bufferView=0, componentType=5123, count=len(indices), type="SCALAR"),
                   pygltflib.Accessor(bufferView=1, componentType=5126, count=len(uv), type="VEC2")]
    g.set_binary_blob(blob)
    path.parent.mkdir(parents=True, exist_ok=True)
    g.save_binary(str(path))


def test_build_end_to_end_on_a_synthetic_quad(tmp_path: Path):
    levels, lighting, tex_root, out = (tmp_path / d for d in ("levels", "lighting", "tex", "out"))
    uv = np.array([[0, 0], [1, 0], [1, 1], [0, 1]], np.float32)
    _write_glb(levels / "village1" / "village1-background.glb", uv, [0, 1, 2, 0, 2, 3], "vil-torch")
    _write_unbake(lighting / "village1.unbake", np.array([1.0, 1.0, 1.0, 1.0]))
    (tex_root / "village1-vis-tfrag").mkdir(parents=True)
    Image.new("RGBA", (16, 8), (200, 120, 40, 128)).save(tex_root / "village1-vis-tfrag" / "vil-torch.png")
    idx = em.build(levels, lighting, out, tex_root, ["village1"], overrides_csv=tmp_path / "none.csv",
                   log=lambda *_: None)
    t = idx["textures"]["vil-torch"]
    assert (t["width"], t["height"], t["tpage"]) == (16, 8, "village1-vis-tfrag")
    assert t["coverage"] == pytest.approx(1.0)
    assert t["role"] == "flames"  # slot 5 is village1's flames slot
    assert idx["anchors"]["torch"]["name"] == "vil-torch"
    assert t["default_strength"] == pytest.approx(1.0)
    png = np.asarray(Image.open(out / "village1-vis-tfrag" / "vil-torch.png"))
    assert png.shape == (8, 16) and png.min() == 255


def test_find_anchors_takes_the_median_candidate():
    tex = {f"torch-{i}": {"source": "bake", "coverage": 1.0, "peak": p, "role": "flames"}
           for i, p in enumerate([0.2, 1.0, 0.5, 0.9, 1.0])}
    assert em.find_anchors(tex)["torch"] == "torch-3"  # peaks 0.2 0.5 [0.9] 1.0 1.0


def test_lava_falls_back_to_named_luminance_textures():
    tex = {"lava-flow-01": {"source": "luminance", "coverage": 0.5, "peak": 1.0, "luma_p95": 0.8},
           "lava-rock": {"source": "luminance", "coverage": 1.0, "peak": 1.0, "luma_p95": 0.9}}
    assert em.find_anchors(tex)["lava"] == "lava-flow-01"


def test_reindex_adds_override_textures_without_rescanning(tmp_path: Path):
    tex_root, out = tmp_path / "tex", tmp_path / "out"
    (tex_root / "common").mkdir(parents=True)
    Image.new("RGBA", (4, 4), (250, 250, 250, 128)).save(tex_root / "common" / "sign.png")
    out.mkdir()
    (out / "index.json").write_text('{"drawn_textures": 3, "textures": {}}')
    ov = tmp_path / "ov.csv"
    ov.write_text("combo_id,tpage,name,level,draw_id,verdict,note,timestamp\n,common,sign,,,emissive,,t\n")
    idx = em.reindex(out, tex_root, ov)
    assert idx["textures"]["sign"]["source"] == "luminance"
    assert idx["textures"]["sign"]["default_strength"] > 0
    assert (out / "common" / "sign.png").is_file()
