import csv
import math
from pathlib import Path

import pytest

from sandover.materials import actor_emissive as ae
from sandover.materials import emissive_strengths as es
from sandover.materials import overrides as ov

CFG = {
    "ramp": (0.1, 0.4), "threshold": 0.3, "strength": 1.0, "cap": 0.98,
    "textures": [
        {"name": "bigpuff", "combo_id": "0x00020000", "tpage": "effects", "element": "eco",
         "peak": 0.6, "min_chroma": 0.5, "note": ""},
        {"name": "egg-ndimadman", "combo_id": "0x01cf002c", "tpage": "common", "element": "orb",
         "peak": 0.7, "min_chroma": 0.0, "note": "", "lights": 0.0},
    ],
}


def test_weight_inverse_round_trips():
    for w in (0.0, 0.1, 0.5, 0.9, 1.0):
        e = ae.e_for_weight(w, 0.1, 0.4)
        assert 0.1 <= e <= 0.4
        assert ae.bloom_weight(e, 0.1, 0.4) == pytest.approx(w, abs=1e-6)


def test_max_weight_edges_and_formula():
    assert ae.max_weight(1.0, 0.3) == 0.0
    assert ae.max_weight(0.99, 0.3, cap=0.98) == 0.0
    assert ae.max_weight(0.2, 0.3) == 1.0
    assert ae.max_weight(0.8, 0.3, cap=0.98) == pytest.approx(0.18 / 0.5)


@pytest.mark.parametrize("peak", [0.35, 0.5, 0.7, 0.85, 0.95])
def test_ladder_step_is_the_highest_safe_quarter_octave(peak):
    s = ae.ladder_step(peak, CFG)
    lo, hi = CFG["ramp"]

    def after_bloom(strength):
        return peak + ae.bloom_weight(strength, lo, hi) * (peak - CFG["threshold"])

    assert after_bloom(s) <= CFG["cap"] + 1e-9
    up = s * es.NUDGE
    assert after_bloom(up) > CFG["cap"] or ae.bloom_weight(up, lo, hi) == 1.0
    k = math.log(s) / math.log(es.NUDGE)
    assert k == pytest.approx(round(k))


def test_ladder_step_for_clipped_peak_is_below_the_ramp():
    s = ae.ladder_step(1.0, CFG)
    assert ae.bloom_weight(s, *CFG["ramp"]) == 0.0


def test_seed_rows_new_texture_gets_override_and_ladder():
    new_ov, new_st = ae.seed_rows(CFG, [], [])
    assert [r["name"] for r in new_ov] == ["bigpuff", "egg-ndimadman"]
    assert all(r["verdict"] == "emissive" for r in new_ov)
    assert [r["name"] for r in new_st] == ["bigpuff", "egg-ndimadman"]
    assert new_st[0]["strength"] == ae.ladder_step(0.6, CFG)


def test_seed_rows_is_idempotent_and_keeps_human_rows():
    new_ov, new_st = ae.seed_rows(CFG, [], [])
    human = {"combo_id": "0x01cf002c", "name": "egg-ndimadman", "strength": "2", "source": "human"}
    no = {"combo_id": "0x00020000", "name": "bigpuff", "verdict": "not-emissive"}
    ov2, st2 = ae.seed_rows(CFG, new_ov + [no], [{**r, "strength": str(r["strength"])} for r in new_st] + [human])
    assert ov2 == [] and st2 == []


def test_export_rows_scales_strength_and_skips_non_emissive():
    overrides = [
        {"combo_id": "0x00020000", "name": "bigpuff", "verdict": "emissive"},
        {"combo_id": "0x01cf002c", "name": "egg-ndimadman", "verdict": "emissive"},
        {"combo_id": "", "name": "no-combo", "verdict": "emissive"},
        {"combo_id": "0x00d6005c", "name": "gen-metal-dark", "verdict": "not-emissive"},
        {"combo_id": "0x00d60002", "name": "bab-eye", "verdict": "emissive"},
    ]
    strengths = [
        {"combo_id": "0x00020000", "name": "bigpuff", "strength": "0.25", "source": "default"},
        {"combo_id": "0x01cf002c", "name": "egg-ndimadman", "strength": "4", "source": "human"},
    ]
    rows = {r["name"]: r for r in ae.export_rows(CFG, overrides, strengths, peak_e=0.9)}
    assert set(rows) == {"bigpuff", "egg-ndimadman", "bab-eye"}
    assert float(rows["bigpuff"]["e"]) == pytest.approx(0.225)
    assert float(rows["bigpuff"]["min_chroma"]) == 0.5
    assert float(rows["egg-ndimadman"]["e"]) == 1.0
    assert float(rows["bab-eye"]["e"]) == pytest.approx(0.9)
    assert float(rows["bab-eye"]["min_chroma"]) == 0.0
    assert float(rows["egg-ndimadman"]["lights"]) == 0.0
    assert float(rows["bigpuff"]["lights"]) == 1.0 and float(rows["bab-eye"]["lights"]) == 1.0


def test_preset_textures_are_well_formed():
    cfg = ae.load_config(Path(__file__).resolve().parents[2] / "presets" / "emissive.toml")
    names = [t["name"] for t in cfg["textures"]]
    assert names and len(names) == len(set(names))
    for t in cfg["textures"]:
        assert t["combo_id"].startswith("0x") and int(t["combo_id"], 16) > 0
        assert 0.0 < t["peak"] <= 1.0
        assert 0.0 <= t["min_chroma"] < 1.0
        assert 0.0 <= t["lights"] <= 1.0


def test_orbs_glow_but_light_nothing_cells_light():
    """Design decision 2026-09-26: orb textures keep E (bloom, SSAO fade), give no tile light."""
    cfg = ae.load_config(Path(__file__).resolve().parents[2] / "presets" / "emissive.toml")
    by = {t["name"]: t for t in cfg["textures"]}
    for orb in ("egg-ndimadman", "hud-egg"):
        assert by[orb]["lights"] == 0.0 and by[orb]["peak"] > 0.0
    for cell in ("fuel-cell-inside", "flare", "starflash"):
        assert by[cell]["lights"] == 1.0


def _chroma(rgb):
    hi = max(rgb)
    return (hi - min(rgb)) / hi if hi else 0.0


def test_flare_row_keeps_cell_flares_not_pale_haze():
    """The flare texture is shared: power cell starburst parts (collectables-part.gc 228/229, each
    channel 0 or 255) glow; the beach grotto's light shafts (beach-part.gc 666, 192,192,128) do not."""
    cfg = ae.load_config(Path(__file__).resolve().parents[2] / "presets" / "emissive.toml")
    cut = {t["name"]: t for t in cfg["textures"]}["flare"]["min_chroma"]
    for rgb in ((255, 0, 0), (0, 255, 255), (255, 255, 0), (0, 0, 255)):
        assert _chroma(rgb) >= cut
    assert _chroma((192, 192, 128)) < cut


def test_cli_seed_then_export(tmp_path: Path, monkeypatch):
    preset = tmp_path / "emissive.toml"
    preset.write_text(
        '[actor_emissive]\ncap = 0.98\n'
        '[[actor_emissive.textures]]\nname = "hotdot"\ncombo_id = "0x0002000f"\n'
        'tpage = "effects"\npeak = 0.7\nmin_chroma = 0.4\n', encoding="utf-8")
    o, s, out = tmp_path / "o.csv", tmp_path / "s.csv", tmp_path / "t.csv"
    monkeypatch.setattr(ae, "torch_peak", lambda index=None: 1.0)
    args = ["--seed", "--preset", str(preset), "--overrides", str(o), "--strengths", str(s),
            "--out", str(out)]
    assert ae.main(args) == 0
    assert ae.main(args) == 0
    assert len(ov.read_overrides(o)) == 1
    assert len(es.read_strengths(s)) == 1
    with out.open(encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    assert list(rows[0]) == ae.FIELDS
    assert rows[0]["combo_id"] == "0x0002000f"
    assert float(rows[0]["e"]) == pytest.approx(ae.ladder_step(0.7, ae.load_config(preset)), abs=1e-4)


def test_default_step_adds_octaves_and_honours_explicit_step():
    t = dict(CFG["textures"][0])
    rule = ae.ladder_step(t["peak"], CFG)
    assert ae.default_step(t, CFG) == pytest.approx(rule)
    assert ae.default_step(t, {**CFG, "octaves": 1.0}) == pytest.approx(2 * rule)
    clipped = {**t, "peak": 1.0, "step": 0.105}
    assert ae.default_step(clipped, CFG) == pytest.approx(es.NUDGE ** -13)
    assert ae.default_step(clipped, {**CFG, "octaves": 1.0}) == pytest.approx(es.NUDGE ** -9)


def test_shipped_preset_is_rule_plus_one_octave():
    cfg = ae.load_config()
    assert cfg["octaves"] == 1.0
    for t in cfg["textures"]:
        s = ae.default_step(t, cfg)
        base = t["step"] if t["step"] is not None else ae.ladder_step(t["peak"], cfg)
        assert s == pytest.approx(2 * base, rel=0.1)
