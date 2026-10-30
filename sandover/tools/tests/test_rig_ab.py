"""Paused-frame A/B captures (capture run --ab-mask): forms, rig sequence, file layout."""

import re
import time

import numpy as np
from PIL import Image

from sandover.rig import capture as C
from sandover.rig import pixdiff

RIG_GC = (C.HERE / "rig.gc").read_text(encoding="utf-8")


def test_ab_forms():
    assert C.AbSpec(0xFFFFFFFF, 0).forms() == ("(set! *rig-ab* #t) (set! *rig-ab-a* 4294967295) "
                                                "(set! *rig-ab-mask* 0) (pc-set-water 90 100000000)")
    assert C.AbSpec(7, 3, None).forms() == "(set! *rig-ab* #t) (set! *rig-ab-a* 7) (set! *rig-ab-mask* 3)"
    a, b, a2 = C.ab_dirs(C.Path("out"))
    assert (a.name, b.name, a2.name) == ("A", "B", "A2")


def test_rig_sequence_shoots_a_b_a2_in_the_same_pause():
    seq = RIG_GC[RIG_GC.index("(defun rig-sequence"):RIG_GC.index("(defun rig-set-options")]
    order = [seq.index(s) for s in (
        "(set-master-mode 'pause)",
        "(pc-set-perf-opt *rig-ab-a*)",
        "(pc-screen-shot)",
        "(pc-set-perf-opt *rig-ab-mask*)",
        '(rig-shot-suffix "--ab-b")',
        '(rig-shot-suffix "--ab-a2")',
        "RIG:ABDONE",
        '"RIG:SHOT ~A',
        "(set-master-mode 'game)")]
    assert order == sorted(order)
    assert seq.count("(pc-screen-shot)") == 3
    # each frame waits the same settle after its mask is set; A2 is taken with mask A again
    assert len(re.findall(r"\(dotimes \(i RIG_AB_SETTLE\)", seq)) == 3
    a2 = seq.index('(rig-shot-suffix "--ab-a2")')
    assert seq.rindex("(pc-set-perf-opt *rig-ab-a*)", 0, a2) > seq.index("(pc-set-perf-opt *rig-ab-mask*)")
    # the suffix replaces the previous one instead of appending to it
    assert "*rig-shot-stem*" in RIG_GC
    assert "(define *rig-ab* #f)" in RIG_GC and "(defconstant RIG_AB_SETTLE" in RIG_GC
    # symbol values are 32 bits: masks above #x7fffffff read back negative, so never gate on them
    assert "(when *rig-ab*" in seq and not re.search(r"\(>=? \*rig-ab-(mask|a)\*", RIG_GC)


class FakeLog:
    """Answers the rig's log waits; at RIG:SHOT the game has written both frames."""

    def __init__(self, shots_dir, name):
        self.shots_dir, self.name, self.pos = shots_dir, name, 0
        self.waits = []

    def skip_to_end(self):
        pass

    def wait_for(self, pattern, timeout, fail=None, alive=None):
        self.waits.append((pattern, timeout))
        if pattern.startswith("RIG:READY"):
            return re.search(pattern, "RIG:READY x load=12")
        if pattern.startswith("RIG:CAMERA") or pattern.startswith("RIG:CAMREQ"):
            return re.search(pattern, pattern.split(" ")[0] + " " + " ".join(["3f800000"] * 12))
        if pattern == "RIG:SHOT":
            time.sleep(0.05)
            for stem, v in ((self.name, 10), (f"{self.name}--ab-b", 11), (f"{self.name}--ab-a2", 10)):
                Image.fromarray(np.full((2, 3, 3), v, np.uint8)).save(self.shots_dir / f"{stem}.png")
        return re.search(".*", pattern)


def test_shoot_copies_a_and_b(tmp_path, monkeypatch):
    shots = tmp_path / "shots"
    shots.mkdir()
    g = object.__new__(C.Game)
    g.shots_dir, g.loads, g.camfix, g.level = shots, [], None, None
    g.held_after, g.materials, g.level_classes = None, [], {}
    g.slot = type("S", (), {"index": 0})()
    g.ab = C.AbSpec(0xFFFFFFFF, 0xFFFFFFFE)
    sent = []
    g.send = sent.append
    g.alive = lambda: True
    g.gk_log = FakeLog(shots, "spot")
    monkeypatch.setattr(C, "camera_bits", lambda h: ([0.0, 0.0, 0.0], [1.0] * 9))
    monkeypatch.setattr(C, "camera_fields", lambda *a: {})
    monkeypatch.setattr(C, "git_sha", lambda: "abc")
    shot = C.Shot(name="spot", continue_point="village1-hut", hour=12.0)
    out = tmp_path / "run"
    meta = g.shoot(shot, C.ab_dirs(out)[0] / "spot.png")
    assert "(set! *rig-ab* #t) (set! *rig-ab-a* 4294967295) (set! *rig-ab-mask* 4294967294)" in " ".join(sent)
    assert sent.index(g.ab.forms()) < next(i for i, f in enumerate(sent) if f.startswith("(rig-shot "))
    assert meta["ab"] == {"a": 0xFFFFFFFF, "b": 0xFFFFFFFE, "water_clock_s": 100.0, "b_hdr": False}
    assert all((out / d / "spot.png").exists() for d in ("A", "B", "A2"))
    rows = pixdiff.compare(out / "A", out / "B")
    assert rows[0]["max"] == 1
    assert pixdiff.compare(out / "A", out / "A2")[0]["max"] == 0
    v = C.ab_report(out, g.ab)
    assert v == [{"name": "spot", "ab_max": 1, "ab_share": 0.0, "floor_max": 0, "floor_share": 0.0,
                  "verdict": "ok"}]
    assert (out / "ab-verdicts.json").exists() and (out / "pixdiff-floor.json").exists()
    # without A/B the rig is told explicitly, so a previous A/B run cannot leak into this shot
    g.ab = None
    sent.clear()
    g.gk_log = FakeLog(shots, "spot")
    g.shoot(shot, tmp_path / "plain" / "spot.png")
    assert "(set! *rig-ab* #f)" in sent


def test_wait_png_waits_for_a_fresh_file(tmp_path):
    p = tmp_path / "x.png"
    p.write_bytes(b"old")
    before = p.stat().st_mtime
    try:
        C.wait_png(p, before + 10, timeout=0.2)
        raise AssertionError("stale file accepted")
    except TimeoutError:
        pass
    C.wait_png(p, before - 1)


def test_ab_verdicts_against_the_shot_floor():
    def row(name, mx, share):
        return {"name": name, "max": mx, "share_over": share}

    ab = [row("quiet", 1, 0.0), row("rain", 90, 0.15), row("lamps", 190, 0.0009),
          row("broken", 60, 0.01), row("new-motion", 40, 0.02)]
    floor = [row("quiet", 0, 0.0), row("rain", 85, 0.149), row("lamps", 180, 0.0008),
             row("broken", 1, 0.0), row("new-motion", 30, 0.001)]
    v = {r["name"]: r["verdict"] for r in C.ab_verdicts(ab, floor)}
    assert v == {"quiet": "ok", "rain": "floor", "lamps": "floor", "broken": "fail",
                 "new-motion": "fail"}
    # no floor frame (older runs): anything over the threshold fails
    assert C.ab_verdicts([row("rain", 90, 0.15)], [])[0]["verdict"] == "fail"
