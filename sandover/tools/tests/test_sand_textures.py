from collections import Counter

from sandover import sand_textures as st
from sandover.paths import root


def test_is_sand_share_and_area():
    assert st.is_sand(Counter(sand=900.0, stone=50.0, none=4000.0))
    assert not st.is_sand(Counter(sand=600.0, stone=400.0))
    assert not st.is_sand(Counter(sand=40.0))


def test_same_art_by_derived_statistics():
    a = {"width": 512, "height": 498, "rough_mean": 0.879, "slope_p50_deg": 1.73, "slope_p95_deg": 4.09}
    index = {"t/x": {**a, "name": "x"}, "t/y": {**a, "name": "y"},
             "t/z": {**a, "rough_mean": 0.94, "name": "z"}}
    assert st.same_art(index, {"x"}) == {"x", "y"}


def test_committed_table():
    text = (root() / st.CPP_TABLE_REL).read_text(encoding="utf-8")
    names = {line.strip().strip(",").strip('"') for line in text.splitlines() if line.startswith('"')}
    assert {"bch-beach-01", "vil-beach-01", "vil2-beach-01", "jng-beach-01"} <= names
    assert not names & {"bch-beachrock", "tra-beachrock", "vil-beachrock", "vil2-rockwall",
                        "vil-plankwood", "vil1-palmtree-trunk-01"}
