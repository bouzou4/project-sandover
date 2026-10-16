from pathlib import Path

import numpy as np

from sandover.materials import emissive_strengths as es
from sandover.materials import emitter_score as sc
from sandover.unbake import overrides, sidecar

PRESET = Path(__file__).resolve().parents[2] / "presets" / "emissive.toml"

SCORES = {"threshold": 0.5, "textures": {
    "lamp": {"score": 0.9},
    "wall": {"score": 0.1},
    "log": {"score": 0.2},
    "torch": {"score": 0.3, "anchor": "torch"},
    "eye": {"score": 0.2, "human": "emitter"},
    "rock": {"score": 0.4999},
}}


def test_receiver_scale_is_a_preset_knob_defaulting_to_zero():
    assert sc.load_params(PRESET).receiver_scale == 0.0


def test_receivers_are_scaled_emitters_anchors_and_human_emitters_are_not():
    f = sidecar.receiver_factors(SCORES, 0.25)
    assert f == {"wall": 0.25, "log": 0.25, "rock": 0.25}


def test_textures_with_a_human_row_are_left_to_it():
    assert "wall" not in sidecar.receiver_factors(SCORES, 0.0, keep={"wall"})


def test_human_ladder_factor_wins_over_receiver_scaling():
    f = sidecar.export_factors({"wall": 1.5}, sidecar.receiver_factors(SCORES, 0.0))
    assert f["wall"] == 1.5 and f["log"] == 0.0


def test_human_strength_names_only_counts_human_last_rows(tmp_path: Path):
    p = tmp_path / "s.csv"
    es.append_strength(p, {"name": "wall", "strength": 0.5, "source": "human"})
    es.append_strength(p, {"name": "log", "strength": 0.5, "source": "human"})
    es.append_strength(p, {"name": "log", "strength": 0.9, "source": "default"})
    assert sidecar.human_strength_names(p) == {"wall"}
    assert sidecar.human_strength_names(tmp_path / "missing.csv") == set()


def test_scaled_receiver_vertices_lose_e_shared_emitter_vertices_keep_it():
    t = sidecar.GlbTree("tfrag", 0, "x-tfrag-0", np.zeros((5, 3), np.float32), None,
                        prims=[("lamp", np.array([0, 1, 2])), ("wall", np.array([2, 3, 4]))])
    f = sidecar.vertex_scale(t, sidecar.receiver_factors(SCORES, 0.0))
    e = np.full(5, 0.8) * f
    assert e.tolist() == [0.8, 0.8, 0.8, 0.0, 0.0]


def test_override_after_scaling_wins():
    t_verts = {"wall": np.array([0, 1])}
    w = np.zeros(2, np.uint32)  # E scaled to 0 on the receiver
    overrides.apply({"wall": "emissive"}, w, t_verts)
    assert (((w >> 16) & 0xFF) == 255).all()


def test_scan_dir_prefers_the_raw_sidecars(tmp_path: Path):
    assert sidecar.scan_dir(tmp_path) == tmp_path
    (tmp_path / "raw").mkdir()
    assert sidecar.scan_dir(tmp_path) == tmp_path
    (tmp_path / "raw" / "beach.unbake").write_bytes(b"x")
    assert sidecar.scan_dir(tmp_path) == tmp_path / "raw"
