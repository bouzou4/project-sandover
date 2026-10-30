"""Rig cutscenes: ended by default on every shot, allowed per shot or with --cutscenes."""

import json

from sandover.rig import capture as C
from test_rig_freeze import ShotGame, _manifest, _rig


def test_watcher_runs_before_the_start_and_options_reset_the_allowance():
    src = _rig()
    seq = src[src.index("(defun rig-sequence ()"):src.index("(defun rig-set-options")]
    assert seq.index("(rig-cutscene-watch self)") < seq.index("(start 'play")
    opts = src[src.index("(defun rig-set-options"):src.index("(defun rig-set-actors")]
    assert "(set! *rig-cutscenes* #f)" in opts
    assert "(define *rig-cutscenes* #f)" in src


def test_watcher_ends_taskable_and_pov_camera_movies():
    src = _rig()
    end = src[src.index("(defun rig-end-cutscenes"):src.index("(defun rig-cutscene-watch")]
    assert "(set! (-> pt query decision) 'no)" in end
    assert "(pov-camera-flag notify-of-abort allow-abort)" in end and "(send-event pv 'abort)" in end
    watch = src[src.index("(defun rig-cutscene-watch"):src.index("(defun rig-mode")]
    # runs through movies and only while the shot does not allow them
    assert "(process-mask pause menu progress movie)" in watch
    assert "(when (not *rig-cutscenes*)" in watch and ":to parent" in watch


def test_manifest_cutscenes(tmp_path):
    text = ("set: t\n"
            "shots:\n"
            "  - {name: a, continue: jungle-start, hour: 12}\n"
            "  - {name: b, continue: jungle-start, hour: 12, cutscenes: true}\n")
    a, b = _manifest(tmp_path, text)
    assert (a.cutscenes, b.cutscenes) == (False, True)


def test_default_shot_sends_no_allowance_and_records_skip(tmp_path):
    g = ShotGame(tmp_path)
    g.emit()
    meta = g.shoot(C.Shot(name="d", continue_point="jungle-start", hour=12), tmp_path / "o" / "d.png")
    assert not any("rig-set-cutscenes" in s for s in g.sent)
    assert meta["cutscenes"] == "skip" and meta["cutscenes_ended"] == []
    assert set(meta["seconds"]) == {"ready", "shot"}
    json.dumps(meta)


def test_allowed_shot_sends_the_form_after_the_options(tmp_path):
    g = ShotGame(tmp_path)
    meta = g.shoot(C.Shot(name="c", continue_point="jungle-start", hour=12, cutscenes=True),
                   tmp_path / "o" / "c.png")
    opts = next(i for i, s in enumerate(g.sent) if s.startswith("(rig-set-options"))
    assert g.sent.index("(rig-set-cutscenes #t)") > opts
    assert meta["cutscenes"] == "allow"


def test_cutscene_lines_go_to_the_sidecar(tmp_path):
    g = ShotGame(tmp_path)
    orig = g.send

    def send(text):
        if text.startswith("(rig-shot "):
            g.emit('RIG:CUTSCENE "citadel-plat-start" taskable "green-sagecage-1" play-anim')
        orig(text)

    g.send = send
    meta = g.shoot(C.Shot(name="e", continue_point="jungle-start", hour=12), tmp_path / "o" / "e.png")
    assert meta["cutscenes_ended"] == ['taskable "green-sagecage-1" play-anim']
