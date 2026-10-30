import json
from pathlib import Path

import numpy as np
import pytest
from PIL import Image

from sandover.materials import pbr_maps as pm

RECIPES = pm.load_recipes(Path(__file__).resolve().parents[2] / "presets" / "pbr-recipes.toml")


def _rgba(lum: np.ndarray, alpha: np.ndarray | None = None) -> np.ndarray:
    v = np.clip(np.rint(lum * 255), 0, 255).astype(np.uint8)
    a = np.full(lum.shape, 128, np.uint8) if alpha is None else alpha.astype(np.uint8)
    return np.dstack([v, v, v, a])


def _stone():
    return pm.resolve(RECIPES, "stone")[0]


def test_flat_texture_gives_flat_normal():
    d = pm.derive(_rgba(np.full((64, 64), 0.5)), _stone())
    assert np.allclose(d.normal[..., 2], 1.0)
    assert np.allclose(d.normal[..., :2], 0.0, atol=1e-6)


def test_vertical_stripe_bends_normals_horizontally_only():
    lum = np.full((64, 64), 0.3)
    lum[:, 28:36] = 0.8
    d = pm.derive(_rgba(lum), _stone())
    nx, ny = d.normal[..., 0], d.normal[..., 1]
    assert np.abs(nx).max() > 0.05
    assert np.abs(ny).max() < 1e-6
    # bright = high for stone: the left edge faces left (-x), the right edge faces right
    assert nx[:, 26].mean() < 0 < nx[:, 37].mean()
    # constant along the stripe
    assert np.allclose(nx, nx[0:1], atol=1e-6)


def test_horizontal_stripe_uses_glTF_y_up():
    lum = np.full((64, 64), 0.3)
    lum[28:36, :] = 0.8
    d = pm.derive(_rgba(lum), _stone())
    ny = d.normal[..., 1]
    # the top edge of the ridge faces up the image (+Y), the bottom edge down
    assert ny[26].mean() > 0 > ny[37].mean()


def test_invert_flips_the_slope():
    lum = np.full((64, 64), 0.3)
    lum[:, 28:36] = 0.8
    base = _stone()
    a = pm.derive(_rgba(lum), base)
    b = pm.derive(_rgba(lum), pm.Recipe(**{**pm.asdict(base), "invert": True}))
    assert np.allclose(a.normal[..., 0], -b.normal[..., 0], atol=1e-6)


def test_normals_wrap_at_the_border():
    lum = np.full((64, 64), 0.3)
    lum[:, :4] = 0.8  # stripe touching the left edge
    d = pm.derive(_rgba(lum), _stone())
    # the far right column sees the stripe's rising edge through the wrap
    assert d.normal[:, 63, 0].mean() < -0.01


def test_alpha_masked_texels_are_flat_and_do_not_ring():
    rng = np.random.default_rng(0)
    lum = 0.5 + 0.02 * rng.standard_normal((64, 64))
    alpha = np.full((64, 64), 128)
    alpha[:, 32:] = 0  # right half cut out, with a very different colour
    lum[:, 32:] = 0.0
    d = pm.derive(_rgba(lum, alpha), _stone())
    assert np.allclose(d.normal[:, 32:, 2], 1.0)
    # no step at the cut-out edge: the opaque column next to it is no steeper than the rest
    edge = np.abs(d.normal[:, 31, 0]).mean()
    inner = np.abs(d.normal[:, 8:24, 0]).mean()
    assert edge < 3 * inner + 0.01
    unmasked = pm.derive(_rgba(lum), _stone())
    assert np.abs(unmasked.normal[:, 31, 0]).mean() > 10 * edge


def test_flat_classes_and_tiny_textures_are_flat():
    rng = np.random.default_rng(1)
    lum = rng.random((64, 64))
    for cls in ("special", "glass"):
        assert np.allclose(pm.derive(_rgba(lum), pm.resolve(RECIPES, cls)[0]).normal[..., 2], 1.0)
    # lava crust shades as stone (the glow takes the molten texels back to their own colour)
    assert not pm.resolve(RECIPES, "lava")[0].flat
    assert pm.derive(_rgba(lum[:8, :8]), _stone()).stats["flat"]


def test_scale_keeps_the_look_between_vanilla_and_4x():
    lum = np.full((32, 32), 0.3)
    lum[:, 12:20] = 0.8
    small = pm.derive(_rgba(lum), _stone(), 1.0)
    big = pm.derive(_rgba(np.kron(lum, np.ones((4, 4)))), _stone(), 4.0)
    assert abs(small.stats["slope_p95_deg"] - big.stats["slope_p95_deg"]) < 0.35 * small.stats["slope_p95_deg"]


def test_roughness_and_metallic_follow_the_class():
    lum = np.full((32, 32), 0.5)
    rough = {c: pm.derive(_rgba(lum), pm.resolve(RECIPES, c)[0]).rough.mean() for c in ("metal", "stone", "glass", "cloth")}
    assert rough["glass"] < rough["metal"] < rough["stone"] <= rough["cloth"] + 0.05
    assert pm.resolve(RECIPES, "metal")[0].metallic == 1.0
    assert pm.resolve(RECIPES, "stone")[0].metallic == 0.0


def test_precursor_tag_overrides_metal():
    r, key = pm.resolve(RECIPES, "metal", ("precursor",))
    assert key == "metal+precursor"
    assert r.metallic == 1.0 and r.invert is False
    assert 0.35 <= r.rough_base <= 0.55


def test_wood_grain_follows_the_dominant_orientation():
    lum = np.full((64, 64), 0.5)
    lum[:, ::6] = 0.2  # vertical grain lines
    angle, coh = pm.structure_orientation(lum, np.ones_like(lum, bool))
    assert coh > 0.9
    assert abs(np.cos(angle)) < 0.05  # grain runs along y


def test_unknown_recipe_key_is_rejected(tmp_path: Path):
    p = tmp_path / "r.toml"
    p.write_text("version = 1\n[class.stone]\nnormal_strenght = 2\n", encoding="utf-8")
    with pytest.raises(ValueError, match="normal_strenght"):
        pm.load_recipes(p)


def test_build_writes_maps_and_merges_the_index(tmp_path: Path):
    tex = tmp_path / "tex" / "tp"
    tex.mkdir(parents=True)
    lum = np.full((32, 32), 0.3)
    lum[:, 10:14] = 0.9
    for n in ("a", "b"):
        Image.fromarray(_rgba(lum), "RGBA").save(tex / f"{n}.png")
    rows = [{"tpage": "tp", "name": "a", "class": "stone", "tags": ""},
            {"tpage": "tp", "name": "b", "class": "metal", "tags": "precursor"}]
    out = tmp_path / "out"
    jobs = pm.plan(rows, RECIPES, tmp_path / "tex", tmp_path / "nopack")
    pm.build(jobs[:1], out, RECIPES["version"], workers=1)
    pm.build(pm.plan(rows, RECIPES, tmp_path / "tex", tmp_path / "nopack", only={"tp/b"}), out,
             RECIPES["version"], workers=1)
    index = json.loads((out / "index.json").read_text(encoding="utf-8"))
    assert set(index["textures"]) == {"tp/a", "tp/b"}
    assert index["textures"]["tp/b"]["metallic"] == 1.0
    assert index["textures"]["tp/b"]["recipe"] == "metal+precursor"
    assert index["textures"]["tp/a"]["source"] == "vanilla"
    with Image.open(out / "tp" / "a.normal.png") as im:
        assert im.mode == "RGB" and im.size == (32, 32)
    with Image.open(out / "tp" / "a.rough.png") as im:
        assert im.mode == "L"
    sheets = pm.render_sheets(index, out, tmp_path / "tex", tmp_path / "nopack")
    assert {p.name for p in sheets} == {"stone.png", "metal.png", "tag-precursor.png"}


def test_extra_bands_add_broad_forms():
    y, x = np.mgrid[0:64, 0:64]
    lum = 0.5 + 0.2 * np.sin(2 * np.pi * x / 32)  # a broad wave the fine band barely sees
    base = pm.Recipe(band_lo=0.5, band_hi=2.0)
    wide = pm.Recipe(band_lo=0.5, band_hi=2.0, extra_bands=((2.0, 8.0, 1.0),))
    assert pm.derive(_rgba(lum), wide).stats["slope_p95_deg"] > 2 * pm.derive(_rgba(lum), base).stats["slope_p95_deg"]
    r, _ = pm.resolve(RECIPES, "stone")
    assert r.extra_bands and len(r.extra_bands[0]) == 3


def test_parallax_gate_masonry_cue_and_column():
    cfg = RECIPES["parallax"]
    assert pm.parallax_qualifies(cfg, "stone", "vil1-sages-stonewall-01")
    assert pm.parallax_qualifies(cfg, "stone", "bch-idol-brick-01")
    assert not pm.parallax_qualifies(cfg, "stone", "vil-beachrock")
    assert not pm.parallax_qualifies(cfg, "wood", "vil-wood-wall")
    assert not pm.parallax_qualifies(cfg, "stone", "cv-wall-02", "no")
    assert pm.parallax_qualifies(cfg, "stone", "vil-beachrock", "yes")


def test_parallax_gate_stone_floors_by_pat():
    cfg = RECIPES["parallax"]
    assert pm.parallax_qualifies(cfg, "stone", "vil3-cave-ground", floor=("stone", 1.0, 4237.0))
    assert not pm.parallax_qualifies(cfg, "stone", "vil3-cave-ground")
    assert not pm.parallax_qualifies(cfg, "stone", "bch-beach-01", floor=("sand", 1.0, 70656.0))
    assert not pm.parallax_qualifies(cfg, "stone", "jng-beach-01", floor=("stone", 0.77, 15994.0))
    assert not pm.parallax_qualifies(cfg, "wood", "vil-hutfloor", floor=("stone", 1.0, 4237.0))
    assert not pm.parallax_qualifies(cfg, "stone", "ogr-cavefloor", "no", floor=("stone", 1.0, 4237.0))
    assert not pm.parallax_qualifies(cfg, "stone", "vil3-miner-camprock", floor=("stone", 1.0, 0.4))


def test_read_floor_pats_keeps_the_largest_level(tmp_path: Path):
    p = tmp_path / "pat-census.csv"
    p.write_text("texture,tpage,level,pat_material,share,matched_area_m2\n"
                 "a,tp,l1,sand,1.000,10.0\n"
                 "a,tp,l2,stone,0.980,50.0\n"
                 "b,tp,l1,,,0.0\n", encoding="utf-8")
    assert pm.read_floor_pats(p) == {"tp/a": ("stone", 0.98, 50.0)}
    with pytest.raises(FileNotFoundError):
        pm.read_floor_pats(tmp_path / "missing.csv")


def test_prune_parallax_clears_textures_that_no_longer_qualify(tmp_path: Path):
    (tmp_path / "tp").mkdir()
    for n in ("keep", "stale"):
        (tmp_path / "tp" / f"{n}.parallax.png").write_bytes(b"x")
    index = {"recipe_version": 4, "textures": {"tp/keep": {"parallax": True},
                                               "tp/stale": {"parallax": True},
                                               "tp/flat": {"parallax": False}}}
    (tmp_path / "index.json").write_text(json.dumps(index), encoding="utf-8")
    assert pm.prune_parallax(tmp_path, {"tp/keep"}) == ["tp/stale"]
    after = json.loads((tmp_path / "index.json").read_text(encoding="utf-8"))["textures"]
    assert after["tp/keep"]["parallax"] is True and after["tp/stale"]["parallax"] is False
    assert (tmp_path / "tp" / "keep.parallax.png").exists()
    assert not (tmp_path / "tp" / "stale.parallax.png").exists()


def test_parallax_height_only_for_opaque_qualifying_textures(tmp_path: Path):
    tex = tmp_path / "tex" / "tp"
    tex.mkdir(parents=True)
    lum = np.full((32, 32), 0.3)
    lum[:, 10:14] = 0.9
    alpha = np.full((32, 32), 128)
    alpha[:, :24] = 0
    Image.fromarray(_rgba(lum), "RGBA").save(tex / "x-brick.png")
    Image.fromarray(_rgba(lum, alpha), "RGBA").save(tex / "y-brick.png")
    Image.fromarray(_rgba(lum), "RGBA").save(tex / "z-rock.png")
    rows = [{"tpage": "tp", "name": n, "class": "stone", "tags": ""}
            for n in ("x-brick", "y-brick", "z-rock")]
    out = tmp_path / "out"
    jobs = pm.plan(rows, RECIPES, tmp_path / "tex", tmp_path / "nopack", parallax_only=True)
    assert [j.name for j in jobs] == ["x-brick", "y-brick"]
    floors = {"tp/z-rock": ("stone", 1.0, 100.0)}
    assert [j.name for j in pm.plan(rows, RECIPES, tmp_path / "tex", tmp_path / "nopack",
                                    parallax_only=True, floors=floors)] == ["x-brick", "y-brick", "z-rock"]
    index = pm.build(jobs, out, RECIPES["version"], workers=1)
    assert index["textures"]["tp/x-brick"]["parallax"] is True
    assert index["textures"]["tp/y-brick"]["parallax"] is False  # cut-out
    assert not (out / "tp" / "y-brick.parallax.png").exists()
    with Image.open(out / "tp" / "x-brick.parallax.png") as im:
        h = np.asarray(im)
    assert im.mode == "L" and h[:, 11].mean() > h[:, 2].mean()  # bright stripe is high
