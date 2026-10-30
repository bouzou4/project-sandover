import json

import numpy as np
from PIL import Image

from sandover.rig import pixdiff


def _png(path, arr):
    Image.fromarray(arr.astype(np.uint8)).save(path)


def test_identical_and_small_and_large(tmp_path):
    a, b = tmp_path / "a", tmp_path / "b"
    a.mkdir()
    b.mkdir()
    base = np.full((4, 5, 3), 100, dtype=np.uint8)
    _png(a / "same.png", base)
    _png(b / "same.png", base)
    small = base.copy()
    small[1, 2, 0] = 102
    _png(a / "small.png", base)
    _png(b / "small.png", small)
    big = base.copy()
    big[0, 0, 2] = 110
    _png(a / "big.png", base)
    _png(b / "big.png", big)
    _png(a / "only-a.png", base)
    rows = {r["name"]: r for r in pixdiff.compare(a, b, threshold=2)}
    assert set(rows) == {"same", "small", "big"}
    assert rows["same"]["max"] == 0 and rows["same"]["px_diff"] == 0
    assert rows["small"]["max"] == 2 and rows["small"]["px_diff"] == 1 and rows["small"]["px_over"] == 0
    assert rows["big"]["max"] == 10 and rows["big"]["px_over"] == 1
    assert pixdiff.main([str(a), str(b)]) == 1
    assert pixdiff.main([str(a), str(b), "--threshold", "10"]) == 0
    assert rows["big"]["share_over"] == 1 / 20
    assert pixdiff.main([str(a), str(b), "--json", str(tmp_path / "d.json")]) == 1
    doc = json.loads((tmp_path / "d.json").read_text(encoding="utf-8"))
    assert doc["threshold"] == 2 and {r["name"] for r in doc["shots"]} == {"same", "small", "big"}
