"""Play-session guard (--while-playing) and the rig's no-controller environment."""

import json
from pathlib import Path
from types import SimpleNamespace

import pytest

from sandover import locks
from sandover.rig import capture as C
from sandover.rig import playguard as G

PLAY_BACK = r"data\config\play"
SLOT = r'"data\bin\x\gk.exe" -v --game jak1 --config-path data\config\slot2 --port 8132 -- -boot'


@pytest.mark.parametrize("cmd", [
    rf"data\bin\x\gk.exe -v --game jak1 --config-path {PLAY_BACK} -- -boot -fakeiso -debug",
    "/data/bin/x/gk -v --game jak1 --config-path /data/config/play -- -boot",
    r'"my data\bin\gk.exe" -v --config-path "my data\config\play" --audio-spatial binaural',
    r'gk.exe --config-path "my data\config\play\\" --port 8112',
    r"gk.exe --CONFIG-PATH Data\CONFIG\Play",
    r"gk.exe --config-path=data\config/play/ -- -boot",
    "gk --config-path '/srv/my data/config/play' -- -boot",
])
def test_play_session_matches(cmd):
    assert G.is_play_session(cmd)


@pytest.mark.parametrize("cmd", [
    SLOT,
    "/data/bin/x/gk -v --config-path /data/config/slot7 -- -boot",
    r"play\bin\gk.exe -v --config-path play\config\slot1 -- -boot",  # "play" elsewhere
    r"data\config\play\gk.exe -v --config-path data\config\slot1",
    r"gk.exe --config-path data\config\play-old --port 8112",
    r"gk.exe --config-path data\config\playground",
    r"gk.exe --config-path data\play --port 8112",
    r"gk.exe -v --game jak1 -- -boot -fakeiso -debug",  # no --config-path
    "",
])
def test_other_gk_does_not_match(cmd):
    assert not G.is_play_session(cmd)


def test_config_path_forms():
    assert G.config_path(r'gk --config-path "a b\config\play" --port 1') == r"a b\config\play"
    assert G.config_path("gk --config-path=/x/config/play") == "/x/config/play"
    assert G.config_path("gk --port 1") is None


class Clock:
    def __init__(self):
        self.t = 0.0
        self.sleeps = []

    def __call__(self):
        return self.t

    def sleep(self, s):
        self.sleeps.append(s)
        self.t += s


def _guard(mode, listings, max_wait=None, **kw):
    """A guard whose process listings come from ``listings`` (one per call; the last repeats)."""
    clock, said, calls = Clock(), [], []

    def lister():
        calls.append(clock.t)
        return listings[min(len(calls), len(listings)) - 1]

    g = G.PlayGuard(mode, max_wait, lister=lister, clock=clock, sleep=clock.sleep, say=said.append, **kw)
    return g, clock, said, calls


PLAYING = [(4242, rf"gk.exe --config-path {PLAY_BACK}"), (77, SLOT)]
CLEAR = [(77, SLOT)]


def test_wait_polls_until_the_session_ends():
    g, clock, said, calls = _guard("wait", [PLAYING, PLAYING, PLAYING, CLEAR])
    assert g.wait("slot2 hut") == {"detected": False, "waited_s": 90.0}
    assert clock.sleeps == [30.0, 30.0, 30.0]
    assert len(said) == 2
    assert said[0].startswith("RIG:PLAY-WAIT slot2 hut") and "4242" in said[0] and "77" not in said[0]
    assert said[1] == "RIG:PLAY-RESUME slot2 hut after 90s"


def test_wait_without_a_session_returns_at_once():
    g, clock, said, _ = _guard("wait", [CLEAR])
    assert g.wait("x") == {"detected": False, "waited_s": 0.0}
    assert clock.sleeps == [] and said == []


def test_wait_stops_at_the_wall_cap():
    g, clock, said, _ = _guard("wait", [PLAYING])
    assert g.wait("x", deadline=45.0) == {"detected": True, "waited_s": 45.0}
    assert clock.sleeps == [30.0, 15.0]
    assert "wall cap" in said[-1]


def test_wait_gives_up_after_max_play_wait():
    g, clock, _, _ = _guard("wait", [PLAYING], max_wait=50.0)
    with pytest.raises(G.PlaySessionActive, match="max-play-wait"):
        g.wait("x")
    assert clock.sleeps == [30.0, 20.0]
    # the wall cap comes first: a skip like any other wall cap, not a failure
    g, _, _, _ = _guard("wait", [PLAYING], max_wait=50.0)
    assert g.wait("x", deadline=40.0)["detected"] is True


def test_fail_raises_and_never_waits():
    g, clock, _, _ = _guard("fail", [PLAYING])
    with pytest.raises(G.PlaySessionActive, match="4242"):
        g.wait("the run")
    assert clock.sleeps == []
    g, _, _, _ = _guard("fail", [CLEAR])
    assert g.wait("the run")["detected"] is False


def test_ignore_records_only():
    g, clock, said, _ = _guard("ignore", [PLAYING])
    assert g.wait("x") == {"detected": True, "waited_s": 0.0}
    assert clock.sleeps == [] and said == []
    assert g.check() is True


def test_unlistable_processes_are_unknown_and_warned_once():
    g, clock, said, _ = _guard("fail", [None])
    assert g.wait("a") == {"detected": None, "waited_s": 0.0}
    clock.t += 10
    assert g.check() is None
    assert len(said) == 1 and "cannot list" in said[0]


def test_listing_is_cached_briefly():
    g, clock, _, calls = _guard("ignore", [CLEAR])
    g.check()
    g.check()
    assert len(calls) == 1
    clock.t += G.CACHE_S
    g.check()
    assert len(calls) == 2


def test_bad_mode():
    with pytest.raises(ValueError):
        G.PlayGuard("pause")


def test_sidecar_field():
    assert G.sidecar({"detected": False, "waited_s": 0.0}, False)["detected"] is False
    assert G.sidecar({"detected": False, "waited_s": 31.0}, True) == {
        "detected": True, "before": False, "after": True, "waited_s": 31.0}
    assert G.sidecar({"detected": None, "waited_s": 0.0}, False)["detected"] is None
    assert G.sidecar({"detected": None, "waited_s": 0.0}, True)["detected"] is True


class FakeGame:
    def __init__(self, *a, **k):
        pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def alive(self):
        return True

    def send(self, form):
        pass

    def shoot(self, shot, out_png):
        return {"load_ticks": 1}


def _run(monkeypatch, tmp_path, guard, **kw):
    monkeypatch.setattr(C, "Game", FakeGame)
    shots = [C.Shot(name=n, continue_point="jungle-start", hour=12) for n in "abc"]
    skipped = []
    failed, crashed = C.run_worker(SimpleNamespace(index=3), tmp_path, shots, tmp_path, False, None,
                                   skipped=skipped, guard=guard, **kw)
    return [s.name for s in failed], [s.name for s in crashed], [s.name for s in skipped]


def test_worker_fail_mode_skips_everything(monkeypatch, tmp_path, capsys):
    g, *_ = _guard("fail", [PLAYING])
    assert _run(monkeypatch, tmp_path, g) == ([], [], ["a", "b", "c"])
    assert "RIG:PLAYING slot3 skipping 3 shots" in capsys.readouterr().out
    assert not list(tmp_path.glob("*.json"))


def test_worker_ignore_marks_sidecars(monkeypatch, tmp_path, capsys):
    g, *_ = _guard("ignore", [PLAYING])
    assert _run(monkeypatch, tmp_path, g) == ([], [], [])
    meta = json.loads((tmp_path / "b.json").read_text(encoding="utf-8"))
    assert meta["play_session"] == {"detected": True, "before": True, "after": True, "waited_s": 0.0}
    assert "RIG:PLAYING b: a play session was running at capture time" in capsys.readouterr().out


def test_worker_wait_then_capture_clean(monkeypatch, tmp_path):
    # playing at the instance start, gone after one poll
    g, clock, said, _ = _guard("wait", [PLAYING, CLEAR])
    assert _run(monkeypatch, tmp_path, g) == ([], [], [])
    assert clock.sleeps == [30.0] and said[-1].startswith("RIG:PLAY-RESUME slot3 start")
    meta = json.loads((tmp_path / "a.json").read_text(encoding="utf-8"))
    assert meta["play_session"]["detected"] is False


def test_worker_wait_hits_the_wall_cap(monkeypatch, tmp_path, capsys):
    # instance start, a and its after-check are clear, then play starts: b waits until the wall
    # cap, then the rest is skipped as any wall cap is
    g, clock, said, _ = _guard("wait", [CLEAR, CLEAR, CLEAR, PLAYING], cache_s=0.0)
    clock.t = 1000.0
    monkeypatch.setattr(C.time, "monotonic", clock)
    failed, crashed, skipped = _run(monkeypatch, tmp_path, g, deadline=1100.0)
    assert (failed, crashed, skipped) == ([], [], ["b", "c"])
    assert clock.sleeps == [30.0, 30.0, 30.0, 10.0]
    assert "wall cap" in said[-1]
    assert "RIG:WALL-CAP slot3 skipping 2 shots from b" in capsys.readouterr().out


def test_run_shots_fail_mode_starts_nothing(monkeypatch, tmp_path, capsys):
    monkeypatch.setattr(C.engine, "staged_bin", lambda: pytest.fail("staged binaries while playing"))
    g, *_ = _guard("fail", [PLAYING])
    shots = [C.Shot(name="a", continue_point="jungle-start", hour=12)]
    assert C.run_shots("t", shots, tmp_path, guard=g) == 1
    out = capsys.readouterr().out
    assert "RIG:PLAYING not starting" in out and "skipped (wall cap or play session): ['a']" in out


def test_rig_gk_opens_no_gamepad_and_takes_no_focus():
    env = C.gk_env(locks.Slot(2), "/root")
    assert env["SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT"] == "0x0000/0x0001"
    assert env["SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS"] == "0"
    assert env["SDL_WINDOW_ACTIVATE_WHEN_SHOWN"] == "0"
    assert env["SDL_WINDOW_ACTIVATE_WHEN_RAISED"] == "0"
    assert env["OPENGOAL_PROJ_PATH"] == "/root"
    assert env["OPENGOAL_LOG_DIR"] == str(locks.Slot(2).logs_dir / "engine")


def test_run_shots_wall_cap_during_the_first_wait_boots_nothing(monkeypatch, tmp_path, capsys):
    monkeypatch.setattr(C.engine, "staged_bin", lambda: pytest.fail("staged binaries after the wall cap"))
    g, clock, _, _ = _guard("wait", [PLAYING])
    monkeypatch.setattr(C.time, "monotonic", clock)
    shots = [C.Shot(name="a", continue_point="jungle-start", hour=12)]
    assert C.run_shots("t", shots, tmp_path, max_seconds=60, guard=g) == 1
    assert "wall cap passed" in capsys.readouterr().out


def test_listing_failure_during_a_wait_keeps_waiting():
    g, clock, said, _ = _guard("wait", [PLAYING, None, None, CLEAR])
    assert g.wait("x") == {"detected": False, "waited_s": 90.0}
    assert clock.sleeps == [30.0, 30.0, 30.0]


def test_unreadable_command_line_is_unknown_not_absent():
    # an elevated gk hides its command line from a normal process
    g, *_ = _guard("fail", [[(4242, ""), (77, SLOT)]])
    assert g.wait("x") == {"detected": None, "waited_s": 0.0}
    g, *_ = _guard("fail", [[(4242, ""), *PLAYING]])
    with pytest.raises(G.PlaySessionActive):
        g.wait("x")


class CountingGame(FakeGame):
    built = 0
    closed = 0

    def __init__(self, *a, **k):
        CountingGame.built += 1

    def __exit__(self, *exc):
        CountingGame.closed += 1
        return False


def test_wall_cap_during_the_start_wait_builds_no_game(monkeypatch, tmp_path, capsys):
    CountingGame.built = 0
    g, clock, _, _ = _guard("wait", [PLAYING])
    clock.t = 1000.0
    monkeypatch.setattr(C.time, "monotonic", clock)
    monkeypatch.setattr(C, "Game", CountingGame)
    shots = [C.Shot(name=n, continue_point="jungle-start", hour=12) for n in "ab"]
    skipped = []
    compiled = C.threading.Event()
    failed, crashed = C.run_worker(SimpleNamespace(index=0), tmp_path, shots, tmp_path, True, compiled,
                                   deadline=1045.0, skipped=skipped, guard=g)
    assert CountingGame.built == 0
    assert (failed, crashed, [s.name for s in skipped]) == ([], [], ["a", "b"])
    assert compiled.is_set()
    assert "RIG:PLAYING slot0 skipping 2 shots: wall cap reached" in capsys.readouterr().out


def test_long_wait_stops_the_instance_and_boots_it_again(monkeypatch, tmp_path, capsys):
    CountingGame.built = CountingGame.closed = 0
    # start and a clear; b waits 90 s: the instance stops at 60 s and boots again after resume
    g, clock, said, _ = _guard("wait", [CLEAR, CLEAR, CLEAR, PLAYING, PLAYING, PLAYING, CLEAR], cache_s=0.0)
    monkeypatch.setattr(C, "Game", CountingGame)
    shots = [C.Shot(name=n, continue_point="jungle-start", hour=12) for n in "ab"]
    failed, crashed = C.run_worker(SimpleNamespace(index=1), tmp_path, shots, tmp_path, False, None, guard=g)
    assert (failed, crashed) == ([], [])
    assert CountingGame.built == 2 and CountingGame.closed == 2
    assert "stopping the idle instance" in capsys.readouterr().out
    assert json.loads((tmp_path / "b.json").read_text(encoding="utf-8"))["play_session"]["waited_s"] == 90.0


def test_game_starts_gk_with_the_input_env(monkeypatch, tmp_path):
    monkeypatch.setenv("SANDOVER_DATA", str(tmp_path))
    calls = []

    class Proc:
        def __init__(self, args, **kw):
            calls.append((args, kw))

        def poll(self):
            return 0

    monkeypatch.setattr(C.subprocess, "Popen", Proc)
    monkeypatch.setattr(C.Log, "wait_for", lambda self, *a, **k: True)
    monkeypatch.setattr(C.Game, "send", lambda self, text: None)
    monkeypatch.setattr(C.Game, "make", lambda self: None)
    monkeypatch.setattr(C.locks, "holder", lambda name: None)
    with C.Game(locks.Slot(2), tmp_path / "bin"):
        pass
    gk = [kw for args, kw in calls if Path(args[0]).stem == "gk"]
    goalc = [kw for args, kw in calls if Path(args[0]).stem == "goalc"]
    assert len(gk) == 1 and len(goalc) == 1
    for k, v in C.RIG_INPUT_ENV.items():
        assert gk[0]["env"][k] == v
        assert k not in goalc[0]["env"] or goalc[0]["env"][k] == C.os.environ.get(k)
    assert gk[0]["env"]["SDL_WINDOW_ACTIVATE_WHEN_RAISED"] == "0"
