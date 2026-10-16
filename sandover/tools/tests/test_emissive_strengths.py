from pathlib import Path

import numpy as np
import pytest

from sandover.materials import emissive_strengths as es
from sandover.materials.emissive_review import build_queue, current_strength, ladder_action
from sandover.unbake import sidecar


def test_rail_pos_is_log_and_round_trips():
    assert es.rail_pos(es.LADDER_MIN) == 0.0
    assert es.rail_pos(es.LADDER_MAX) == pytest.approx(1.0)
    # equal ratios are equal distances
    assert es.rail_pos(2.0) - es.rail_pos(1.0) == pytest.approx(es.rail_pos(0.5) - es.rail_pos(0.25))
    for s in (0.03, 0.5, 1.0, 7.0):
        assert es.rail_strength(es.rail_pos(s)) == pytest.approx(s)
    assert es.rail_pos(1e-9) == 0.0 and es.rail_pos(1e9) == pytest.approx(1.0)


def test_nudge_is_a_quarter_octave_and_clamps():
    assert es.nudge(1.0, 4) == pytest.approx(2.0)
    assert es.nudge(1.0, -1) == pytest.approx(2 ** -0.25)
    assert es.nudge(es.LADDER_MAX, 1) == es.LADDER_MAX
    assert es.nudge(es.LADDER_MIN, -3) == es.LADDER_MIN


def test_neighbours_nearest_below_and_above():
    s = {"a": 0.1, "b": 0.5, "c": 0.9, "me": 1.0, "d": 1.1, "e": 3.0, "f": 8.0}
    assert [n for n, _ in es.neighbours(s, "me", 1.0, k=2)] == ["b", "c", "d", "e"]


def test_append_strength_writes_header_once_and_last_row_wins(tmp_path: Path):
    p = tmp_path / "emissive-strengths.csv"
    es.append_strength(p, {"combo_id": "0x1", "tpage": "t", "name": "lamp", "strength": 0.5, "source": "default"})
    es.append_strength(p, {"combo_id": "0x1", "tpage": "t", "name": "lamp", "strength": 0.707,
                           "source": "human", "note": "brighter"})
    es.append_strength(p, {"name": "eco", "strength": 2, "source": "human"})
    lines = p.read_text(encoding="utf-8").splitlines()
    assert lines[0] == ",".join(es.FIELDS)
    assert len(lines) == 4
    latest = es.latest_strengths(es.read_strengths(p))
    assert latest["lamp"]["strength"] == "0.707" and latest["lamp"]["source"] == "human"
    assert latest["lamp"]["note"] == "brighter" and latest["lamp"]["timestamp"]
    assert latest["eco"]["strength"] == "2"


@pytest.mark.parametrize("row", [{"name": "a", "strength": 1, "source": "judge"},
                                 {"name": "a", "strength": 0, "source": "human"},
                                 {"name": "a", "strength": "nan", "source": "human"}])
def test_append_strength_rejects_bad_rows(tmp_path: Path, row):
    with pytest.raises(ValueError):
        es.append_strength(tmp_path / "s.csv", row)


def test_ladder_actions():
    row = {"name": "lamp", "default": 0.5}
    assert ladder_action(row, 0.5, "up") == (pytest.approx(0.5 * 2 ** 0.25), "human", 0)
    assert ladder_action(row, 0.5, "down")[1:] == ("human", 0)
    assert ladder_action(row, 3.0, "reset") == (0.5, "default", 0)
    assert ladder_action(row, 0.5, "accept") == (0.5, "default", 1)
    assert ladder_action(row, 0.7, "accept") == (0.7, "human", 1)
    assert ladder_action(row, 0.7, "set", 2.0) == (2.0, "human", 0)
    assert ladder_action(row, 0.7, "set", 1e6)[0] == es.LADDER_MAX
    assert ladder_action(row, 0.7, "skip") == (0.7, None, 1)
    assert ladder_action(row, 0.7, "back") == (0.7, None, -1)


def test_build_queue_orders_by_default_and_applies_overrides():
    index = {"textures": {
        "lamp": {"tpage": "t", "source": "bake", "coverage": 0.5, "default_strength": 0.5},
        "torch": {"tpage": "t", "source": "bake", "coverage": 0.2, "default_strength": 1.0},
        "rock": {"tpage": "t", "source": "bake", "coverage": 0.9, "default_strength": 0.3},
        "sign": {"tpage": "u", "source": "luminance", "coverage": 0.1, "default_strength": 0.2},
    }}
    q = build_queue(index, {"rock": "not-emissive"}, [{"tpage": "t", "name": "lamp", "combo_id": "0x9"}])
    assert [r["name"] for r in q] == ["torch", "lamp", "sign"]
    assert q[1]["combo_id"] == "0x9"
    latest = {"lamp": {"strength": "0.8", "source": "human"}}
    assert current_strength(q[1], latest) == pytest.approx(0.8)
    assert current_strength(q[0], latest) == pytest.approx(1.0)


def test_export_sidecar_factors_from_the_csv(tmp_path: Path):
    csv_p = tmp_path / "s.csv"
    es.append_strength(csv_p, {"name": "lamp", "strength": 1.0, "source": "human"})
    es.append_strength(csv_p, {"name": "torch", "strength": 1.0, "source": "default"})
    es.append_strength(csv_p, {"name": "gone", "strength": 1.0, "source": "human"})
    idx = tmp_path / "index.json"
    idx.write_text('{"textures": {"lamp": {"default_strength": 0.5}, "torch": {"default_strength": 1.0}}}')
    assert sidecar.load_e_factors(csv_p, idx) == {"lamp": pytest.approx(2.0)}
    assert sidecar.load_e_factors(tmp_path / "missing.csv", idx) == {}


def test_vertex_scale_takes_the_max_factor_per_vertex():
    t = sidecar.GlbTree("tfrag", 0, "x-tfrag-0", np.zeros((5, 3), np.float32), None,
                        prims=[("lamp", np.array([0, 1, 2])), ("wall", np.array([2, 3]))])
    assert sidecar.vertex_scale(t, {"lamp": 0.5}).tolist() == [0.5, 0.5, 1.0, 1.0, 1.0]
    assert sidecar.vertex_scale(t, {"lamp": 3.0}).tolist() == [3.0, 3.0, 3.0, 1.0, 1.0]
    assert sidecar.vertex_scale(t, {"other": 2.0}) is None
