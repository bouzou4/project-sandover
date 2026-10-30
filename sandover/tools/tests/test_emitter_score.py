import json
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

from sandover.materials import emissive_strengths as es
from sandover.materials import emitter_score as sc

PRESET = sc.load_params(Path(__file__).resolve().parents[2] / "presets" / "emissive.toml")


def _rgba(luma: np.ndarray) -> np.ndarray:
    a = np.empty(luma.shape + (4,))
    a[..., :3] = luma[..., None]
    a[..., 3] = 128 / 255  # PS2 alpha 128 = opaque
    return a


def _lamp():
    """Bright panel in the middle of a dark frame; E only on the panel."""
    luma = np.full((16, 16), 0.15)
    luma[4:12, 4:12] = 0.95
    e = np.zeros((16, 16))
    e[4:12, 4:12] = 0.9
    return e, _rgba(luma)


def _wall():
    """Dim noisy texture under even E."""
    rng = np.random.default_rng(0)
    luma = 0.25 + 0.1 * rng.random((16, 16))
    return np.full((16, 16), 0.6), _rgba(luma)


def test_bright_correlated_mask_is_an_emitter():
    e, rgba = _lamp()
    c = sc.map_components(e, rgba)
    assert c["correlation"] > 0.99
    assert c["coverage"] == pytest.approx(0.25)
    assert c["brightness"] == pytest.approx(0.95)
    c.update(gradient=0.05, consistency=1.0, slot=PRESET.slot_prior["flames"])
    assert sc.combine(c, PRESET) >= PRESET.threshold


def test_uniform_mask_over_dim_texture_is_a_receiver():
    e, rgba = _wall()
    c = sc.map_components(e, rgba)
    assert c["correlation"] == 0.0  # flat E: no correlation
    assert c["coverage"] == 1.0
    c.update(gradient=0.8, consistency=0.05, slot=PRESET.slot_prior["night"])
    assert sc.combine(c, PRESET) < PRESET.threshold


def test_map_is_resampled_to_the_albedo_size():
    e, rgba = _lamp()
    big = np.kron(rgba, np.ones((2, 2, 1)))
    assert sc.map_components(e, big)["correlation"] > 0.99


def test_transparent_texels_are_ignored():
    e, rgba = _lamp()
    rgba[:2, :, 3] = 0.0
    e[:2, :] = 1.0  # E on invisible texels must not count
    assert sc.map_components(e, rgba)["correlation"] > 0.99


def test_draw_stats_falloff_and_lit_fraction():
    tri = np.array([[0.5, 0.5, 0.5],  # even: falloff 0
                    [0.8, 0.2, 0.0],  # falls off: 1
                    [0.05, 0.0, 0.0]])  # below covered: unlit
    n, lit, fall = sc.draw_stats(tri, 0.1)
    assert (n, lit) == (3, 2)
    assert fall == pytest.approx(1.0)
    g = sc.geometry_components({"levels": {"a": [n, lit], "b": [1, 0]}, "falloff": fall})
    assert g == {"gradient": 0.5, "consistency": 0.5}
    assert sc.geometry_components(None) == {"gradient": 0.5, "consistency": 0.5}


def test_combine_is_monotone_in_each_weight_sign():
    base = {k: 0.5 for k in sc.COMPONENTS}
    s0 = sc.combine(base, PRESET)
    for k, w in PRESET.weights.items():
        up = sc.combine({**base, k: 0.9}, PRESET)
        assert (up > s0) == (w > 0) or w == 0


def test_human_labels_and_weak_labels():
    latest = {"a": {"source": "human", "strength": str(es.LADDER_MIN)},
              "b": {"source": "human", "strength": "1.0"},
              "c": {"source": "default", "strength": "1.0"}}
    h = sc.human_labels({"d": "emissive", "a": "emissive"}, latest)
    assert h == {"a": "emitter", "b": "emitter", "d": "emitter"}
    assert sc.human_labels({}, latest)["a"] == "receiver"
    assert sc.weak_label("cv-torch-01") == "emitter"
    assert sc.weak_label("lava-flow-01") == "emitter"
    assert sc.weak_label("lava-metal-plate-01") == "receiver"  # level prefix, not lava
    assert sc.weak_label("bch-lamp-wood") == "receiver"
    assert sc.weak_label("sage-thing") is None


def _scores(**kv):
    return {n: {"score": s} for n, s in kv.items()}


def test_precision_recall_and_keep_threshold():
    s = _scores(lamp=0.9, torch=0.62, wall=0.7, log=0.1)
    labels = {"lamp": "emitter", "torch": "emitter", "wall": "receiver", "log": "receiver"}
    pr = sc.precision_recall(s, labels, 0.5)
    assert (pr["tp"], pr["fp"], pr["fn"], pr["tn"]) == (2, 1, 0, 1)
    assert pr["precision"] == pytest.approx(2 / 3, abs=1e-3) and pr["recall"] == 1.0
    assert sc.keep_threshold(s, {"torch": "emitter"}, 0.5) == 0.5
    assert sc.keep_threshold(s, {"torch": "emitter"}, 0.8) == 0.62
    assert sc.keep_threshold(s, {}, 0.5) == 0.5


def test_score_all_and_scored_queue(tmp_path):
    maps, tex = tmp_path / "maps", tmp_path / "tex"
    for name, (e, rgba) in {"lamp": _lamp(), "wall": _wall()}.items():
        (maps / "tp").mkdir(parents=True, exist_ok=True)
        (tex / "tp").mkdir(parents=True, exist_ok=True)
        Image.fromarray((e * 255).astype(np.uint8), "L").save(maps / "tp" / f"{name}.png")
        Image.fromarray((rgba * 255).astype(np.uint8), "RGBA").save(tex / "tp" / f"{name}.png")
    index = {"textures": {
        "lamp": {"tpage": "tp", "role": "flames", "source": "bake", "coverage": 0.25, "default_strength": 0.8},
        "wall": {"tpage": "tp", "role": "night", "source": "bake", "coverage": 1.0, "default_strength": 0.9}}}
    draws = {"lamp": {"levels": {"x": [10, 10]}, "falloff": 0.5},
             "wall": {"levels": {"x": [100, 5]}, "falloff": 4.0}}
    out = sc.score_all(index, draws, PRESET, maps, tex, classes=({}, {"wall": "stone"}))
    assert out["lamp"]["emitter"] and not out["wall"]["emitter"]
    assert out["wall"]["ladder_default"] == PRESET.receiver_default
    assert out["lamp"]["ladder_default"] == 0.8
    assert out["wall"]["class"] == "stone"
    assert sc.class_breakdown(out, PRESET.threshold)["stone"] == [1, 1]
    json.dumps(out)

    queue = [{"name": "wall", "default": 0.9}, {"name": "other", "default": 0.5},
             {"name": "lamp", "default": 0.8}]
    q = es.scored_queue(queue, {"threshold": PRESET.threshold, "textures": out})
    assert [r["name"] for r in q] == ["lamp", "other", "wall"]
    assert q[2]["default"] == es.LADDER_MIN and q[2]["bake_default"] == 0.9
    assert q[0]["default"] == 0.8 and q[1]["score"] is None
    out["wall"]["human"] = "emitter"
    q = es.scored_queue(queue, {"threshold": PRESET.threshold, "textures": out})
    assert q[1]["name"] == "wall" and q[1]["default"] == 0.9
