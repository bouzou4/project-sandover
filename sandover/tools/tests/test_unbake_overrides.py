import numpy as np

from sandover.materials.overrides import append_override
from sandover.unbake import emissive as em
from sandover.unbake import overrides as ov


def _w(e, src):
    return (np.asarray(e, np.uint32) << 16) | (np.asarray(src, np.uint32) << 24) | np.uint32(0x1FF)


def test_load_uses_shared_contract(tmp_path):
    p = tmp_path / "o.csv"
    for row in [
        {"combo_id": "0x00d40025", "name": "bch-lamp", "level": "beach", "verdict": "not-emissive"},
        {"combo_id": "0x00d40025", "name": "bch-lamp", "level": "beach", "draw_id": "tfrag/beach/0/0/3",
         "verdict": "emissive", "note": "a, note"},
        {"combo_id": "0x00d40009", "name": "rock", "verdict": "emissive"},
        {"combo_id": "0x00d40009", "name": "rock", "verdict": ""},
        {"combo_id": "", "name": "wall", "verdict": "not-emissive"},
    ]:
        append_override(p, row)
    assert ov.load(p) == {"bch-lamp": "emissive", "wall": "not-emissive"}
    assert ov.load(tmp_path / "missing.csv") == {}


def test_apply_forces_e_and_keeps_rest():
    verdicts = {"lamp": "emissive", "rock": "not-emissive"}
    w = _w([0, 120, 50, 60], [0, em.SRC_NIGHT, 5, 5])
    n = ov.apply(verdicts, w, {"lamp": np.array([0, 1]), "rock": np.array([2]), "x": np.array([3])})
    e, src = (w >> 16) & 0xFF, (w >> 24) & 0xF
    assert e.tolist() == [255, 255, 0, 60]
    assert src.tolist() == [em.SRC_STATIC, em.SRC_NIGHT, 0, 5]
    assert ((w & 0xFFFF) == 0x1FF).all()
    assert n == 3
