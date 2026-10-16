import re
import struct
from collections import Counter

import numpy as np
import pytest

from sandover import pat_census as pc
from sandover.paths import root
from sandover.sand_textures import PAT_MATERIALS, load_collide_pat


def tri(*pts):
    return np.array(pts, dtype=np.float64)


def test_pat_enum_matches_goal_source():
    src = (root() / "goal_src/jak1/engine/collide/pat-h.gc").read_text(encoding="utf-8")
    body = src[src.index("(defenum pat-material"):src.index("(defenum pat-mode")]
    assert re.findall(r"^\s+\((\w+)\)", body, re.M) == PAT_MATERIALS
    assert set(pc.PAT_CLASSES) == set(PAT_MATERIALS)


def test_point_tri_dist_inside_and_edge():
    a, b, c = tri(0, 0, 0), tri(2, 0, 0), tri(0, 0, 2)
    assert pc.point_tri_dist(tri(0.5, 0.3, 0.5), a, b, c) == pytest.approx(0.3)
    assert pc.point_tri_dist(tri(-1, 0, 0.5), a, b, c) == pytest.approx(1.0)
    assert pc.point_tri_dist(tri(2, 0, 2), a, b, c) == pytest.approx(np.sqrt(2))


def test_match_prefers_near_same_facing_surface():
    floor = tri([0, 0, 0], [0, 0, 4], [4, 0, 0])
    roof = tri([0, 5, 0], [0, 5, 4], [4, 5, 0])
    wall = tri([0, 0, 1], [0, 3, 1], [4, 0, 1])  # vertical, 1 m from the visible floor
    col = np.stack([roof, wall, floor])
    vis = np.stack([
        tri([0.5, 0.1, 0.5], [1.5, 0.1, 0.5], [0.5, 0.1, 1.5]),   # just above the floor
        tri([0.5, 2.0, 0.5], [1.5, 2.0, 0.5], [0.5, 2.0, 1.5]),   # nothing within range
        tri([1, 0, 1.2], [1, 2, 1.2], [2, 0, 1.2]),               # wall face 0.2 m off the wall
    ])
    assert pc.match_tris(vis, col, max_dist=0.5).tolist() == [2, -1, 1]


def test_ground_mask_either_winding():
    n = np.array([[0, 1, 0], [0, -1, 0], [1, 0, 0], [0.6, 0.8, 0]])
    assert pc.ground_mask(n).tolist() == [True, True, False, True]


def _row(name, tpage, cls, levels):
    return {"combo_id": "0x0", "tpage": tpage, "name": name, "class": cls, "levels": levels,
            "tags": ""}


def test_join_row_methods():
    by = pc.table_index([
        _row("a", "jungleb-vis-tfrag", "metal", "JUB.DGO"),
        _row("a", "citadel-vis-tfrag", "stone", "CIT.DGO"),
        _row("b", "x", "wood", "JUN.DGO"),
        _row("c", "beach-vis-shrub", "wood", "BEA.DGO"),
        _row("c", "beach-vis-tfrag", "stone", "BEA.DGO"),
        _row("d", "beach-vis-shrub", "foliage", "BEA.DGO"),
        _row("d", "beach-vis-pris", "foliage", "BEA.DGO"),
    ])
    assert pc.join_row(by, "a-envmap", "jungleb")[0]["class"] == "metal"
    assert pc.join_row(by, "a-envmap", "jungleb")[1:] == ("a", "name+level")
    assert pc.join_row(by, "b", "snow")[2] == "name"
    row, _, how = pc.join_row(by, "c", "beach")
    assert (row["tpage"], how) == ("beach-vis-tfrag", "conflict")
    assert pc.join_row(by, "d", "beach")[2] == "same-class"
    assert pc.join_row(by, "zz", "beach") == (None, "zz", "none")


def test_verdicts():
    assert pc.verdict("metal", "pcmetal") == "agree"
    assert pc.verdict("stone", "pcmetal") == "disagree"
    assert pc.verdict("metal", "stone") == "disagree-stone"
    assert pc.verdict("stone", "swamp") == "no-class"
    assert pc.verdict("stone", None) == "no-match"
    assert pc.verdict("", "stone") == "unjoined"


def test_build_rows_merges_envmap_variant_and_reports():
    rec = lambda pats: {"tris": sum(pats.values()), "matched": sum(pats.values()),  # noqa: E731
                        "pat_area": Counter({k: float(v) for k, v in pats.items()}),
                        "pat_tris": Counter(pats)}
    per_level = {"jungleb": {"t": rec({"pcmetal": 3}), "t-envmap": rec({"pcmetal": 5, "stone": 2})}}
    rows = pc.build_rows(per_level, [_row("t", "jungleb-vis-tfrag", "stone", "JUB.DGO")])
    assert len(rows) == 1
    r = rows[0]
    assert (r["pat_material"], r["share"], r["samples"], r["agreement"]) == \
        ("pcmetal", "0.800", 8, "disagree")
    text = pc.render_report(rows, per_level, {"max_dist": 0.5, "min_cos": 0.7})
    assert "| t | jungleb | stone | pcmetal | 0.800 |" in text


def _fr3(tmp_path, verts_pat):
    import zstandard

    rec = b""
    for (x, y, z), pat in verts_pat:
        rec += struct.pack("<3fI3hHII", x * 4096, y * 4096, z * 4096, 0, 0, 32767, 0, 0, pat, 0)
    body = b"\x07" * 13 + struct.pack("<Q", len(verts_pat)) + rec + b"\x01" * 9
    p = tmp_path / "t.fr3"
    p.write_bytes(struct.pack("<Q", len(body)) + zstandard.ZstdCompressor().compress(body))
    return p


def test_load_collide_pat_decodes_material_bits(tmp_path):
    pat = (8 << 6) | (0 << 3)  # pcmetal, ground
    p = _fr3(tmp_path, [((0, 0, 0), pat), ((1, 0, 0), pat), ((0, 0, 1), pat)])
    tris, pats = load_collide_pat(p)
    assert tris.shape == (1, 3, 3)
    assert tris[0, 1].tolist() == [1, 0, 0]
    assert PAT_MATERIALS[(int(pats[0]) >> 6) & 0x3F] == "pcmetal"
