import numpy as np

from sandover.unbake import sidecar as sc


def _oct_decode(e):
    n = np.stack([e[:, 0], e[:, 1], 1 - np.abs(e[:, 0]) - np.abs(e[:, 1])], 1)
    neg = n[:, 2] < 0
    x, y = n[neg, 0].copy(), n[neg, 1].copy()
    n[neg, 0] = (1 - np.abs(y)) * np.where(x >= 0, 1, -1)
    n[neg, 1] = (1 - np.abs(x)) * np.where(y >= 0, 1, -1)
    return n / np.linalg.norm(n, axis=1, keepdims=True)


def test_pack_roundtrip():
    rng = np.random.default_rng(1)
    n = rng.normal(size=(500, 3))
    n /= np.linalg.norm(n, axis=1, keepdims=True)
    a, k = rng.uniform(0, 1.5, 500), rng.uniform(0, 3.9, 500)
    slots = np.array([0, 1, 2, 3, 4, 6, 7])
    v = rng.uniform(0, 1, (500, 7))
    q = rng.uniform(0, 1, 500)
    rec = sc.pack_records(a, k, n, v, slots, q)
    assert np.allclose((rec[:, 0] & 0xFFFF) / sc.AK_SCALE, a, atol=1e-4)
    assert np.allclose((rec[:, 0] >> 16) / sc.AK_SCALE, k, atol=1e-4)
    e = np.stack([rec[:, 1] & 0xFFFF, rec[:, 1] >> 16], 1) / 32767.5 - 1.0
    assert np.degrees(np.arccos(np.clip((_oct_decode(e) * n).sum(1), -1, 1))).max() < 0.05
    for j, s in enumerate(slots):
        assert np.allclose(((rec[:, 2] >> (4 * s)) & 0xF) / 15, v[:, j], atol=1 / 30 + 1e-6)
    assert ((rec[:, 2] >> 20) & 0xF == 0).all()  # slot 5 has no fit


def test_digest_is_order_sensitive():
    p = np.arange(30, dtype=np.float32).reshape(10, 3)
    assert sc.pos_digest(p) != sc.pos_digest(p[::-1].copy())
    assert sc.pos_digest(p) == sc.pos_digest(p.copy())


def test_emissive_sources():
    from sandover.unbake import emissive as em

    # vertices: plain (day-lit), lamp baked into night slots, flame slot 5, lit all day
    lum = np.zeros((8, 4))
    lum[[0, 1, 2, 3, 4], :] = [[60], [60], [60], [60], [60]]
    lum[[6, 7], :] = 30
    lum[[6, 7], 1] = 180
    lum[5, 2] = 200
    lum[:, 3] = 220
    lum[5, 3] = 0
    er = em.EmissiveRoles(effect=[5], night=[6, 7], day=[0, 1, 2, 3, 4], emitter=[5], night_ratio=0.5)
    e, src = em.compute(lum, er)
    assert e[0] == 0 and src[0] == 0
    assert np.isclose(e[1], (180 - 30 - em.NIGHT_MARGIN) / 255) and src[1] == em.SRC_NIGHT
    assert np.isclose(e[2], 200 / 255) and src[2] == 5
    night3 = 220 - (0.5 * 220 + em.NIGHT_MARGIN)
    assert np.isclose(e[3], (220 - em.STATIC_T + night3) / 255) and src[3] == em.SRC_STATIC
    w = sc.pack_emissive(e, src)
    assert (((w >> 16) & 0xFF) == np.rint(e * 255)).all()
    assert (((w >> 24) & 0xF) == np.where(e > 0, src, 0)).all()
    assert (w & 0xFFFF == 0).all()


def test_scan_glb_tree_names_json_only():
    gltf = {
        "scene": 0,
        "scenes": [{"nodes": [0, 1, 2, 3]}],
        "nodes": [
            {"name": "jungle-tfrag-lowres"},  # skipped: SKIP_TREES
            {"name": "jungle-tfrag-normal"},  # kind 0, encounter order 0
            {"name": "jungle-tie-3"},         # kind 1, from the name's suffix
            {"name": "jungle-shrub-0"},       # no vertex-order guarantee: omitted
        ],
    }
    assert sc.scan_glb_tree_names(gltf) == {
        "jungle-tfrag-normal": (sc.KIND["tfrag"], 0),
        "jungle-tie-3": (sc.KIND["tie"], 3),
    }


def test_scan_glb_tree_names_tfrag_index_is_encounter_order_not_the_name():
    gltf = {
        "scenes": [{"nodes": [0, 1]}],
        "nodes": [{"name": "beach-tfrag-a"}, {"name": "beach-tfrag-b"}],
    }
    assert sc.scan_glb_tree_names(gltf) == {"beach-tfrag-a": (0, 0), "beach-tfrag-b": (0, 1)}


def test_emissive_roles():
    from sandover.unbake import emissive as em

    er = em.roles("village1")
    assert er.effect == [5] and er.night == [6, 7] and er.night_mask == 0xC0 and er.emitter_mask == 0x20
    assert em.roles("maincave").night == []
    assert em.roles("rolling").night == [2, 3] and em.roles("firecanyon").night == [3]
    sw = em.roles("swamp")
    assert sw.night == [1] and sw.emitter == [4, 5, 6, 7]  # lightning 2-3 is not an emitter


def test_day_mask_leaves_out_night_slots():
    from sandover.unbake import emissive

    # misty's palette has its moon in slot 1, firecanyon's in slot 3, beach's in 6-7
    assert sc.day_slot_mask(np.array([0, 1]), emissive.roles("misty")) == 0b1
    assert sc.day_slot_mask(np.array([0, 1, 2, 3]), emissive.roles("firecanyon")) == 0b111
    assert sc.day_slot_mask(np.array([0, 1, 2, 3, 4, 6, 7]), emissive.roles("beach")) == 0b11111
