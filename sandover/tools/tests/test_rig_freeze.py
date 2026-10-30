"""Rig actor freeze, restart after a hang, wall cap, env and materials provenance, play look."""

import json
import re
from types import SimpleNamespace

import pytest

from sandover.rig import capture as C

HEX = "0 0 0 3f800000 0 0 0 3f800000 0 0 0 3f800000"
TABLE = ("[29:53] [info] materials: 4002 rows (0 bad) from /data/materials/materials.csv: "
         "stone 309 wood 444 metal 762")
PRESET = "[29:53] [info] materials: preset /root/sandover/presets/pbr.toml look bold applied 114 values"
LEVEL = "[29:57] [info] materials: jungle 640 textures, 0 unknown: stone 20 wood 86 metal 85"


def _rig() -> str:
    return (C.HERE / "rig.gc").read_text(encoding="utf-8")


def _manifest(tmp_path, text, **kw):
    m = tmp_path / "m.yaml"
    m.write_text(text, encoding="utf-8")
    return C.load_manifest(m, **kw)[1]


def test_manifest_actors_default_and_overrides(tmp_path):
    text = ("set: t\n"
            "defaults: {actors: live}\n"
            "shots:\n"
            "  - {name: a, continue: jungle-start, hour: 12}\n"
            "  - {name: b, continue: jungle-start, hour: 12, actors: freeze}\n")
    a, b = _manifest(tmp_path, text, actors="freeze")
    assert (a.actors, b.actors) == ("live", "freeze")
    (s,) = _manifest(tmp_path, "set: t\nshots:\n  - {name: a, continue: jungle-start, hour: 12}\n",
                     actors="freeze")
    assert s.actors == "freeze"
    (s,) = _manifest(tmp_path, "set: t\nshots:\n  - {name: a, continue: jungle-start, hour: 12}\n")
    assert s.actors == "live"  # perf and cost tools keep today's behaviour
    with pytest.raises(ValueError, match="actors"):
        _manifest(tmp_path, "set: t\nshots:\n  - {name: a, continue: jungle-start, hour: 12, actors: stop}\n")


def test_freeze_mask_is_the_actor_groups_only():
    m = re.search(r"\(define \*rig-freeze-mask\* \(process-mask ([\w ]+)\)\)", _rig())
    groups = set(m.group(1).split())
    assert groups == {"enemy", "platform", "projectile", "death", "crate", "ambient"}
    # never Jak, Daxter, cameras, pickups, the pause/menu bits or execute
    assert not groups & {"target", "sidekick", "camera", "collectable", "pause", "menu", "execute"}


def test_freeze_is_in_the_settle_frames_and_released_after_the_shot():
    src = _rig()
    seq = src[src.index("(defun rig-sequence ()"):src.index("(defun rig-set-options")]
    at = seq.index
    assert at("(if *rig-frozen* (rig-freeze #f))") < at("(start 'play")
    assert at("(reset-actors 'life)") < at("(rig-reset-ripples)") < at("(rig-freeze #t)")
    assert at("(rig-freeze #t)") < at("(*rig-stage-hook*)") < at("(set-master-mode 'pause)")
    assert at("(hide-hud-quick)") < at("(set-master-mode 'pause)") < at("(pc-screen-shot)")
    assert at("RIG:SHOT") < at("(rig-freeze #f)\n") < at("(set-master-mode 'game)")
    # the settle loop keeps its length in both modes
    assert len(re.findall(r"\(dotimes \(i \(-> o settle\)\)", seq)) == 1
    opts = src[src.index("(defun rig-set-options"):src.index("(defun rig-set-actors")]
    assert "(set! (-> o actors) 'live)" in opts


def test_freeze_uses_the_setting_system_and_restores():
    src = _rig()
    fz = src[src.index("(defun rig-freeze"):src.index("(defun rig-sequence")]
    assert "(set-setting! 'process-mask 'set 0.0 *rig-freeze-mask*)" in fz
    assert "(remove-setting! 'process-mask)" in fz
    assert "(apply-settings *setting-control*)" in fz
    assert fz.count("(set! *spawn-actors*") == 2
    assert "(state-flags invulnerable)" in src[src.index("(defun rig-protect-jak"):]


def test_materials_summary():
    assert C.materials_summary(TABLE) == ("table", TABLE.split("materials: ")[1])
    assert C.materials_summary(PRESET)[0] == "table"
    assert C.materials_summary("[w] materials: /x/maps.csv not found, every texture gets flat maps")[0] == "table"
    assert C.materials_summary(LEVEL) == ("level", LEVEL.split("materials: ")[1])
    assert C.materials_summary("[i] materials: jungle maps uploaded, 3.0 MB VRAM (all levels 9 MB), 4 ms") is None
    assert C.materials_summary("RIG:SHOT jungle-start") is None


class ShotGame(C.Game):
    """A Game whose nREPL answers like rig.gc: every RIG line of one shot goes into the log."""

    def __init__(self, tmp_path, ready=True):
        slot = SimpleNamespace(logs_dir=tmp_path / "logs", config_dir=tmp_path / "cfg", nrepl_port=0, index=0)
        super().__init__(slot, tmp_path)
        self.sent, self.ready = [], ready
        self.gk_log.path.write_text("", encoding="utf-8")
        self.goalc_log.path.write_text("", encoding="utf-8")
        self.shots_dir.mkdir(parents=True, exist_ok=True)
        self.emit(TABLE, PRESET)

    def emit(self, *lines):
        with open(self.gk_log.path, "a", encoding="utf-8") as f:
            f.writelines(ln + "\n" for ln in lines)

    def send(self, text):
        self.sent.append(text)
        if text.startswith("(rig-shot-setup"):
            self.emit("RIG:SHOTCFG")
        elif text.startswith("(rig-shot "):
            self.emit("RIG:SEQUENCE jungle-start")
            if not self.ready:
                return
            freeze = any(s == "(rig-set-actors 'freeze)" for s in self.sent)
            self.emit(LEVEL, "RIG:READY jungle-start load=300",
                      *(["RIG:ACTORS freeze after=20"] if freeze else []),
                      f"RIG:CAMERA {HEX}", f"RIG:CAMREQ {HEX}", "RIG:SHOT jungle-start")
            name = re.search(r'"([^"]+)"', self.sent[0]).group(1)
            (self.shots_dir / f"{name}.png").write_bytes(b"png")

    def alive(self):
        return True


def test_shoot_sends_the_mode_and_records_it(tmp_path, monkeypatch):
    monkeypatch.setenv("SANDOVER_PBR_PRESET", "presets/x.toml")
    monkeypatch.delenv("SANDOVER_MATERIALS", raising=False)
    g = ShotGame(tmp_path)
    shot = C.Shot(name="log", continue_point="jungle-start", hour=12, actors="freeze")
    meta = g.shoot(shot, tmp_path / "out" / "log.png")
    assert "(rig-set-actors 'freeze)" in g.sent
    assert g.sent.index("(rig-set-actors 'freeze)") > next(i for i, s in enumerate(g.sent)
                                                           if s.startswith("(rig-set-options"))
    assert meta["actors"] == "freeze" and meta["actors_held_after"] == 20
    assert meta["env"]["SANDOVER_PBR_PRESET"] == "presets/x.toml" and meta["env"]["SANDOVER_MATERIALS"] is None
    assert set(meta["env"]) == set(C.ENV_KEYS)
    assert meta["materials_log"]["table"][0].startswith("4002 rows (0 bad) from ")
    assert meta["materials_log"]["level"].startswith("jungle 640 textures")
    json.dumps(meta)


def test_live_shot_sends_no_actor_form(tmp_path):
    g = ShotGame(tmp_path)
    meta = g.shoot(C.Shot(name="live", continue_point="jungle-start", hour=12), tmp_path / "o" / "live.png")
    assert not any("rig-set-actors" in s for s in g.sent)
    assert meta["actors"] == "live" and "actors_held_after" not in meta


def test_no_ready_is_a_hang(tmp_path):
    g = ShotGame(tmp_path, ready=False)
    with pytest.raises(C.Hung, match="RIG:READY"):
        g.shoot(C.Shot(name="h", continue_point="jungle-start", hour=12), tmp_path / "h.png", ready_timeout=0.3)


class Boots:
    """Game factory for run_worker: each boot is a fresh fake; ``hang`` maps shot name to how
    many times it hangs."""

    def __init__(self, hang):
        self.hang, self.boots, self.killed, self.shot = dict(hang), 0, 0, []

    def __call__(self, *a, **k):
        self.boots += 1
        outer = self

        class G:
            def __enter__(self):
                return self

            def __exit__(self, *exc):
                return False

            def alive(self):
                return True

            def send(self, form):
                pass

            def kill(self):
                outer.killed += 1

            def shoot(self, shot, out_png):
                outer.shot.append(shot.name)
                if outer.hang.get(shot.name, 0) > 0:
                    outer.hang[shot.name] -= 1
                    raise C.Hung(f"no /RIG:READY \\S+/ in gk.out.log after 300s")
                return {"load_ticks": 1}

        return G()


def _run(monkeypatch, tmp_path, hang, names=("a", "b", "c"), **kw):
    boots = Boots(hang)
    monkeypatch.setattr(C, "Game", boots)
    shots = [C.Shot(name=n, continue_point="jungle-start", hour=12) for n in names]
    failed, crashed = C.run_worker(SimpleNamespace(index=3), tmp_path, shots, tmp_path, False, None, **kw)
    return boots, [s.name for s in failed], [s.name for s in crashed]


def test_hang_restarts_the_instance_and_retries_once(monkeypatch, tmp_path, capsys):
    boots, failed, crashed = _run(monkeypatch, tmp_path, {"b": 1})
    out = capsys.readouterr().out
    assert boots.boots == 2 and boots.killed == 1
    assert boots.shot == ["a", "b", "b", "c"] and failed == [] and crashed == []
    assert "RIG:RESTART slot3 after b hung; retrying it once" in out
    assert json.loads((tmp_path / "b.json").read_text(encoding="utf-8"))["restarted_after_hang"]


def test_second_hang_fails_the_shot_and_goes_on(monkeypatch, tmp_path, capsys):
    boots, failed, crashed = _run(monkeypatch, tmp_path, {"b": 2})
    assert boots.shot == ["a", "b", "b", "c"] and failed == ["b"] and crashed == []
    assert boots.boots == 3 and boots.killed == 2
    # a hang on the last shot does not boot an instance nobody uses
    boots, failed, _ = _run(monkeypatch, tmp_path, {"c": 2})
    assert failed == ["c"] and boots.boots == 2


def test_wall_cap_skips_the_rest(monkeypatch, tmp_path, capsys):
    skipped = []
    boots, failed, crashed = _run(monkeypatch, tmp_path, {}, deadline=0.0, skipped=skipped)
    assert boots.shot == [] and failed == [] and crashed == []
    assert [s.name for s in skipped] == ["a", "b", "c"]
    assert "RIG:WALL-CAP slot3 skipping 3 shots from a" in capsys.readouterr().out


def test_summary_lists_failed_and_skipped(tmp_path):
    sh = [C.Shot(name=n, continue_point="jungle-start", hour=12) for n in "abcd"]
    line = C.summary(sh, [sh[1]], [sh[2], sh[3]], tmp_path, 12.0)
    assert "1/4 shots" in line and "failed: ['b']" in line and "skipped (wall cap or play session): ['c', 'd']" in line


def test_play_look_goes_first(tmp_path):
    (s,) = _manifest(tmp_path, "set: t\nshots:\n  - {name: a, continue: jungle-start, hour: 12, "
                               "goals_after_load: '(manifest)'}\n")
    C.apply_play_look([s])
    C.apply_post_load_args([s], ["(pc-set-pbr-param 3 500000)"], None, False, None)
    assert s.goals_after_load[0] == C.PLAY_LOOK and s.hold
    assert s.goals_after_load[1:] == ("(manifest)", "(pc-set-pbr-param 3 500000)")
    look = C.PLAY_LOOK
    assert look.index("sandover-settings-reset!") < look.index("(sandover-settings-reapply)") \
        < look.index("(sandover-settings-update)")


def test_play_look_holds_the_vr_profile_for_xr_shots(tmp_path):
    a, b = _manifest(tmp_path, "set: t\nshots:\n  - {name: a, continue: jungle-start, hour: 12}\n"
                               "  - {name: b, continue: jungle-start, hour: 12, goals_after_load: '(pc-set-xr #t)'}\n")
    C.apply_play_look([a, b])
    assert a.goals_after_load[0] == C.PLAY_LOOK and b.goals_after_load[0] == C.PLAY_LOOK_VR
    (c,) = _manifest(tmp_path, "set: t\nshots:\n  - {name: c, continue: jungle-start, hour: 12}\n")
    C.apply_play_look([c], ["(pc-set-xr #t)"])
    look = c.goals_after_load[0]
    assert look == C.PLAY_LOOK_VR
    assert look.index("(set! *sandover-profile-force* 'vr)") < look.index("(sandover-settings-update)") \
        < look.index("(sandover-settings-reset-vr! (-> *pc-settings* sandover))") \
        < look.index("(sandover-settings-reapply)")
