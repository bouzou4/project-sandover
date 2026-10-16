from pathlib import Path
import base64
import io

import numpy as np
import pygltflib
from PIL import Image

from sandover.unbake import canopy as cp


def _quad(x0, x1, z0, z1, y, uv_rep=1.0):
    """Two horizontal triangles and their UVs."""
    a, b, c, d = (x0, y, z0), (x1, y, z0), (x1, y, z1), (x0, y, z1)
    tri = np.array([[a, b, c], [a, c, d]], np.float64)
    uv = np.array([[(0, 0), (1, 0), (1, 1)], [(0, 0), (1, 1), (0, 1)]], np.float64) * uv_rep
    return tri, uv


def _wall(x, z0, z1, y0, y1):
    """Two vertical triangles (a trunk face) at x."""
    a, b, c, d = (x, y0, z0), (x, y0, z1), (x, y1, z1), (x, y1, z0)
    return np.array([[a, b, c], [a, c, d]], np.float64), np.zeros((2, 3, 2))


def _alpha(kind):
    if kind == "solid":
        return cp.TextureAlpha(np.ones((8, 8)))
    a = np.indices((8, 8)).sum(0) % 2 * 1.0  # checker, half the texels cut out
    return cp.TextureAlpha(a, cutout=0.5)


def _scene():
    """Ground 0..60 x 0..20 at y 0; a tree (trunk + crown at 10 m) at x 10; a bush at 0.5 m;
    a cliff top at y 10 from x 40 with a grass fringe hanging past its edge and no tree."""
    meshes, alphas, classes = [], {}, {}

    def add(name, cls, alpha, parts):
        meshes.append(cp.Mesh(name, np.concatenate([p[0] for p in parts]), np.concatenate([p[1] for p in parts])))
        alphas[name] = _alpha(alpha)
        classes[name] = cls

    add("ground", "stone", "solid", [_quad(0, 40, 0, 20, 0.0)])
    add("cliff", "stone", "solid", [_quad(43, 60, 0, 20, 10.0)])
    add("trunk", "wood", "solid", [_wall(10, 9.5, 10.5, 0, 9.5)])
    add("crown", "foliage", "checker", [_quad(5, 15, 5, 15, 10.0, uv_rep=2.0)])
    add("bush", "foliage", "checker", [_quad(20, 26, 5, 11, 0.5)])
    add("fringe", "foliage", "checker", [_quad(36, 43, 5, 15, 10.0)])
    return meshes, alphas, classes


def test_classify_picks_tree_crowns_only():
    meshes, alphas, classes = _scene()
    keep, rep = cp.classify(meshes, alphas, classes, cp.Params())
    assert set(keep) == {"crown"}
    assert rep["crown"]["canopy"] and rep["crown"]["held_share"] == 1.0
    assert rep["bush"]["clear_m2"] == 0.0  # 0.5 m above the ground
    assert rep["fringe"]["held_m2"] == 0.0  # clear of the ground below the cliff, but no tree
    assert "ground" not in rep and "trunk" not in rep  # not foliage


def test_solid_foliage_is_not_a_candidate():
    meshes, alphas, classes = _scene()
    alphas["crown"] = _alpha("solid")
    keep, rep = cp.classify(meshes, alphas, classes, cp.Params())
    assert "crown" not in rep and not keep


def test_rasterize_coverage_and_height():
    meshes, alphas, classes = _scene()
    keep, _ = cp.classify(meshes, alphas, classes, cp.Params())
    crown = meshes[3]
    p = cp.Params(blur=0.0)
    cm = cp.rasterize([(crown.tri[keep["crown"]], crown.uv[keep["crown"]], alphas["crown"])], p)
    assert cm.texel == 0.25
    i0, j0 = int((7 - cm.x0) / cm.texel), int((7 - cm.z0) / cm.texel)
    inner = cm.coverage[j0:j0 + 24, i0:i0 + 24]
    assert abs(inner.mean() - 0.45) < 0.05  # checker alpha: half the samples, each blocks opacity 0.9
    assert np.allclose(cm.height, 10.0, atol=1e-4)  # empty texels take the nearest canopy height
    outside = cm.coverage[:, : int((4 - cm.x0) / cm.texel)]
    assert outside.max() == 0.0
    covered = cm.coverage.sum() * cm.texel ** 2
    assert abs(covered - 45.0) < 3.0  # 10 x 10 m crown, half open, opacity 0.9


def test_large_extent_coarsens_texel():
    tri, uv = _quad(0, 2000, 0, 2, 10.0)
    cm = cp.rasterize([(tri, uv, _alpha("solid"))], cp.Params(blur=0.0))
    assert cm.texel > 0.25 and max(cm.coverage.shape) <= cp.MAX_SIZE


def test_write_read_roundtrip(tmp_path):
    rng = np.random.default_rng(3)
    cov = rng.uniform(0, 1, (7, 9))
    hgt = rng.uniform(20, 60, (7, 9))
    cm = cp.CanopyMap(-12.5, 340.0, 0.5, float(hgt.min()), float(hgt.max()), cov, hgt)
    path = tmp_path / "x.canopy"
    cp.write(path, cm)
    b = path.read_bytes()
    assert b[:4] == b"SOVC" and len(b) == cp.HEADER.size + 7 * 9 * 4
    back = cp.read(path)
    assert (back.x0, back.z0, back.texel) == (-12.5, 340.0, 0.5)
    assert np.allclose(back.coverage, cov, atol=1 / 65535)
    assert np.allclose(back.height, hgt, atol=(hgt.max() - hgt.min()) / 65535 + 1e-4)


def _png_data_uri(alpha):
    img = np.zeros(alpha.shape + (4,), np.uint8)
    img[..., 3] = alpha
    buf = io.BytesIO()
    Image.fromarray(img, "RGBA").save(buf, "PNG")
    return "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode()


def test_load_level_places_instanced_ties(tmp_path):
    pos = np.array([[0, 0, 0], [1, 0, 0], [0, 0, 1]], np.float32)
    uv = np.array([[0, 0], [1, 0], [0, 1]], np.float32)
    idx = np.array([0, 1, 2], np.uint32)
    blob = pos.tobytes() + uv.tobytes() + idx.tobytes()
    g = pygltflib.GLTF2(
        scene=0,
        scenes=[pygltflib.Scene(nodes=[0])],
        nodes=[pygltflib.Node(name="lvl-tie-0", children=[1, 2]), pygltflib.Node(mesh=0),
               pygltflib.Node(mesh=0, matrix=[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 100, 5, 0, 1])],
        meshes=[pygltflib.Mesh(primitives=[pygltflib.Primitive(
            attributes=pygltflib.Attributes(POSITION=0, TEXCOORD_0=1), indices=2, material=0)])],
        materials=[pygltflib.Material(pbrMetallicRoughness=pygltflib.PbrMetallicRoughness(
            baseColorTexture=pygltflib.TextureInfo(index=0)))],
        textures=[pygltflib.Texture(name="leaf", source=0, sampler=0)],
        samplers=[pygltflib.Sampler(wrapS=33071, wrapT=10497)],
        images=[pygltflib.Image(uri=_png_data_uri(np.array([[0, 128], [128, 128]], np.uint8)))],
        accessors=[pygltflib.Accessor(bufferView=0, componentType=5126, count=3, type="VEC3"),
                   pygltflib.Accessor(bufferView=1, componentType=5126, count=3, type="VEC2"),
                   pygltflib.Accessor(bufferView=2, componentType=5125, count=3, type="SCALAR")],
        bufferViews=[pygltflib.BufferView(buffer=0, byteOffset=0, byteLength=36),
                     pygltflib.BufferView(buffer=0, byteOffset=36, byteLength=24),
                     pygltflib.BufferView(buffer=0, byteOffset=60, byteLength=12)],
        buffers=[pygltflib.Buffer(byteLength=len(blob))],
    )
    g.set_binary_blob(blob)
    path = tmp_path / "lvl-background.glb"
    g.save(str(path))
    meshes, alphas = cp.load_level(path)
    assert [m.texture for m in meshes] == ["leaf"]
    tri = meshes[0].tri
    assert tri.shape == (2, 3, 3)
    assert np.allclose(tri[1], pos + [100, 5, 0])  # instance placed by its matrix
    a = alphas["leaf"]
    assert a.alpha.max() == 1.0 and a.cutout == 0.25  # 0..128 range normalized to the texture's own max
    assert not a.wrap_s and a.wrap_t


def test_load_classes_prefers_level_rows(tmp_path):
    csv = tmp_path / "m.csv"
    csv.write_text("combo_id,tpage,name,class,levels\n"
                   "1,a,leaf,foliage,JUN.DGO\n2,b,leaf,wood,VI1.DGO\n3,c,leaf,wood,BEA.DGO\n")
    assert cp.load_classes(csv, "JUN.DGO")["leaf"] == "foliage"
    assert cp.load_classes(csv, None)["leaf"] == "wood"


def test_layers_multiply_and_duplicates_count_once():
    tri, uv = _quad(0, 4, 0, 4, 10.0)
    solid = _alpha("solid")
    p = cp.Params(blur=0.0, opacity=0.5)
    one = cp.rasterize([(tri, uv, solid)], p)
    dup = cp.rasterize([(np.concatenate([tri, tri[:, ::-1]]), np.concatenate([uv, uv[:, ::-1]]), solid)], p)
    tri2, uv2 = _quad(0, 4, 0, 4, 12.0)
    two = cp.rasterize([(np.concatenate([tri, tri2]), np.concatenate([uv, uv2]), solid)], p)
    c = lambda m: m.coverage[int((2 - m.z0) / m.texel), int((2 - m.x0) / m.texel)]
    assert abs(c(one) - 0.5) < 1e-6 and abs(c(dup) - 0.5) < 1e-6
    assert abs(c(two) - 0.75) < 1e-6  # transmittance 0.5 x 0.5, no saturation


def test_header_matches_engine_reader():
    # this checkout's engine source (tests/ -> tools/ -> sandover/ -> repo root)
    repo = Path(__file__).resolve().parents[3]
    cpp = (repo / "game/graphics/opengl_renderer/background/Canopy.cpp").read_text(encoding="utf-8")
    assert cp.HEADER.size == 48 and "static_assert(sizeof(hdr) == 48);" in cpp
    assert f"constexpr u32 kVersion = {cp.VERSION};" in cpp
    # field order: magic, version, w, h, x0, z0, texel, y0, y1, pad[3]
    assert "u32 version, w, h;" in cpp and "float x0, z0, texel, y0, y1;" in cpp
