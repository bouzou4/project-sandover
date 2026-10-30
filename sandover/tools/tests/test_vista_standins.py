"""kStandins (Neighbours.cpp) against the stand-in census ($SANDOVER_DATA/vista/standins.csv)."""
import csv
import os
import re
from pathlib import Path

import pytest

from sandover import presets

# census rows under the threshold that kStandins leaves out on purpose, with the reason
KNOWN_UNLISTED = {
    ("village1", "vil1-medres-wood"): "village2 bridge wood; not reviewed as a hide candidate yet",
}


def _engine_table():
    src = (presets.root() / "game/graphics/opengl_renderer/Neighbours.cpp").read_text(
        encoding="utf-8")
    table = src[src.index("kStandins = {"):]
    table = table[:table.index("};")]
    rows = re.findall(r'\{"(\w+)", "([\w-]+)", \{([^}]*)\}, ([0-9.]+)f\}', table)
    threshold = float(re.search(r"kStandinOnGeometryM = ([0-9.]+)f", src).group(1))
    return [(h, n, re.findall(r'"(\w+)"', d), float(m)) for h, n, d, m in rows], threshold


def _census():
    data = os.environ.get("SANDOVER_DATA")
    path = Path(data) / "vista" / "standins.csv" if data else None
    if not path or not path.exists():
        pytest.skip("no stand-in census under SANDOVER_DATA")
    with path.open(newline="") as f:
        return [r for r in csv.DictReader(f) if r["median_dist_to_real_m"]]


def test_standin_distances_match_census():
    rows, threshold = _engine_table()
    assert rows, "kStandins not parsed"
    census = _census()
    for host, name, depicts, dist in rows:
        parts = [r for r in census if r["host"] == host and r["name"].startswith(name)]
        assert parts, f"{host} {name}: not in the census"
        worst = max(float(r["median_dist_to_real_m"]) for r in parts)
        assert abs(worst - dist) < 0.05, f"{host} {name}: table {dist} m, census {worst} m"
        if dist < threshold:
            for r in parts:
                assert r["real_level_at_median"] in depicts, f"{host} {r['name']}"


def test_no_unlisted_census_row_under_threshold():
    rows, threshold = _engine_table()
    hosts = {h for h, *_ in rows}
    unlisted = []
    for r in _census():
        if r["host"] not in hosts or float(r["median_dist_to_real_m"]) >= threshold:
            continue
        listed = any(r["host"] == h and r["name"].startswith(n) for h, n, *_ in rows)
        if not listed and (r["host"], r["name"]) not in KNOWN_UNLISTED:
            unlisted.append((r["host"], r["name"], r["median_dist_to_real_m"]))
    assert not unlisted
