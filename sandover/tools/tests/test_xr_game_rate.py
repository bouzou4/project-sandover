"""The game clock in an XR session: GOAL's seconds per frame equals the XR tick period.

``set-time-ratios`` (display.gc) and ``pc-game-fps`` (pckernel-h.gc) are evaluated from the GOAL
source by a small evaluator over the forms they use; ``pc-get-xr-game-rate`` is mocked with the
engine's tick rate (``Gfx::kXrGameHz``) while a session paces the game and 0 otherwise.
"""

import re
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
DISPLAY = REPO / "goal_src" / "jak1" / "engine" / "gfx" / "hw" / "display.gc"
PCKERNEL_H = REPO / "goal_src" / "jak1" / "pc" / "pckernel-h.gc"
GFX_H = REPO / "game" / "graphics" / "gfx.h"
OPENGL = REPO / "game" / "graphics" / "pipelines" / "opengl.cpp"

XR_HZ = int(re.search(r"constexpr int kXrGameHz = (\d+);", GFX_H.read_text(encoding="utf-8")).group(1))


class Sym(str):
    pass


def _read(text: str):
    text = re.sub(r";[^\n]*", "", text)
    toks = re.findall(r"\"[^\"]*\"|[()'`]|[^\s()'`]+", text)
    pos = 0

    def one():
        nonlocal pos
        t = toks[pos]
        pos += 1
        if t == "(":
            out = []
            while toks[pos] != ")":
                out.append(one())
            pos += 1
            return out
        if t == "'":
            return ["quote", one()]
        if t == "`":
            return ["quasiquote", one()]
        if t.startswith('"'):
            return t[1:-1]
        try:
            return float(t) if "." in t else int(t)
        except ValueError:
            return Sym(t)

    forms = []
    while pos < len(toks):
        forms.append(one())
    return forms


def _find(forms, head: str, name: str):
    for f in forms:
        if isinstance(f, list) and len(f) > 2 and f[0] == head and f[1] == name:
            return f
    raise KeyError(name)


class Goal:
    def __init__(self, target_fps: int, video_mode: str, xr_rate: int):
        self.target_fps, self.video_mode, self.xr_rate = target_fps, video_mode, xr_rate
        self.funcs = {}
        self.display = {}

    def call(self, name, args):
        if name == "pc-get-xr-game-rate":
            return self.xr_rate
        if name == "get-video-mode":
            return Sym(self.video_mode)
        if name == "format":
            raise AssertionError("unsupported video mode")
        params, body = self.funcs[name]
        env = dict(zip(params, args))
        out = None
        for x in body:
            out = self.ev(x, env)
        return out

    def ev(self, x, env):
        if isinstance(x, Sym):
            return env[x]
        if not isinstance(x, list):
            return x
        op, args = x[0], x[1:]
        if op == "quote":
            return Sym(args[0])
        if op == "let":
            env = dict(env)
            for name, val in args[0]:
                env[name] = self.ev(val, env)
            out = None
            for b in args[1:]:
                out = self.ev(b, env)
            return out
        if op == "if":
            return self.ev(args[1], env) if self.ev(args[0], env) else self.ev(args[2], env)
        if op == "nonzero?":
            return self.ev(args[0], env) != 0
        if op == "->":
            if args[0] == "*pc-settings*" and args[1] == "target-fps":
                return self.target_fps
            assert args[0] == "this"
            return self.display[args[1]]
        if op == "set!":
            assert args[0][0] == "->" and args[0][1] == "this"
            self.display[args[0][2]] = self.ev(args[1], env)
            return None
        if op == "case":
            key = self.ev(args[0], env)
            for clause in args[1:]:
                if clause[0] == "else" or any(self.ev(k, env) == key for k in clause[0]):
                    out = None
                    for b in clause[1:]:
                        out = self.ev(b, env)
                    return out
            return None
        if op == "the":
            return float(self.ev(args[1], env))
        if op == "fmin":
            return min(self.ev(args[0], env), self.ev(args[1], env))
        if op in ("*", "/"):
            a, b = (float(self.ev(v, env)) for v in args)
            return a * b if op == "*" else a / b
        return self.call(op, [self.ev(a, env) for a in args])


def _goal(target_fps: int, xr_rate: int) -> Goal:
    mode = {50: "pal", 60: "ntsc"}.get(target_fps, "custom")
    g = Goal(target_fps, mode, xr_rate)
    # a macro whose expansion is its quasiquoted body (no unquotes)
    fps = _find(_read(PCKERNEL_H.read_text(encoding="utf-8")), "defmacro", "pc-game-fps")
    g.funcs["pc-game-fps"] = (fps[2], [b[1] for b in fps[3:] if isinstance(b, list) and b[0] == "quasiquote"])
    m = _find(_read(DISPLAY.read_text(encoding="utf-8")), "defmethod", "set-time-ratios")
    g.funcs["set-time-ratios"] = ([Sym(p[0]) for p in m[2]], [b for b in m[3:] if not isinstance(b, str)])
    return g


def _ratios(target_fps: int, xr_rate: int, slowdown: float = 1.0) -> dict:
    g = _goal(target_fps, xr_rate)
    g.call("set-time-ratios", [None, slowdown])
    return g.display


@pytest.mark.parametrize("target", [50, 60, 75, 120, 144, 165, 240])
def test_xr_session_steps_the_game_at_the_xr_tick(target):
    d = _ratios(target, XR_HZ)
    assert d["seconds-per-frame"] == pytest.approx(1.0 / XR_HZ)
    assert d["frames-per-second"] == pytest.approx(XR_HZ)
    assert d["time-factor"] == pytest.approx(300.0 / XR_HZ)
    assert d["time-adjust-ratio"] == pytest.approx(60.0 / XR_HZ)
    # a lag frame still steps twice the period
    assert _ratios(target, XR_HZ, 2.0)["seconds-per-frame"] == pytest.approx(2.0 / XR_HZ)


@pytest.mark.parametrize("target,spf,factor", [(50, 0.02, 6.0), (60, 0.016666668, 5.0), (75, 1 / 75, 4.0),
                                               (120, 1 / 120, 2.5), (165, 1 / 165, 300 / 165)])
def test_flat_screen_timing_is_unchanged(target, spf, factor):
    d = _ratios(target, 0)
    assert d["seconds-per-frame"] == pytest.approx(spf)
    assert d["time-factor"] == pytest.approx(factor)


def test_the_tick_and_the_rate_goal_reads_are_one_constant():
    src = OPENGL.read_text(encoding="utf-8")
    tick = src[src.index("bool GLDisplay::xr_game_tick() {"):]
    tick = tick[:tick.index("\n}\n")]
    assert "duration<double>(1. / Gfx::kXrGameHz)" in tick
    # the rate is published where the display switches to XR pacing (xr_game_tick drives the vsync)
    assert "Gfx::g_xr_game_hz = xr_paced ? Gfx::kXrGameHz : 0;" in src
    assert "if (!xr_paced || xr_game_tick()) {" in src


def test_frame_rate_readers_use_the_game_rate():
    pad = (REPO / "goal_src" / "jak1" / "engine" / "ps2" / "pad.gc").read_text(encoding="utf-8")
    seg = pad[pad.index(";; og:preserve-this high fps fix"):]
    seg = seg[:seg.index("(set! (-> history button0-abs 2)")]
    assert "target-fps" not in seg and seg.count("(pc-game-fps)") == 6
    swamp = (REPO / "goal_src" / "jak1" / "levels" / "swamp" / "swamp-obs.gc").read_text(encoding="utf-8")
    assert "(= (pc-game-fps) 60) ;; always run untouched if 60 FPS" in swamp
    for f in ("game/overlord/jak1/srpc.cpp", "game/graphics/opengl_renderer/sprite/GlowRenderer.cpp"):
        text = (REPO / f).read_text(encoding="utf-8")
        assert "Gfx::game_fps()" in text and "g_global_settings.target_fps" not in text, f


def test_lag_compensation_measures_against_the_game_rate():
    src = (REPO / "goal_src" / "jak1" / "engine" / "draw" / "drawable.gc").read_text(encoding="utf-8")
    assert "(* 1953.125 (/ 300.0 (pc-game-fps)))" in src
    assert "(* 1953.125 (-> disp time-factor))" not in src
