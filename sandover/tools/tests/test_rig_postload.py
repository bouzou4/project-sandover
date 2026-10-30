"""Rig forms after the level load and log waits before the settle frames."""

import pytest

from sandover.rig import capture as C

WAITING = ("[20:30] [info] VISTA: drawing beach firecanyon from village1's camera; waiting for the "
           "loader: jungle training misty village2 rolling | hour 7 viewer mood drift 46/64 | "
           "hide 'v1-lores-trees'")
DONE = ("[20:40] [info] VISTA: drawing firecanyon training beach misty jungleb village2 rolling "
        "from village1's camera | hour 23 viewer mood drift 32/64 | hide 'v1-lores-trees,v1-lores-vil2-canvas'")
NOTHING = "[info] VISTA: drawing nothing from jungle's camera; waiting for the loader: misty | hour 18 viewer mood drift -1/64 | hide ''"
MATERIALS = "[20:29] [info] materials: jungle 640 textures, 0 unknown: stone 20 wood 86 metal 85"


def test_parse_vista():
    w = C.parse_vista(WAITING)
    assert w == {"drawing": ["beach", "firecanyon"], "camera": "village1",
                 "waiting": ["jungle", "training", "misty", "village2", "rolling"]}
    d = C.parse_vista(DONE)
    assert d["waiting"] == [] and d["camera"] == "village1" and len(d["drawing"]) == 7
    n = C.parse_vista(NOTHING)
    assert n["drawing"] == [] and n["waiting"] == ["misty"] and n["camera"] == "jungle"
    assert C.parse_vista("[info] VISTA: nothing to draw: every level is drawn by the game") is None
    assert C.parse_vista("RIG:SHOT jungle-start") is None
    off = C.parse_vista("[11:02] [info] VISTA: ogre is not an outdoor vista level, neighbours off")
    assert off == {"drawing": [], "camera": "ogre", "waiting": []}
    assert C.VistaWait(None).feed("[info] VISTA: ogre is not an outdoor vista level, neighbours off")


def test_loaded_level():
    assert C.loaded_level(MATERIALS) == "jungle"
    assert C.loaded_level("[info] materials: jungle maps uploaded, 3.0 MB VRAM (all levels 9.0 MB), 4 ms") == "jungle"
    assert C.loaded_level("[info] materials: sunkenb 10 textures, 0 unknown:") == "sunkenb"
    assert C.loaded_level("[info] materials: preset x.toml look 1 applied 9 values") is None
    assert C.loaded_level("[info] materials: map index 12 textures (3 flat) from x") is None


class Clock:
    def __init__(self):
        self.t = 0.0

    def __call__(self):
        return self.t


def test_vista_wait_needs_an_empty_loader_list():
    vw = C.VistaWait(None, clock=Clock())
    assert not vw.feed(MATERIALS)
    assert not vw.feed(WAITING)
    assert not vw.idle()
    assert vw.feed(DONE)
    assert vw.last["waiting"] == []


def test_vista_wait_quiet_fallback_uses_the_last_complete_line():
    clk = Clock()
    vw = C.VistaWait(C.parse_vista(DONE), quiet=8.0, clock=clk)
    assert not vw.idle()
    clk.t = 8.5
    assert vw.idle()
    # a stale line that was still loading never satisfies the fallback
    clk2 = Clock()
    vw2 = C.VistaWait(C.parse_vista(WAITING), quiet=8.0, clock=clk2)
    clk2.t = 60
    assert not vw2.idle()
    # nor does a fresh line that is still loading
    clk3 = Clock()
    vw3 = C.VistaWait(C.parse_vista(DONE), quiet=8.0, clock=clk3)
    vw3.feed(WAITING)
    clk3.t = 60
    assert not vw3.idle()


def test_log_waiter_needs_every_condition():
    w = C.LogWaiter(r"RIG:MARK (\d+)", C.VistaWait(None, clock=Clock()))
    assert "vista" in w.what and "RIG:MARK" in w.what
    assert not w.feed(DONE)
    assert not w.feed("unrelated")
    assert w.feed("[info] RIG:MARK 7")
    r = w.result()
    assert r["ok"] and r["line"] == "[info] RIG:MARK 7" and r["vista"]["waiting"] == []
    only_log = C.LogWaiter("ready", None)
    assert not only_log.idle() and only_log.feed("all ready") and only_log.result()["ok"]


def test_log_tail_feeds_taps_and_waits(tmp_path):
    p = tmp_path / "gk.out.log"
    p.write_bytes(b"\x1b[32m" + WAITING.encode() + b"\x1b[0m\n" + MATERIALS.encode() + b"\n")
    log = C.Log(p)
    seen = []
    log.taps.append(seen.append)
    log.skip_to_end()
    assert len(seen) == 2 and "\x1b" not in seen[0]
    with open(p, "ab") as f:
        f.write(DONE.encode() + b"\npartial")
    w = C.LogWaiter(None, C.VistaWait(C.parse_vista(seen[0]), clock=Clock()))
    assert log.wait_until(w.feed, 5, w.what, tick=w.idle)
    assert seen[-1] == DONE  # the unterminated line is not read yet
    with pytest.raises(TimeoutError):
        log.wait_for("never", 0.2)


def _manifest(tmp_path, text):
    m = tmp_path / "m.yaml"
    m.write_text(text, encoding="utf-8")
    return C.load_manifest(m)[1]


def test_manifest_post_load_keys(tmp_path):
    shots = _manifest(tmp_path, (
        "set: t\n"
        "defaults:\n"
        "  goals_after_load: ['(pc-set-vista \"neighbours\")']\n"
        "  wait_vista: true\n"
        "shots:\n"
        "  - {name: a, continue: jungle-start, hour: 18}\n"
        "  - {name: b, continue: jungle-start, hour: 12, wait_vista: false, goals_after_load: '(foo)',\n"
        "     wait_log: 'RIG:X \\d+', wait_log_timeout: 30}\n"
        "  - {name: c, continue: village1-hut, hour: 12, goals_after_load: [], wait_vista: off}\n"))
    a, b, c = shots
    assert a.goals_after_load == ('(pc-set-vista "neighbours")',) and a.wait_vista and a.hold
    assert a.wait_log is None and a.wait_timeout == 120.0
    assert b.goals_after_load == ("(foo)",) and not b.wait_vista
    assert b.wait_log == r"RIG:X \d+" and b.wait_timeout == 30.0
    assert c.goals_after_load == () and not c.wait_vista and not c.hold


def test_manifest_without_post_load_keys_does_not_hold(tmp_path):
    (s,) = _manifest(tmp_path, "set: t\nshots:\n  - {name: a, continue: jungle-start, hour: 12}\n")
    assert not s.hold and s.goals_after_load == ()


def test_manifest_rejects_a_bad_regex(tmp_path):
    with pytest.raises(ValueError, match="wait_log"):
        _manifest(tmp_path, "set: t\nshots:\n  - {name: a, continue: jungle-start, hour: 12, wait_log: '('}\n")


def test_command_line_extends_the_manifest(tmp_path):
    shots = _manifest(tmp_path, (
        "set: t\n"
        "shots:\n"
        "  - {name: a, continue: jungle-start, hour: 18, goals_after_load: '(a)', wait_log: 'x', wait_log_timeout: 5}\n"
        "  - {name: b, continue: jungle-start, hour: 12}\n"))
    C.apply_post_load_args(shots, ['(pc-set-vista "neighbours")'], None, True, None)
    a, b = shots
    assert a.goals_after_load == ("(a)", '(pc-set-vista "neighbours")') and a.wait_log == "x"
    assert a.wait_timeout == 5.0 and a.wait_vista
    assert b.goals_after_load == ('(pc-set-vista "neighbours")',) and b.wait_vista and b.hold
    C.apply_post_load_args(shots, [], "RIG:Y", False, 60)
    assert all(s.wait_log == "RIG:Y" and s.wait_timeout == 60.0 and s.wait_vista for s in shots)


class FakeGame(C.Game):
    """A Game whose nREPL writes the game's answers straight into the log."""

    def __init__(self, tmp_path):
        from types import SimpleNamespace
        slot = SimpleNamespace(logs_dir=tmp_path / "logs", config_dir=tmp_path / "cfg", nrepl_port=0, index=0)
        super().__init__(slot, tmp_path)
        self.sent = []
        self.gk_log.path.write_text("", encoding="utf-8")
        self.goalc_log.path.write_text("", encoding="utf-8")

    def emit(self, *lines):
        with open(self.gk_log.path, "a", encoding="utf-8") as f:
            f.writelines(ln + "\n" for ln in lines)

    def send(self, text):
        self.sent.append(text)
        if "RIG:APPLIED" in text:
            self.emit("RIG:APPLIED", WAITING, DONE)
        elif text == "(set! *rig-hold* #f)":
            self.emit("RIG:RELEASED jungle-start")

    def alive(self):
        return True


def test_post_load_sends_forms_after_hold_and_waits_for_the_vista(tmp_path):
    g = FakeGame(tmp_path)
    g.gk_log.skip_to_end()
    g.emit(MATERIALS, "RIG:READY jungle-start load=300", "RIG:HOLD jungle-start")
    shot = C.Shot(name="temple-top-dusk", continue_point="jungle-start", hour=18,
                  goals_after_load=('(pc-set-vista "neighbours")',), wait_vista=True, wait_timeout=10)
    g.gk_log.wait_for("RIG:READY", 1)
    info = g.post_load(shot)
    assert g.sent[0] == '(pc-set-vista "neighbours") (format 0 "RIG:APPLIED~%")'
    assert g.sent[-1] == "(set! *rig-hold* #f)"
    assert info["level"] == "jungle" and info["level_loaded"]
    assert info["applied"] == ['(pc-set-vista "neighbours")']
    assert info["wait"]["ok"] and info["wait"]["vista"]["waiting"] == []


def test_render_scale_manifest_fields(tmp_path):
    m = tmp_path / "m.yaml"
    m.write_text("set: t\ndefaults: {render_scale: 0.666667, upscaler: fsr3}\n"
                 "shots:\n  - {name: a, continue: village1-hut, hour: 12}\n"
                 "  - {name: b, continue: village1-hut, hour: 12, render_scale: 1.0, upscaler: msaa}\n"
                 "  - {name: c, continue: village1-hut, hour: 12, render_scale: null, upscaler: null}\n",
                 encoding="utf-8")
    _, (a, b, c) = C.load_manifest(m)
    assert a.hold and a.render_scale_forms() == ("(pc-set-aa 1 666667)", "(pc-set-aa 0 1000000)")
    assert b.render_scale_forms() == ("(pc-set-aa 1 1000000)", "(pc-set-aa 0 0)")
    assert not c.hold and c.render_scale_forms() == ()
    bad = tmp_path / "bad.yaml"
    bad.write_text("shots:\n  - {name: a, continue: village1-hut, hour: 12, upscaler: lanczos}\n", encoding="utf-8")
    with pytest.raises(ValueError):
        C.load_manifest(bad)
