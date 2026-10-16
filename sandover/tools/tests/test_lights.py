import json

import numpy as np

from sandover.lights import census as C
from sandover.lights.cli import write_summary
from sandover.lights.particles import parse_text
from sandover.lights.statics import cluster_points, texture_colour

GOAL = """
(defpartgroup group-test-lamp
  :id 1
  :bounds (static-bspherem 0 2 0 3)
  :parts
  ((sp-item 10 :fade-after (meters 60) :hour-mask #b111111100000000000111111)
   (sp-item 11 :flags (start-dead))))

(defpartgroup group-test-smoke
  :id 2
  :bounds (static-bspherem 0 0 0 8)
  :parts ((sp-item 12)))

(defpart 10
  :init-specs
  ((:texture (hotdot effects))
   (:r 255.0)
   (:g 128.0 64.0)
   (:b :copy g)
   (:a 64.0)
   (:flags (bit2 bit3))))

(defpart 11
  :init-specs ((:accel-y (meters 0))))

(defpart 12
  :init-specs
  ((:texture (bigpuff effects))
   (:r 128.0)
   (:g 128.0)
   (:b 128.0)
   (:a 32.0)
   (:flags (bit2))))
"""

TABLE = {
    "types": {
        "ecovent": {"emits": True, "colour": [0, 0, 1], "colour_from": "eco-info", "radius_hint_m": 4,
                    "state": {"kind": "entity-flag", "var": "vent blocked until alt-actor task closes"},
                    "source": "x.gc:1"},
        "fuel-cell": {"emits": True, "colour": [1, 1, 0.5], "radius_hint_m": 2, "moves": "small"},
        "crate": {"emits": False},
        "babak": {"emits": False},
    },
    "pickup_emit": {"2": {"colour": [1, 0, 0], "radius_hint_m": 1.5}, "5": {"colour": [1, 0.8, 0.4]}},
}
ACTORS = [
    {"etype": "ecovent", "aid": 1, "trans": [1, 2, 3, 0], "lump": {"name": "ecovent-1", "eco-info": [2, 5]}},
    {"etype": "fuel-cell", "aid": 2, "trans": [4, 5, 6, 0], "lump": {"name": "fuel-cell-1", "eco-info": [6, 4]}},
    {"etype": "crate", "aid": 3, "trans": [7, 8, 9, 0], "lump": {"name": "crate-1", "eco-info": [5, 3]}},
    {"etype": "crate", "aid": 4, "trans": [0, 0, 0, 0], "lump": {"name": "crate-2", "crate-type": "'darkeco"}},
    {"etype": "crate", "aid": 5, "trans": [0, 0, 0, 0], "lump": {"name": "crate-3", "eco-info": [8, (2 << 16) | 4]}},
    {"etype": "babak", "aid": 6, "trans": [0, 0, 0, 0], "lump": {"name": "babak-1"}},
    {"etype": "mystery", "aid": 7, "trans": [0, 0, 0, 0], "lump": {"name": "mystery-1"}},
    {"etype": "test-part", "aid": 8, "trans": [10, 0, 10, 0], "lump": {"name": "test-part-1", "art-name": "group-test-lamp"}},
    {"etype": "test-part", "aid": 9, "trans": [0, 0, 0, 0], "lump": {"name": "test-part-2", "art-name": "group-test-smoke"}},
    {"etype": "test-part", "aid": 10, "trans": [0, 0, 0, 0], "lump": {"name": "test-part-3", "art-name": ["group-nope", "x"]}},
]
TASKS = {4: "jungle-tower"}


def _groups():
    groups, parts = parse_text(GOAL, "goal_src/jak1/levels/test.gc")
    for g in groups.values():
        g.parts = [parts[i] for i in dict.fromkeys(g.items) if i in parts]
    return groups


def test_parse_groups_and_parts():
    g = _groups()
    lamp, smoke = g["group-test-lamp"], g["group-test-smoke"]
    assert lamp.items == [10, 11] and lamp.radius_m == 3.0
    assert lamp.source == "goal_src/jak1/levels/test.gc:2"
    assert lamp.emits and not smoke.emits
    assert lamp.colour() == [1.0, 1.0, 1.0]  # 255 clips; g = 128 + 64/2 = 160 clips; b copies g
    assert lamp.hour_mask() == "111111100000000000111111"
    assert abs(lamp.strength() - 255 / 128 * 0.5) < 1e-6
    assert C.hours(lamp.hour_mask()) == "0,1,2,3,4,5,17,18,19,20,21,22,23"


def test_entity_lights_states_and_crates():
    recs, notes = C.entity_lights("test", ACTORS, TABLE, TASKS, "t.json")
    by = {r["id"]: r for r in recs}
    assert set(by) == {"test:ecovent-1", "test:fuel-cell-1", "test:crate-1"}  # buzzer not in pickup_emit
    vent = by["test:ecovent-1"]
    assert vent["colour"] == [1.0, 0.0, 0.0] and vent["pos"] == [1.0, 2.0, 3.0]
    assert vent["state"]["kind"] == "always" and vent["source"] == "t.json#0"  # no gate lumps
    cell = by["test:fuel-cell-1"]
    assert cell["state"] == {"kind": "task", "var": "jungle-tower closed (cell collected)", "task": "jungle-tower"}
    crate = by["test:crate-1"]
    assert crate["type"] == "crate/money" and crate["contents"] == {"pickup": "money", "amount": 3}
    assert "crate breaks" in crate["state"]["var"]
    assert notes["mystery (unclassified)"] == 1


def test_buzzer_state():
    s = C.task_state(8, (2 << 16) | 4, TASKS)
    assert s["kind"] == "task" and s["task"] == "jungle-tower" and "scout fly 2" in s["var"]


def test_particle_lights():
    recs, notes = C.particle_lights("test", ACTORS, _groups(), "t.json")
    assert [r["id"] for r in recs] == ["test:test-part-1"]
    r = recs[0]
    assert r["kind"] == "particle" and r["state"]["kind"] == "time-of-day" and r["radius_hint_m"] == 3.0
    assert notes["no additive light parts"] == 1 and notes["group-nope (group not in goal_src)"] == 1


def test_cluster_points_splits_far_groups_and_long_strips():
    a = np.array([[0, 0, 0], [0.5, 0, 0], [50, 0, 0]], float)
    out = cluster_points(a, np.ones(3))
    assert sorted(len(m) for *_, m in out) == [1, 2]
    strip = np.stack([np.arange(0, 20, 0.5), np.zeros(40), np.zeros(40)], 1)
    parts = cluster_points(strip, np.ones(40), split_m=8.0)
    assert len(parts) == 3 and all(ext <= 8.0 for _, ext, _, _ in parts)
    ctr, *_ = cluster_points(np.array([[0, 0, 0], [2, 0, 0]], float), np.array([1.0, 3.0]), cell_m=4.0)[0]
    assert np.allclose(ctr, [1.5, 0, 0])


def test_texture_colour_uses_emissive_texels():
    rgba = np.zeros((2, 2, 4))
    rgba[..., 3] = 0.5
    rgba[0, 0, :3] = [1, 0.5, 0]
    emap = np.zeros((2, 2))
    emap[0, 0] = 0.8
    assert texture_colour(emap, rgba) == [1.0, 0.5, 0.0]
    assert texture_colour(None, None) is None


def test_summary(tmp_path):
    recs, _ = C.entity_lights("test", ACTORS, TABLE, TASKS, "t.json")
    p = write_summary(tmp_path, {"test": recs}, {})
    text = p.read_text(encoding="utf-8")
    assert "| test | 3 | 0 | 0 | 0 | 0 | 3 | 1 / 1 / 1 / 0 / 0 / 0 |" in text
    json.dumps(recs)


def test_vent_states():
    names = {7: "eggtop-1"}
    assert C.vent_state({"options": 5}, 3, names)["task"] == "jungle-eggtop"
    s = C.vent_state({"alt-actor": 7}, 2, names)
    assert s["kind"] == "entity-flag" and s["entity"] == "eggtop-1"
    assert C.vent_state({"options": 1}, 1, names)["kind"] == "entity-flag"
    assert C.vent_state({}, 3, names)["kind"] == "always"


def test_plain_crate_drops_green_pill():
    table = {"types": {"crate": {"emits": False}}, "pickup_emit": {"9": {"colour": [0.5, 1, 0.1]}}}
    actors = [{"etype": "crate", "aid": 1, "trans": [0, 0, 0, 0], "lump": {"name": "crate-9"}},
              {"etype": "crate", "aid": 2, "trans": [0, 0, 0, 0], "lump": {"name": "b", "crate-type": "'bucket"}}]
    recs, _ = C.entity_lights("t", actors, table, {}, "t.json")
    assert [r["type"] for r in recs] == ["crate/eco-pill-random"]


def test_dark_types_from_the_table():
    from sandover.lights import bake as B

    dark = B.dark_types()
    assert {"money", "crate/money"} <= dark
    assert not {"fuel-cell", "crate/fuel-cell", "eco"} & dark
    table = {"types": {"money": {"emits": False}, "crate": {"contents_emit": {"5": {"emits": False}}}},
             "pickup_colours": {}}
    actors = [{"etype": "eco", "aid": 1, "trans": [0, 0, 0, 0], "lump": {"name": "e", "eco-info": [5, 1]}}]
    table["types"]["eco"] = {"emits": True, "colour_from": "eco-info", "colour": [0, 0, 1]}
    recs, _ = C.entity_lights("t", actors, table, {}, "t.json")
    assert recs == []


def _rec(lid, kind, typ, **kw):
    r = {"id": lid, "kind": kind, "type": typ, "pos": [1.0, 2.0, 3.0], "colour": [1.0, 0.5, 0.0],
         "radius_hint_m": 4.0, "state": {"kind": "always", "var": ""}, "level": "lv"}
    r.update(kw)
    return r


def test_bake_level_filters_and_gates():
    from sandover.lights import bake as B

    recs = [
        _rec("lv:vent", "entity", "ecovent", aid=7),
        _rec("lv:money", "entity", "money", aid=8),
        _rec("lv:cell", "entity", "fuel-cell", aid=14),
        _rec("lv:orbcrate", "entity", "crate/money", aid=15),
        _rec("lv:crate", "entity", "crate/eco-blue", aid=9),
        _rec("lv:pill", "entity", "crate/eco-pill-random", aid=10),
        _rec("lv:plat", "entity", "plat", maybe=True),
        _rec("lv:crystal", "entity", "cavecrystal", aid=11, radius_hint_m=70.0),
        _rec("lv:sage", "entity", "red-sagecage", aid=12),
        _rec("lv:kit", "vanilla-light", "kit 1"),
        _rec("lv:moth", "particle", "group-moth", textures=["hotdot"], intensity=0.5,
             state={"kind": "time-of-day", "var": "off at hours 0,1,23"}),
        _rec("lv:arc", "particle", "group-spewer", textures=["lightning", "hotdot"], intensity=0.9),
        _rec("lv:empty", "particle", "group-x", textures=[], intensity=0.0),
        _rec("lv:s0", "static", "lamp", energy=0.5, role="night", e_source=9, radius_hint_m=3.0),
        _rec("lv:s1", "static", "lava", energy=3.0, role="lava", e_source=5),
        _rec("lv:s2", "static", "trim", energy=2.0, role="static"),
        _rec("lv:s3", "static", "zap", energy=9.0, role="lightning"),
        _rec("lv:s4", "static", "cave-wall", energy=8.0, role="fade-lights", e_source=3),
        _rec("lv:nan", "entity", "ecovent", aid=13, pos=[float("nan"), 0.0, 0.0]),
    ]
    rows, skipped = B.bake_level(recs, static_cap=3, energy_ref=1.0)
    by = {r["id"]: r for r in rows}
    assert by["lv:vent"]["gate"] == B.GATES["alive"] and by["lv:vent"]["cls"] == B.CLASSES["strong"]
    assert by["lv:vent"]["aid"] == 7
    # orbs glow but light nothing (design decision 2026-09-26); power cells stay strong
    assert by["lv:cell"]["cls"] == B.CLASSES["strong"] and by["lv:cell"]["gate"] == B.GATES["alive"]
    assert skipped["type gives no light (entity_types.json emits false)"] == 2
    assert by["lv:crate"]["gate"] == B.GATES["crate"]
    assert by["lv:crystal"]["gate"] == B.GATES["crystal"]
    assert by["lv:crystal"]["radius_m"] == B.DEFAULTS["radius_cap_m"]
    assert by["lv:sage"]["gate"] == B.GATES["task-done"]
    assert by["lv:moth"]["gate"] == B.GATES["hour"]
    assert by["lv:moth"]["param"] == (1 << 0) | (1 << 1) | (1 << 23)
    assert by["lv:moth"]["cls"] == B.CLASSES["particle"] and by["lv:moth"]["gain"] == 0.5
    assert by["lv:arc"]["cls"] == B.CLASSES["arc"]
    # statics: lamps get their own class, gate on their own E source, and the lamp radius floor
    assert by["lv:s0"]["cls"] == B.CLASSES["lamp"] and by["lv:s0"]["gate"] == B.GATES["slot"]
    assert by["lv:s0"]["param"] == 9 and by["lv:s0"]["radius_m"] == 7.0
    assert by["lv:s1"]["gate"] == B.GATES["slot"] and by["lv:s1"]["param"] == 5
    assert by["lv:s1"]["cls"] == B.CLASSES["strong"] and by["lv:s1"]["gain"] == 1.0
    assert by["lv:s2"]["gate"] == B.GATES["always"]
    # crystal-lit cave walls and lightning are receivers or arcs, not lights
    for gone in ("lv:money", "lv:orbcrate", "lv:pill", "lv:plat", "lv:kit", "lv:empty", "lv:s3", "lv:s4", "lv:nan"):
        assert gone not in by
    assert skipped["maybe"] == 1 and skipped["non-finite value"] == 1
    assert skipped["static on a fade-lights slot"] == 1


def test_bake_lava_has_its_own_cap():
    from sandover.lights import bake as B

    recs = [_rec(f"lv:lava{i}", "static", "lava", energy=5.0 - i, role="lava", e_source=5)
            for i in range(3)]
    recs += [_rec(f"lv:trim{i}", "static", "trim", energy=9.0 - i, role="static") for i in range(2)]
    rows, skipped = B.bake_level(recs, static_cap=1, energy_ref=1.0, cfg={"lava_cap": 2})
    ids = {r["id"] for r in rows}
    assert ids == {"lv:lava0", "lv:lava1", "lv:trim0"}
    assert skipped["lava beyond the per-level cap"] == 1
    assert skipped["static beyond the per-level cap"] == 1


def test_bake_lamps_have_their_own_cap():
    from sandover.lights import bake as B

    # a level's big lit floors (flames role) must not crowd its lamps (night role) out: village2
    recs = [_rec(f"lv:floor{i}", "static", "floor", energy=50.0 - i, role="flames", e_source=5)
            for i in range(3)]
    recs += [_rec(f"lv:lamp{i}", "static", "lamp", energy=1.0, role="night", e_source=9) for i in range(3)]
    rows, skipped = B.bake_level(recs, static_cap=2, energy_ref=1.0, cfg={"night_cap": 2})
    ids = {r["id"] for r in rows}
    assert ids == {"lv:floor0", "lv:floor1", "lv:lamp0", "lv:lamp1"}
    assert skipped["night beyond the per-level cap"] == 1


def test_bake_lamp_glass_has_its_own_cap():
    from sandover.lights import bake as B

    # citadel: 251 cit-glass-01 shield lamps fell under the shared static cap, so only tiles lit them
    recs = [_rec(f"lv:trim{i}", "static", "cit-precursor-metal-edging-01-hitweak", energy=90.0 - i,
                 role="light") for i in range(3)]
    recs += [_rec(f"lv:glass{i}", "static", "cit-glass-01", energy=12.0, role="light") for i in range(3)]
    rows, skipped = B.bake_level(recs, static_cap=2, energy_ref=1.0, cfg={"glass_cap": 3})
    ids = {r["id"] for r in rows}
    assert ids == {"lv:trim0", "lv:trim1", "lv:glass0", "lv:glass1", "lv:glass2"}
    assert skipped["static beyond the per-level cap"] == 1
    assert B.Baker.pool({"type": "rol-light", "role": "light"}) == "named"
    assert B.Baker.pool({"type": "cit-glass-01", "role": "light"}) == "glass"
    # other levels' glass is not lamp glass: it keeps its static class and actor light
    for tex in ("jng-precursor-glass-b-01", "cmn-precursor-glass-b-02", "rol-glass", "rol-glass-s"):
        assert B.Baker.pool({"type": tex, "role": "light"}) == "static", tex


def test_bake_snow_bumper_field_takes_over_its_ring():
    from sandover.lights import bake as B

    # the field's sprites stand on a 5 m ring, 1.5 m up (snow-bumper.gc part 1893): the row sits
    # at the field's height and reaches past the ring, so it takes over the sprites' tiles
    rows, _ = B.bake_level([_rec("lv:bump", "entity", "snow-bumper", aid=7)], static_cap=4,
                           energy_ref=1.0)
    r = rows[0]
    assert r["radius_m"] >= (5.0 ** 2 + 1.5 ** 2) ** 0.5 + 1.0
    assert abs(r["pos"][1] - 3.5) < 1e-6


def test_bake_promoted_type_keeps_its_maybe_rows_with_its_override():
    from sandover.lights import bake as B

    recs = [_rec("lv:hutlamp", "entity", "hutlamp", aid=5, maybe=True),
            _rec("lv:plat", "entity", "plat", aid=6, maybe=True)]
    cfg = {"bright_types": ["hutlamp"],
           "bake_overrides": {"hutlamp": {"class": "lamp", "gate": "slot", "param": 5}}}
    rows, skipped = B.bake_level(recs, static_cap=4, energy_ref=1.0, cfg=cfg)
    assert [r["id"] for r in rows] == ["lv:hutlamp"]
    r = rows[0]
    assert (r["cls"], r["gate"], r["param"], r["aid"]) == (B.CLASSES["lamp"], B.GATES["slot"], 5, 5)
    assert r["radius_m"] == 7.0 and skipped["maybe"] == 1
    # the shipped table: village1's hut lamps light while the flames slot (5) has weight (#237)
    bright, over = B.entity_bake()
    assert "hutlamp" in bright and over["hutlamp"] == {"class": "lamp", "gate": "slot", "param": 5}


def test_bake_writes_cpp_table_and_csv(tmp_path):
    import csv

    from sandover.lights import bake as B

    d = tmp_path / "lights"
    d.mkdir()
    (d / "lv.json").write_text(json.dumps([_rec("lv:vent", "entity", "ecovent", aid=7),
                                           _rec("lv:s0", "static", "lamp", energy=0.5, role="night")]),
                               encoding="utf-8")
    (d / "summary.json").write_text("{}", encoding="utf-8")
    inc, out_csv = d / "WorldLights.inc", d / "world-lights.csv"
    stats = B.bake(d, inc, cfg={"static_cap": 4}, csv_out=out_csv)
    assert stats["lv"]["rows"] == 2
    lines = inc.read_text(encoding="utf-8").splitlines()
    assert lines[:3] == B.INC_HEADER
    assert lines[3] == '{"lv", 1.f, 2.f, 3.f, 1.f, 0.5f, 0.f, 4.f, 1.f, 0, 10, 0u, 7u},'
    assert lines[4] == '{"lv", 1.f, 2.f, 3.f, 1.f, 0.5f, 0.f, 7.f, 0.5f, 4, 2, 9u, 0u},'
    rows = list(csv.DictReader(out_csv.open(encoding="utf-8")))
    assert list(rows[0]) == B.FIELDS and rows[1]["param"] == "9"


def test_bake_props_and_dark_crystal_override():
    from sandover.lights import bake as B

    recs = [_rec(f"lv:prop:ice:{i}", "prop", "ice", gain=0.5) for i in range(3)]
    recs.append(_rec("lv:dc", "entity", "dark-crystal", aid=4, maybe=True))
    recs.append(_rec("lv:prop:mush:0", "prop", "mush"))
    cfg = {"prop_cap": 2, "bright_types": ["dark-crystal"], "crystal_props": ["ice"],
           "prop_verdicts": {"ice": "", "mush": "prop not drawn as a light in vanilla"},
           "bake_overrides": {"dark-crystal": {"class": "strong", "gate": "dark-crystal",
                                               "colour": [0.5, 0.15, 1.0], "radius_m": 6.0,
                                               "offset_m": [0.0, 1.7, 0.0]}}}
    rows, skipped = B.bake_level(recs, static_cap=4, energy_ref=1.0, cfg=cfg)
    by = {r["id"]: r for r in rows}
    assert set(by) == {"lv:prop:ice:0", "lv:prop:ice:1", "lv:dc"}
    assert skipped["prop beyond the per-level cap"] == 1
    assert skipped["prop not drawn as a light in vanilla"] == 1
    p = by["lv:prop:ice:0"]
    assert (p["cls"], p["gate"], p["gain"], p["radius_m"]) == (B.CLASSES["glow"], B.GATES["always"], 0.5, 4.0)
    assert p["param"] == B.PROP_CRYSTAL  # a crystal is its own light for a character
    assert "snow-crystals" in B.crystal_props() and "cv-mushroom-01-decal-01" not in B.crystal_props()
    d = by["lv:dc"]
    assert (d["cls"], d["gate"], d["aid"]) == (B.CLASSES["strong"], B.GATES["dark-crystal"], 4)
    assert d["colour"] == [0.5, 0.15, 1.0] and d["radius_m"] == 6.0 and d["pos"] == [1.0, 3.7, 3.0]
    # the shipped table (issue #267)
    bright, over = B.entity_bake()
    assert "dark-crystal" in bright and over["dark-crystal"]["gate"] == "dark-crystal"


def test_eco_clusters_light_their_ground():
    from sandover.lights import bake as B

    _, over = B.entity_bake()
    e = over["eco"]
    # the light that takes over the cluster's tiles must carry their light: weak x 40, 5 m,
    # from the collision sphere the particles spawn at (collectables.gc:115)
    assert (e["class"], e["gate"], e["gain"], e["radius_m"]) == ("weak", "alive", 40.0, 5.0)
    assert e["offset_m"] == [0.0, 0.8, 0.0]
    rec = _rec("lv:eco", "entity", "eco", aid=7)
    rows, _ = B.bake_level([rec], static_cap=4, energy_ref=1.0, cfg={"bake_overrides": over})
    assert rows[0]["cls"] == B.CLASSES["weak"] and rows[0]["gain"] == 40.0 and rows[0]["radius_m"] == 5.0


def test_crate_eco_lights_its_ground_while_out():
    """Crate eco contents (0009, 2026-10-08) bake like a cluster, gated by the crate; orbs and
    cells in crates keep their rows."""
    from sandover.lights import bake as B

    _, over = B.entity_bake()
    for t in ("crate/eco-yellow", "crate/eco-red", "crate/eco-blue"):
        o = over[t]
        assert (o["class"], o["gate"], o["gain"], o["radius_m"]) == ("weak", "crate", 40.0, 5.0)
        assert o["offset_m"] == [0.0, 0.8, 0.0]
    assert "crate/fuel-cell" not in over and "crate/money" not in over
    recs = [_rec("lv:ye", "entity", "crate/eco-yellow", aid=9), _rec("lv:cell", "entity", "crate/fuel-cell", aid=10)]
    rows, _ = B.bake_level(recs, static_cap=4, energy_ref=1.0, cfg={"bake_overrides": over, "bright_types": []})
    by = {r["id"]: r for r in rows}
    ye, cell = by["lv:ye"], by["lv:cell"]
    assert (ye["gate"], ye["gain"], ye["radius_m"], ye["aid"]) == (B.GATES["crate"], 40.0, 5.0, 9)
    assert ye["pos"] == [1.0, 2.8, 3.0]
    assert (cell["gate"], cell["gain"]) == (B.GATES["crate"], 1.0)


def test_eco_blasts_flash_not_linger():
    """A blast's light is a short flash, not the part-tracker's spawn + linger (7 s for the
    yellow shot's launcher and hit; 0010, 2026-10-08)."""
    import re

    from sandover import paths

    gc = (paths.root() / "goal_src/jak1/engine/game/main.gc").read_text(encoding="utf-8")
    life = float(re.search(r"\(define \*pc-eco-blast-life\* \(seconds ([0-9.]+)\)\)", gc).group(1))
    assert 0.0 < life <= 0.2
    body = gc[gc.index("(defun pc-eco-lights-update"):gc.index("(defun pc-lava-light-update")]
    assert "linger-duration" not in body and "*pc-eco-blast-life*" in body


def test_props_glow_only_where_vanilla_draws_them_brighter():
    from sandover.lights import bake as B
    from sandover.lights import props as P

    f = lambda c, sat, own=0.0, around=0.0: {"frame": "x", "contrast": c, "saturation": sat,
                                             "ceiling": [own, around]}
    assert P.glows_in_vanilla({}) == (False, "prop with no measured vanilla frame")
    # cave mushrooms: brighter than the floor in a vivid green
    assert P.glows_in_vanilla({"vanilla": [f(1.65, 0.78), f(1.43, 0.88), f(1.34, 0.86)]})[0]
    # village3 mushrooms: as bright, but pale (brighter by albedo alone)
    assert not P.glows_in_vanilla({"vanilla": [f(1.46, 0.49), f(1.55, 0.45)]})[0]
    # a source the display clips: the snow crystals at the SDR ceiling on bright snow
    assert P.glows_in_vanilla({"vanilla": [f(1.15, 0.7, 0.96, 0.37), f(1.75, 0.68, 0.93, 0.14)]})[0]
    assert not P.glows_in_vanilla({"vanilla": [f(1.2, 0.3, 0.6, 0.4)]})[0]
    # the shipped measurements (prop_lights.json `vanilla`)
    v = B.glow_verdicts()
    assert v["snow-crystals"] == v["cv-tallshroom-decal-01"] == v["cv-mushroom-01-decal-01"] == ""
    assert v["vil3-mushroom-01"] and v["vil3-mushroom-02"] and v["ya-mushroom-whole-01"]


def test_prop_records_skip_in_an_older_bake(tmp_path, monkeypatch):
    """A bake without the prop kind takes a prop for a part spawner and must skip it: prop
    records carry no ``textures`` or ``intensity``."""
    from PIL import Image

    from sandover.lights import props as P

    png = tmp_path / "t.png"
    img = np.zeros((2, 2, 4), np.uint8)
    img[0, 0] = [255, 128, 0, 255]  # transparent texels do not count
    Image.fromarray(img).save(png)
    c = P.texture_linear_colour(png)
    assert c[0] == 1.0 and abs(c[1] - (128 / 255) ** 2.2) < 1e-6 and c[2] == 0.0

    monkeypatch.setattr(P, "level_props",
                        lambda glb, table, level: [P.Prop("ice", "snow-shrub-0", np.array([1.0, 2.0, 3.0]), 2.5, 40)])
    recs = C.prop_lights("snow", tmp_path, {"textures": {"ice": {"levels": ["snow"], "gain": 0.7}}}, tmp_path)
    assert len(recs) == 1
    r = recs[0]
    assert r["kind"] == "prop" and r["gain"] == 0.7 and r["state"]["kind"] == "always"
    assert "textures" not in r and "intensity" not in r


def test_level_props_finds_the_snow_crystals():
    import pytest

    from sandover import paths
    from sandover.lights import props as P

    glb = paths.data_dir() / "unbake/riproot/decompiler_out/jak1/levels/snow/snow-background.glb"
    if not glb.is_file():
        pytest.skip("no snow background glb")
    props = P.level_props(glb, C.load_json("prop_lights.json")["textures"], "snow")
    assert len(props) > 50 and all(p.texture == "snow-crystals" for p in props)
    # the crystal beside Jak in runs/20260926-1011-play 0019
    d = min(float(np.linalg.norm(p.pos - np.array([810.1, 252.6, -3468.3]))) for p in props)
    assert d < 1.0


def test_world_lights_inc_is_current():
    """gk compiles WorldLights.inc; regenerate with `sandover-lights bake` if this fails."""
    import pytest

    from sandover import paths
    from sandover.lights import bake as B

    census = paths.data_dir() / "lights"
    inc = B.cpp_table_path()
    assert inc.is_file(), "WorldLights.inc missing; run sandover-lights bake"
    text = inc.read_text(encoding="utf-8")
    assert text.startswith(B.INC_HEADER[0]) and text.count('{"') > 100
    if not any(census.glob("*.json")):
        pytest.skip("no light census under $SANDOVER_DATA/lights")
    rows, _ = B.bake_rows(census)
    assert text == B.cpp_table(rows)


def test_spots_geometry_helpers():
    from sandover.lights import spots as S

    tri = np.array([[[0.0, -1.0, -1.0], [0.0, 1.0, -1.0], [0.0, 0.0, 1.0]]])
    assert S.seg_blocked(tri, np.array([-2.0, 0.0, 0.0]), np.array([2.0, 0.0, 0.0]))
    assert not S.seg_blocked(tri, np.array([-2.0, 5.0, 0.0]), np.array([2.0, 5.0, 0.0]))
    R = S.look_rot(np.array([0.0, 0.0, 1.0]))
    assert np.allclose(R, np.eye(3))
    ax, _, z = S.angles(S.yaw(R, np.radians(30)), np.zeros(3), np.array([0.0, 0.0, 5.0]))
    assert z > 0 and abs(abs(np.degrees(ax)) - 30.0) < 1e-6


def test_spots_palette_weights():
    from sandover.lights import spots as S

    quick = S.palette_weights({"mode": "quick", "quick_slot": {"lights": 0}}, 12)
    assert quick.tolist() == [1.0] + [0.0] * 7
    interp = [{"snapshot1": 6, "snapshot2": 7, "morph_start": 0.0, "morph_end": 0.2}] * 24
    w = S.palette_weights({"mode": "cycle", "palette_interp": interp}, 23)
    assert abs(w[6] - 0.9) < 1e-9 and abs(w[7] - 0.1) < 1e-9


def test_eco_lights_inc_is_current():
    """gk compiles EcoLights.inc; regenerate with `sandover-lights eco` if this fails."""
    from sandover.lights import eco as E

    inc = E.cpp_table_path()
    assert inc.is_file(), "EcoLights.inc missing; run sandover-lights eco"
    rows = E.eco_groups()
    assert inc.read_text(encoding="utf-8") == E.cpp_table(rows)
    names = {r.name: r for r in rows}
    # Jak's yellow shot, its hit, the boss's shots and the bomb blast; the blue eco pickup stream is not a shot
    for n in ("group-yellow-eco-fireball", "group-part-yellow-eco-fireball-hit", "group-robotboss-greenshot",
              "group-robotboss-yellowshot", "group-robotboss-redshot-body", "group-final-boss-mine-explosion"):
        assert n in names, n
    assert "group-eco-blue" not in names
    assert names["group-part-yellow-eco-fireball-hit"].kind == 1 and names["group-yellow-eco-fireball"].kind == 0
    r, g, b = names["group-robotboss-greenshot"].colour
    assert g == 1.0 and r < 0.5 and b < 0.5
    assert all(max(r.colour) == 1.0 for r in rows)


def test_eco_strength_preset_matches_engine():
    import re

    from sandover import presets

    gfx = (presets.root() / "game/graphics/gfx.h").read_text(encoding="utf-8")
    m = re.search(r"float eco_lights_strength = ([0-9.]+)f;", gfx)
    assert m and float(m.group(1)) == presets.emissive_light_params("default")["eco_strength"]
    assert "bool eco_lights = true;" in gfx
    assert "(pc-set-emissive-lights 28 2500000)" in presets.goal_forms("emissive", "default")


def test_eco_defaults_are_the_picked_arm():
    """The engine defaults and the play preset are the forms of the arm picked in
    the chosen variant (27 on, 28 at 2.5), so the play look needs no forms."""
    import re

    from sandover import presets

    picked = {27: 1000000, 28: 2500000}
    gfx = (presets.root() / "game/graphics/gfx.h").read_text(encoding="utf-8")
    on = re.search(r"bool eco_lights = (true|false);", gfx).group(1) == "true"
    strength = float(re.search(r"float eco_lights_strength = ([0-9.]+)f;", gfx).group(1))
    assert {27: int(on) * 1000000, 28: round(strength * 1000000)} == picked
    forms = presets.goal_forms("emissive", "default")
    assert f"(pc-set-emissive-lights 28 {picked[28]})" in forms


def test_launch_lights_inc_is_current():
    """gk compiles LaunchLights.inc; regenerate with `sandover-lights launch` if this fails."""
    from sandover import paths
    from sandover.lights import launch as L

    inc = L.cpp_table_path()
    assert inc.is_file(), "LaunchLights.inc missing; run sandover-lights launch"
    groups = L.launch_groups()
    assert inc.read_text(encoding="utf-8") == L.cpp_table(groups)
    assert not set(groups) - L.defined_groups(paths.root() / "goal_src" / "jak1")
    # glow groups only: broken-state smoke never lights; the eco pickup stream does (maintainer, 2026-10-15)
    assert "group-eco-blue" in groups and "group-eco-blue-collect" in groups
    assert groups["group-snow-bumper-idle"] == (0.0, 1.5, 0.0)
    assert not [g for g in groups if "broken" in g or g.endswith("-off")]
