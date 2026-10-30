import pytest

from sandover import mood_tables


def test_village1_hour_12_snapshot_and_morph():
    data = mood_tables.extract("village1")
    snapshot1, snapshot2, morph = mood_tables.evaluate_palette_hour(data["palette_interp"], 12)
    assert (snapshot1, snapshot2, morph) == (2, 3, 0.0)
    # morph-end 0.33 is only reached as the hour runs out
    assert mood_tables.evaluate_palette_hour(data["palette_interp"], 12.5)[2] == pytest.approx(0.165)


def test_quick_levels_have_no_hour_table():
    for level in mood_tables.QUICK_LEVELS:
        data = mood_tables.extract(level)
        assert data["mode"] == "quick"
        assert data["palette_interp"] is None
        assert data["quick_slot"] == mood_tables.QUICK_SLOT


def test_village3_flags_indirect_binding():
    data = mood_tables.extract("village3")
    assert data["indirect_binding"] is True
    assert data["palette_interp"] is not None


def test_engine_table_is_current():
    # gk reads NeighboursMood.inc; regenerate with `sandover-mood-tables` if this fails
    assert mood_tables.CPP_TABLE.read_text() == mood_tables.cpp_table()
