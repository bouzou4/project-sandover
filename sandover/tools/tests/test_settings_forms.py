"""The play look's engine forms, evaluated from the GOAL source.

``sandover-settings-reset!``, ``sandover-settings-reapply`` and ``sandover-settings-update`` are run
by a small evaluator over the subset of GOAL they use, recording every ``pc-set-*`` call in order.
The recorded forms are compared with the ones master sent before the settings page had tier rows
(``MASTER_*`` below, taken with this evaluator from master's ``pckernel.gc``): the defaults must push
the same forms byte for byte, and reapply may only add the resets of the new sub-options.
"""

import re
from pathlib import Path

import pytest

from sandover import settings_table
from sandover.bridge_sources import bridge_text
from sandover.settings_table import Sym, read_forms

REPO = Path(__file__).resolve().parents[3]
PCKERNEL = REPO / "goal_src" / "jak1" / "pc" / "pckernel.gc"
IMPL = REPO / "goal_src" / "jak1" / "pc" / "pckernel-impl.gc"
PC_DEFAULT_MSAA = int(re.search(r"^\(defconstant PC_DEFAULT_MSAA (\d+)\)",
                                (REPO / "goal_src" / "jak1" / "pc" / "pckernel-h.gc").read_text(encoding="utf-8"),
                                flags=re.M).group(1))

#: what master's reapply sent (its non-field resets), in order, minus the SMAA and FSR 1 resets
#: (pc-set-smaa 4, pc-set-render-scale 3), removed with those passes
MASTER_REAPPLY = [
    "(pc-set-lava 15 1)",
    "(pc-set-emissive-lights 37 1)",
    "(pc-set-shadow-option 20 1000000)",
    "(pc-set-sun-option 19 1)",
]

#: what master's first settings update sent after reset! and reapply (the play look), in order
MASTER_PLAY = [
    "(pc-set-sandover-path 1)",  # the render path (world sprite order), new since master
    "(pc-set-ssao-preset 1)",
    "(pc-set-ssao-param 15 1000000)",
    "(pc-set-emissive-lights 0 1000000)",
    "(pc-set-bloom 0 1000000)",
    "(pc-set-bloom 4 1000000)",
    "(pc-set-shadows 1 1.0)",
    "(pc-set-shadow-option 5 300000)",
    "(pc-set-shadow-option 30 1000000)",
    "(pc-set-pbr-param 11 2000000)",
    "(pc-set-pbr-param 0 1)",
    "(pc-set-hide-shafts 1)",  # the remaster look hides the fake light shafts, new since master
    "(pc-set-fur 0 1000000)",
    "(pc-set-water 0 1000000)",
    "(pc-set-water 94 1000000)",  # the glint row, new since master
    "(pc-set-motion-blur 0 2000000)",  # the motion blur row (medium), new since master
    "(pc-set-motion-blur 10 1000000)",  # camera motion blur (vehicles), new since master
    '(pc-set-vista-fog "unified" 150 150 800)',
    '(pc-set-vista-neighbour-fog "clear" -1 -1 -1)',
    "(pc-set-vista-haze 850 200 9277312 700)",
    '(pc-set-vista-neighbour-fog "misty" 16000 8025969 -1)',
    '(pc-set-vista-neighbour-haze "misty" 0)',
    '(pc-set-vista-neighbour-fog "rolling" 10000 6973816 -1)',
    '(pc-set-vista-neighbour-fog "village2" 16000 6645121 -1)',
    '(pc-set-vista-neighbour-fog "training" 7000 7762801 -1)',
    '(pc-set-vista-neighbour-haze "training" 0)',
    "(pc-set-vista-option 5 <time-of-day>)",
    "(pc-set-vista-option 12 1)",
    '(pc-set-vista "neighbours")',
    "(pc-set-sun-option 10 1000000)",
    "(pc-set-hdr-target #t)",
    "(pc-set-tonemap 0 2000000)",
    "(pc-set-hdr-output 1 1000000)",
    "(pc-set-hdr-output 0 1000000)",
    "(pc-set-aa 0 1000000)",  # temporal AA (FSR 3.1), the Sandover path since the TAA-only decision
    "(pc-set-aa 1 1000000)",  # the render scale, re-pushed with the mode: reapply forgets both
    '(pc-set-audio-output 0 "")',  # the Audio page: auto on the Windows default, new since master
]

#: reapply resets of the sub-options added with the tier rows, of the world sun map (32-38) and of
#: the water glint's parameters (95-97) and of the sky bodies (sun options 20-28)
NEW_REAPPLY = ["(pc-set-shadow-option 40 2000000)", "(pc-set-shadow-option 39 1)",
               "(pc-set-sun-option 29 1)",
               "(pc-set-water 98 1)", "(pc-set-aa 5 1)", "(pc-set-shadow-option 41 0)",
               "(pc-set-motion-blur 7 1)", "(pc-set-motion-blur 10 0)", "(pc-set-motion-blur 11 0)"]


# ---- evaluator --------------------------------------------------------------------------------


class Struct(dict):
    pass


class Zero(Struct):
    """A static structure: the fields a form leaves out are 0."""

    def __missing__(self, key):
        return 0


LAYOUT = {v: k for k, v in settings_table.layout().items()}


class Addr:
    """``(+ (the-as int obj) offset)``: the field of ``obj`` at that byte offset of sandover-settings."""

    def __init__(self, obj, field):
        self.obj, self.field = obj, field

    def get(self):
        return self.obj[self.field]

    def set(self, v):
        self.obj[self.field] = v

    def array(self):
        return self.obj.setdefault(self.field, Struct())  # an inline byte array: (-> ptr 0)


def _fmt(v) -> str:
    if v is True:
        return "#t"
    if v is False:
        return "#f"
    if isinstance(v, str) and not isinstance(v, Sym):
        return f'"{v}"'
    return str(v)


class Goal:
    def __init__(self, sources: list[str], gfx_msaa: int | None = None):
        self.defuns: dict[str, list] = {}
        self.macros: dict[str, list] = {}
        self.constants: dict[str, object] = {}
        for text in sources:
            for f in read_forms(text):
                if isinstance(f, list) and f and f[0] == "defun":
                    self.defuns[f[1]] = f
                elif isinstance(f, list) and f and f[0] == "defmethod":
                    self.defuns[f"method:{f[1]}"] = f
                elif isinstance(f, list) and f and f[0] == "defmacro":
                    self.macros[f[1]] = f
                elif isinstance(f, list) and f and f[0] == "defconstant":
                    self.constants[f[1]] = f[2]
        self.calls: list[str] = []
        self.temporal_state = 1  # pc-temporal-aa-state: 0 unavailable, 1 runs, 2 stereo or XR
        self.xr_state = [0, 0, 0]  # pc-get-xr-state: session runs, eye frame width, height
        self.hooks: dict = {}  # defuns replaced by Python (the file readers and writers)
        self.files: dict[str, dict] = {}  # what pc-filepath-exists? sees
        self.globals = {
            **self.constants,
            "*pc-settings*": Struct(sandover=Struct(), **{"sandover-other": Struct()},
                                    **{"gfx-msaa": PC_DEFAULT_MSAA if gfx_msaa is None else gfx_msaa}),
            "PC_DEFAULT_MSAA": PC_DEFAULT_MSAA,
            "*sandover-profile*": Sym("flat"),
            "*sandover-profile-force*": False,
            "*sandover-menu-editing-func*": False,
            "*sandover-settings-loaded?*": False,
            "*pc-settings-folder*": "cfg",
            "*pc-temp-string-1*": "",
            "*sandover-applied*": Struct(),
            "*sandover-calibration-pattern*": 0,
            "*sandover-calibration-pattern-func*": False,
            "*sandover-path-sent*": -1,
            "*sandover-vehicle*": 0,
            "*sandover-audio-sent*": -1,
            "*sandover-audio-cli*": -2,
            "*sandover-audio-device-str*": "",
            "*sandover-audio-device-sent*": "",
            "*target*": False,
        }
        self.audio_cli = -1  # pc-get-audio-state 4: the command line's output, -1 none
        for text in sources:
            for f in read_forms(text):
                if isinstance(f, list) and f[:2] == ["define", "*sandover-fields*"]:
                    self.globals["*sandover-fields*"] = self.ev(f[2], {})

    def call(self, name: str, *args):
        f = self.defuns[name]
        params = [p[0] if isinstance(p, list) else p for p in f[2]]
        env = dict(zip(params, args))
        body = f[3:]
        if body and isinstance(body[0], str) and not isinstance(body[0], Sym):
            body = body[1:]  # docstring
        return self.block(body, env)

    def block(self, body, env):
        v = None
        for x in body:
            v = self.ev(x, env)
        return v

    def lookup(self, s: Sym, env):
        if s in env:
            return env[s]
        if s in self.globals:
            return self.globals[s]
        raise KeyError(s)

    def deref(self, obj, path, env):
        for p in path:
            if isinstance(obj, list):  # a boxed array: (-> arr length), (-> arr index)
                obj = len(obj) if p == "length" else obj[self.ev(p, env)]
            elif isinstance(obj, Addr):
                obj = obj.array()[str(p)]
            else:
                obj = obj[str(p)]
        return obj.get() if isinstance(obj, Addr) else obj

    def ev(self, x, env):
        if isinstance(x, Sym):
            return None if x == "none" else self.lookup(x, env)
        if not isinstance(x, list):
            return x
        op, args = x[0], x[1:]
        if isinstance(op, list) and op and op[0] == "method-of-type":
            # the parent method: record what pc-settings.gc would get
            obj = self.ev(args[0], env)
            self.calls.append(f"({op[2]} gfx-msaa {obj['gfx-msaa']})")
            return None
        if isinstance(op, list):
            raise NotImplementedError(op)
        if op == "quote":
            return Sym(args[0])
        if op == "none":
            return None
        if op == "->":
            return self.deref(self.ev(args[0], env), args[1:], env)
        if op == "&->":
            return Sym("<time-of-day>") if args[0] == "*time-of-day-context*" else Sym("<ptr>")
        if op == "set!":
            tgt, v = args[0], self.ev(args[1], env)
            if isinstance(tgt, Sym):
                (env if tgt in env else self.globals)[tgt] = v
            elif len(tgt) == 2:
                self.ev(tgt[1], env).set(v)  # (-> pointer)
            else:
                obj = self.ev(tgt[1], env)
                if isinstance(obj, Addr):
                    obj = obj.array()
                for p in [str(a) for a in tgt[2:-1]]:
                    obj = obj.setdefault(p, Struct())  # an inline array field: (-> obj field 0)
                obj[str(tgt[-1])] = v
            return v
        if op == "let":
            inner = dict(env)
            for name, val in args[0]:
                inner[name] = self.ev(val, env)
            return self.block(args[1:], inner)
        if op == "let*":
            inner = dict(env)
            for name, val in args[0]:
                inner[name] = self.ev(val, inner)
            return self.block(args[1:], inner)
        if op == "new":
            if len(args) > 1 and args[0] == ["quote", "static"] and args[1] == ["quote", "boxed-array"]:
                return [self.ev(a, env) for a in args[4:]]  # after :type <type>
            if len(args) > 1 and args[0] == ["quote", "static"] and str(args[1][1]) == "sandover-field":
                return Zero({str(k)[1:]: self.ev(v, env) for k, v in zip(args[2::2], args[3::2])})
            return Struct()
        if op == "dotimes":
            var = args[0][0]
            for i in range(self.ev(args[0][1], env)):
                env[var] = i  # the loop shares its scope: a set! of an outer local sticks
                self.block(args[1:], env)
            env.pop(var, None)
            return None
        if op == "when":
            return self.block(args[1:], env) if self.truthy(self.ev(args[0], env)) else None
        if op == "if":
            if self.truthy(self.ev(args[0], env)):
                return self.ev(args[1], env)
            return self.ev(args[2], env) if len(args) > 2 else None
        if op == "cond":
            for clause in args:
                if clause[0] == "else" or self.truthy(self.ev(clause[0], env)):
                    return self.block(clause[1:], env)
            return None
        if op == "case":
            key = self.ev(args[0], env)
            for clause in args[1:]:
                if clause[0] == "else" or key in [self.ev(k, env) for k in clause[0]]:
                    return self.block(clause[1:], env)
            return None
        if op == "and":
            v = True
            for a in args:
                v = self.ev(a, env)
                if not self.truthy(v):
                    return False
            return v
        if op == "or":
            for a in args:
                v = self.ev(a, env)
                if self.truthy(v):
                    return v
            return False
        if op == "not":
            return not self.truthy(self.ev(args[0], env))
        if op == "the":
            v = self.ev(args[1], env)
            if args[0] == "float" and isinstance(v, (int, float)):
                return float(v)
            return int(v) if args[0] == "int" and isinstance(v, (int, float)) else v
        if op == "mem-copy!":
            dst, src = self.ev(args[0], env), self.ev(args[1], env)
            dst.clear()
            dst.update(src)
            return dst
        if op in ("the-as", "size-of"):
            return self.ev(args[-1], env) if op == "the-as" else None
        if op == "sandover-changed?":
            f = str(args[0])
            return env["want"].get(f) != env["old"].get(f)
        if op in self.macros:
            return self.expand(op, args, env)
        if op in self.hooks:
            return self.hooks[op](*[self.ev(a, env) for a in args])
        if op == "format":
            dest = args[0]
            vals = [self.ev(a, env) for a in args[2:]]
            text = self._format(self.ev(args[1], env), vals)
            if isinstance(dest, list) and dest[0] == "clear":
                self.globals[str(dest[1])] = text
            elif isinstance(dest, Sym) and isinstance(self.ev(dest, env), Struct):
                file = self.ev(dest, env)  # a file stream: what the writer prints
                file["text"] = file.get("text", "") + text
            return text
        if op in ("string<-charp", "copy-string<-string"):
            # the device name: an inline byte array (a Struct, its "name" when set) into a string
            src = self.ev(args[1], env)
            src = src.array() if isinstance(src, Addr) else src
            text = src if isinstance(src, str) else (src.get("name", "") if src.get("0", 0) else "")
            self.globals[str(args[0])] = text
            return text
        if op == "pc-get-audio-state":
            return self.audio_cli if self.ev(args[0], env) == 4 else 0
        if op == "pc-get-xr-state":
            return self.xr_state[self.ev(args[0], env)]
        if op == "pc-filepath-exists?":
            return self.ev(args[0], env) in self.files
        if op == "pc-mkdir-file-path":
            return None
        if isinstance(op, Sym) and callable(self.globals.get(op)):
            return self.globals[op]()
        if op in self.defuns:
            return self.call(op, *[self.ev(a, env) for a in args])
        if op == "pc-temporal-aa-state":
            return self.temporal_state
        vals = [self.ev(a, env) for a in args]
        if op == "+" and isinstance(vals[0], Struct):
            return Addr(vals[0], LAYOUT[vals[1]])
        if str(op).startswith("pc-set-"):
            self.calls.append("(" + " ".join([op] + [_fmt(v) for v in vals]) + ")")
            return None
        return self._prim(op, vals)

    @staticmethod
    def _format(fmt, vals):
        """GOAL's directives as the settings use them: ~A prints a string in quotes, ~S bare, ~D an int,
        ~,,Nf a float with N decimals."""
        vals = list(vals)

        def one(m):
            if m.group(2) == "%":
                return "\n"
            if not vals:
                return ""
            v = vals.pop(0)
            if m.group(2) == "f":
                return f"{float(v):.{m.group(1)}f}"
            if m.group(2) == "A":
                return _fmt(v)
            return str(v)

        return re.sub(r"~(?:,,(\d+))?([ASDf%])", one, fmt)

    def expand(self, op, args, env):
        m = self.macros[op]
        params = m[2]
        body = m[-1]
        # the reader reads ` as quote; substitute the ,arguments and evaluate the result
        assert body[0] == "quote", op
        form = self._subst(body[1], dict(zip(params, args)))
        return self.ev(form, env)

    def _subst(self, x, bind):
        if isinstance(x, list):
            if x and x[0] == "unquote":
                return bind[x[1]]
            return [self._subst(y, bind) for y in x]
        return x

    @staticmethod
    def truthy(v):
        return v is not False and v is not None

    @staticmethod
    def _prim(op, v):
        ops = {
            "=": lambda: v[0] == v[1], "!=": lambda: v[0] != v[1], ">=": lambda: v[0] >= v[1],
            "<": lambda: v[0] < v[1], ">": lambda: v[0] > v[1], "<=": lambda: v[0] <= v[1],
            "zero?": lambda: v[0] == 0, "nonzero?": lambda: v[0] != 0, "string=": lambda: v[0] == v[1],
            "+": lambda: sum(v), "-": lambda: v[0] - sum(v[1:]) if len(v) > 1 else -v[0],
            "*": lambda: v[0] * v[1], "fmax": lambda: max(v), "fmin": lambda: min(v),
            "logtest?": lambda: bool(v[0] & v[1]), "logior": lambda: v[0] | v[1],
        }
        if op not in ops:
            raise NotImplementedError(op)
        return ops[op]()


def play_forms(pckernel: str, impl: str, **settings) -> tuple[list[str], list[str]]:
    """(reapply forms, first update forms) after reset! with ``settings`` overrides."""
    g = Goal([impl, pckernel])
    s = g.globals["*pc-settings*"]["sandover"]
    g.call("sandover-settings-reset!", s)
    s.update(settings)
    g.calls.clear()
    g.call("sandover-settings-reapply")
    reapply = list(g.calls)
    g.calls.clear()
    g.call("sandover-settings-update")
    update = list(g.calls)
    g.calls.clear()
    g.call("sandover-settings-update")
    assert g.calls == [], g.calls  # nothing left to send on the next frame
    return reapply, update


def _sources():
    return PCKERNEL.read_text(encoding="utf-8"), IMPL.read_text(encoding="utf-8")


def test_play_look_forms_match_master():
    reapply, update = play_forms(*_sources())
    assert update == MASTER_PLAY
    assert [f for f in reapply if f not in NEW_REAPPLY] == MASTER_REAPPLY
    assert all(f in reapply for f in NEW_REAPPLY)


def test_tiers_push_their_engine_forms():
    _, update = play_forms(*_sources(), **{"ssao-quality": 0, "shadow-quality": 1})
    assert "(pc-set-ssao-preset 0)" in update and "(pc-set-ssao-preset 1)" not in update
    assert "(pc-set-shadow-option 40 1000000)" in update
    # every tier runs as saved, with temporal AA and without it (stereo and XR, state 2)
    for tier in (1, 2, 3):
        _, tier_update = play_forms(*_sources(), **{"ssao-quality": tier})
        assert f"(pc-set-ssao-preset {tier})" in tier_update, tier
        assert f"(pc-set-ssao-preset {tier})" in _update_after_reset(2, **{"ssao-quality": tier}), tier
    _, update = play_forms(*_sources(), **{"ssao-quality": 1, "ssao?": False, "shadows?": False})
    assert not any("ssao-preset" in f or "shadow-option 40" in f for f in update)


#: the AA mode and render scale that the first update after a reapply pushes when temporal AA is off
AA_REPUSH_VANILLA = ["(pc-set-aa 0 0)", "(pc-set-aa 1 1000000)"]


def _update_after_reset(state: int = 1, **settings) -> list[str]:
    g = Goal(list(_sources())[::-1])
    g.temporal_state = state
    s = g.globals["*pc-settings*"]["sandover"]
    g.call("sandover-settings-reset!", s)
    s.update(settings)
    g.call("sandover-settings-reapply")
    g.calls.clear()
    g.call("sandover-settings-update")
    # the audio row has its own tests below: sound is not part of the render path
    return [c for c in g.calls if not c.startswith("(pc-set-audio-output")]


def _audio_forms(cli=-1, frames=2, between=None, **settings):
    g = Goal(list(reversed(_sources())))
    g.audio_cli = cli
    s = g.globals["*pc-settings*"]["sandover"]
    g.call("sandover-settings-reset!", s)
    s.update(settings)
    g.call("sandover-settings-reapply")
    out = []
    for i in range(frames):
        if between and i:
            between(g, s)
        g.calls.clear()
        g.call("sandover-settings-update")
        out.append([c for c in g.calls if c.startswith("(pc-set-audio-output")])
    return out


def test_audio_output_is_pushed_once_and_ignores_vanilla_rendering():
    assert _audio_forms() == [['(pc-set-audio-output 0 "")'], []]
    assert _audio_forms(**{"vanilla?": True}) == [['(pc-set-audio-output 0 "")'], []]
    assert _audio_forms(**{"audio-output": 2})[0] == ['(pc-set-audio-output 2 "")']
    dev = Struct({"0": 79, "name": "OpenAL Soft on Speakers (Steam Streaming Speakers)"})
    assert _audio_forms(**{"audio-output": 3, "audio-device": dev})[0] == [
        '(pc-set-audio-output 3 "OpenAL Soft on Speakers (Steam Streaming Speakers)")']


def test_command_line_output_is_a_runtime_override_never_saved():
    """gk --audio-spatial binaural (headphones, 4) runs instead of the saved 5.1 row; the saved
    field keeps 5.1. Confirming the Output row (its on-confirm) ends the override."""
    seen = {}

    def keep(g, s):
        seen["saved"] = s["audio-output"]

    assert _audio_forms(cli=4, between=keep, **{"audio-output": 2}) == [
        ['(pc-set-audio-output 4 "")'], []]
    assert seen["saved"] == 2

    def confirm(g, s):
        g.globals["*sandover-audio-cli*"] = -1  # the Output row's on-confirm

    assert _audio_forms(cli=4, between=confirm, **{"audio-output": 2}) == [
        ['(pc-set-audio-output 4 "")'], ['(pc-set-audio-output 2 "")']]


def test_initialize_does_not_seed_the_saved_row_from_the_command_line():
    text = PCKERNEL.read_text(encoding="utf-8")
    init = text[text.index("(defmethod initialize ((obj pc-settings-jak1))"):]
    init = init[:init.index("(defmethod", 10)]
    assert "pc-get-audio-state" not in init


def test_saved_spatial_is_pushed_like_any_output():
    """The saved Spatial row is pushed as output 5 on the next frame like the others; the engine
    decides whether this process can run it (set up at start) or keeps its output until a restart."""
    assert _audio_forms(**{"audio-output": 5}) == [['(pc-set-audio-output 5 "")'], []]

    def pick_spatial(g, s):
        s["audio-output"] = 5  # the Output row moved to Spatial mid-session

    assert _audio_forms(between=pick_spatial, **{"audio-output": 2}) == [
        ['(pc-set-audio-output 2 "")'], ['(pc-set-audio-output 5 "")']]


def test_start_reads_the_audio_row_of_the_profile_gk_runs():
    """gk sets Spatial up from the saved row before the sound system starts: the flat screen's
    file, or the VR profile's under --xr (the profile the game switches to for the XR session)."""
    from sandover import settings
    main = (REPO / "game" / "main.cpp").read_text(encoding="utf-8")
    spatial = (REPO / "game" / "sound" / "989snd" / "spatial.cpp").read_text(encoding="utf-8")
    assert "snd::SavedAudioFileName(Gfx::g_global_settings.xr_enable)" in main
    assert re.search(r'SavedAudioFileName\(bool xr\) \{\s*return xr \? "([^"]+)" : "([^"]+)";',
                     spatial).groups() == (settings.VR_FILE_NAME, settings.FILE_NAME)
    profile = PCKERNEL.read_text(encoding="utf-8")
    assert f'"~S/{settings.VR_FILE_NAME}" "~S/{settings.FILE_NAME}"' in profile


def test_command_line_objects_holds_the_audio_rows():
    """gk --audio-spatial objects: Windows Spatial Sound is configured once per process, so the
    menu sends nothing over it."""
    assert _audio_forms(cli=5) == [[], []]


def test_vanilla_rendering_pushes_nothing():
    """The switch puts the engine on upstream's path: no Sandover effect form, only the path itself
    and the AA mode and render scale re-push that a reapply leaves behind (mode 0, scale 1.0)."""
    assert _update_after_reset(**{"vanilla?": True}) == ["(pc-set-sandover-path 0)", *AA_REPUSH_VANILLA]


def test_no_temporal_aa_falls_back_to_vanilla():
    """Below GL 4.3 or with the FSR 3.1 programs unbuilt the engine reports 0: vanilla rendering."""
    assert _update_after_reset(state=0) == ["(pc-set-sandover-path 0)", *AA_REPUSH_VANILLA]


def test_stereo_and_xr_run_temporal_aa_per_eye():
    calls = _update_after_reset(state=2, **{"render-scale": 2, "contact-shadows?": True})
    assert calls[-2:] == ["(pc-set-aa 0 1000000)", "(pc-set-aa 1 666667)"]  # as on the flat screen
    assert "(pc-set-bloom 0 1000000)" in calls
    assert not any("pc-set-shadow-option 41" in c for c in calls)  # contact shadows: mono only


def test_render_scale_under_temporal_aa():
    calls = _update_after_reset(**{"render-scale": 2})
    assert calls[-2:] == ["(pc-set-aa 0 1000000)", "(pc-set-aa 1 666667)"]


def test_first_boot_takes_sandovers_msaa_over_a_stock_saved_one():
    """A stock OpenGOAL saves (msaa 1); with no sandover.gc yet, the first Sandover boot takes 2x (the
    vanilla path's MSAA) and runs temporal AA."""
    g = Goal(list(reversed(_sources())), gfx_msaa=1)
    s = g.globals["*pc-settings*"]
    g.call("sandover-first-boot!", s)
    assert s["gfx-msaa"] == PC_DEFAULT_MSAA == 2
    g.call("sandover-settings-reapply")
    g.calls.clear()
    g.call("sandover-settings-update")
    assert "(pc-set-aa 0 1000000)" in g.calls
    load = PCKERNEL.read_text(encoding="utf-8")
    load = load[load.index("(defun sandover-settings-load"):]
    assert load.index("(sandover-first-boot! obj)") < load.index("((method-of-type pc-settings commit-to-file) obj)")


def test_sun_row_is_its_own_switch():
    _, update = play_forms(*_sources(), **{"vista?": False})
    assert "(pc-set-sun-option 10 1000000)" in update and '(pc-set-vista "neighbours")' not in update
    assert "(pc-set-vista-option 5 <time-of-day>)" in update  # the disc's clock
    _, update = play_forms(*_sources(), **{"sun?": False})
    assert not any("pc-set-sun-option 10" in f for f in update)


def test_glint_row_is_its_own_switch():
    _, update = play_forms(*_sources(), **{"glint?": False})
    assert not any("pc-set-water 94" in f for f in update)
    _, update = play_forms(*_sources(), **{"water?": False})
    assert "(pc-set-water 94 1000000)" in update  # inland water keeps it


def test_contact_shadows_row_is_its_own_switch():
    _, update = play_forms(*_sources())
    assert not any("pc-set-shadow-option 41" in f for f in update)  # off by default
    _, update = play_forms(*_sources(), **{"contact-shadows?": True})
    assert "(pc-set-shadow-option 41 2000000)" in update


def test_light_shafts_follow_the_materials_switch():
    _, update = play_forms(*_sources(), **{"pbr?": False})
    assert not any("pc-set-hide-shafts" in f for f in update)


def test_mounted_flag_is_sent_on_change():
    """Camera motion blur, Vehicles: the mounted flag goes out once when it changes, not every frame."""
    pckernel, impl = _sources()
    g = Goal([impl, pckernel])
    s = g.globals["*pc-settings*"]["sandover"]
    g.call("sandover-settings-reset!", s)
    g.call("sandover-settings-reapply")
    g.call("sandover-settings-update")
    g.calls.clear()
    g.defuns["sandover-riding-vehicle?"] = ["defun", "sandover-riding-vehicle?", [], True]
    g.call("sandover-settings-update")
    assert g.calls == ["(pc-set-motion-blur 11 1)"]
    g.calls.clear()
    g.call("sandover-settings-update")
    assert g.calls == []
    g.defuns["sandover-riding-vehicle?"] = ["defun", "sandover-riding-vehicle?", [], False]
    g.call("sandover-settings-update")
    assert g.calls == ["(pc-set-motion-blur 11 0)"]


# ---- flat screen and VR profiles ----------------------------------------------------------------

FLAT_FILE, VR_FILE = "cfg/sandover.gc", "cfg/sandover-vr.gc"


def _profile_goal(gfx_msaa: int | None = None) -> Goal:
    """The evaluator with sandover-settings-read and -write over an in-memory folder: a file is the
    dict of the fields it holds (msaa only when the writer was given one, as the game writes it;
    None is a file that does not parse)."""
    g = Goal(list(_sources())[::-1], gfx_msaa=gfx_msaa)

    def write(obj, filename, msaa):
        g.files[filename] = {k: v for k, v in obj.items() if k != "msaa"} | ({"msaa": msaa} if msaa >= 0 else {})
        return True

    def read(obj, filename):
        if g.files.get(filename) is None:
            return False
        obj.update(g.files[filename])
        return True

    g.hooks = {"sandover-settings-write": write, "sandover-settings-read": read}
    return g


def _settings(g: Goal):
    return g.globals["*pc-settings*"]


def _defaults(g: Goal, vr: bool = False) -> dict:
    s = Struct()
    g.call("sandover-settings-reset-vr!" if vr else "sandover-settings-reset!", s)
    return {k: v for k, v in s.items() if k != "msaa"}


def _frame(g: Goal) -> list[str]:
    g.calls.clear()
    g.call("sandover-settings-update")
    return list(g.calls)


def _xr(g: Goal, running: bool = True):
    g.xr_state = [1 if running else 0, 4491, 3468]
    g.temporal_state = 2 if running else 1  # pc-temporal-aa-state: 2 with stereo or XR (per eye)


def _booted(gfx_msaa: int = 2) -> Goal:
    g = _profile_goal(gfx_msaa=gfx_msaa)
    g.call("sandover-settings-load", _settings(g))
    g.call("sandover-settings-reapply")
    _frame(g)
    return g


def test_first_boot_writes_both_profiles():
    g = _profile_goal()
    g.call("sandover-settings-load", _settings(g))
    assert g.globals["*sandover-profile*"] == "flat"
    assert g.files[FLAT_FILE] == _defaults(g)
    vr = g.files[VR_FILE]
    assert {k: v for k, v in vr.items() if k != "msaa"} == _defaults(g, vr=True)
    assert _defaults(g, vr=True) == _defaults(g) | {"render-scale": 2}  # 67%, the rest as flat
    assert vr["msaa"] == _settings(g)["gfx-msaa"] == PC_DEFAULT_MSAA


def test_older_install_gets_vr_defaults_not_its_flat_values():
    """A sandover.gc from before the split keeps its values; the VR file starts from the VR
    defaults, and from the flat screen's MSAA (the stock row in pc-settings.gc)."""
    g = _profile_goal(gfx_msaa=4)
    flat = _defaults(g) | {"ssao?": False, "render-scale": 4}
    g.files[FLAT_FILE] = dict(flat)
    g.call("sandover-settings-load", _settings(g))
    assert _settings(g)["sandover"]["ssao?"] is False and _settings(g)["sandover"]["render-scale"] == 4
    assert g.files[FLAT_FILE] == flat  # not rewritten
    assert g.files[VR_FILE]["ssao?"] is True and g.files[VR_FILE]["render-scale"] == 2
    assert g.files[VR_FILE]["msaa"] == 4


def test_unreadable_vr_file_falls_back_to_vr_defaults():
    g = _profile_goal(gfx_msaa=8)
    g.files[FLAT_FILE] = _defaults(g)
    g.files[VR_FILE] = None
    g.call("sandover-settings-load", _settings(g))
    other = _settings(g)["sandover-other"]
    assert other["render-scale"] == 2 and other["msaa"] == 8


def test_xr_session_switches_to_the_vr_profile_at_once():
    g = _booted()
    assert _frame(g) == []
    _xr(g)
    calls = _frame(g)
    assert g.globals["*sandover-profile*"] == "vr"
    # the VR render scale sizes each eye's frame; the upscaler stays (it runs per eye)
    assert "(pc-set-aa 1 666667)" in calls and not any(c.startswith("(pc-set-aa 0 ") for c in calls)
    assert _frame(g) == []  # nothing left on the next frame
    _xr(g, running=False)
    calls = _frame(g)
    assert g.globals["*sandover-profile*"] == "flat"
    assert "(pc-set-aa 1 1000000)" in calls and not any(c.startswith("(pc-set-aa 0 ") for c in calls)


def test_a_vr_edit_never_reaches_the_flat_file():
    g = _booted(gfx_msaa=2)
    flat_before = dict(g.files[FLAT_FILE])
    _xr(g)
    _frame(g)
    s = _settings(g)
    # edits in VR, through the rows' fields: render scale, a switch, and the stock MSAA row
    s["sandover"]["render-scale"] = 4
    s["sandover"]["bloom?"] = False
    s["gfx-msaa"] = 8
    g.calls.clear()
    g.call("method:commit-to-file", s)
    assert g.calls == ["(commit-to-file gfx-msaa 2)"]  # pc-settings.gc keeps the flat MSAA
    assert s["gfx-msaa"] == 8  # and the VR one stays in effect
    assert g.files[FLAT_FILE] == flat_before
    vr = g.files[VR_FILE]
    assert vr["render-scale"] == 4 and vr["bloom?"] is False and vr["msaa"] == 8
    # back on the flat screen: the flat values and MSAA return, the VR ones are kept for next time
    _xr(g, running=False)
    _frame(g)
    assert s["sandover"]["render-scale"] == 0 and s["sandover"]["bloom?"] is True and s["gfx-msaa"] == 2
    assert s["sandover-other"]["render-scale"] == 4 and s["sandover-other"]["msaa"] == 8
    g.calls.clear()
    g.call("method:commit-to-file", s)
    assert g.calls == ["(commit-to-file gfx-msaa 2)"]
    assert g.files[FLAT_FILE] == flat_before and g.files[VR_FILE]["render-scale"] == 4


def test_a_flat_edit_never_reaches_the_vr_file():
    g = _booted()
    vr_before = dict(g.files[VR_FILE])
    s = _settings(g)
    s["sandover"]["ssao-quality"] = 3
    s["gfx-msaa"] = 4
    g.calls.clear()
    g.call("method:commit-to-file", s)
    assert g.calls == ["(commit-to-file gfx-msaa 4)"]
    assert g.files[VR_FILE] == vr_before and g.files[FLAT_FILE]["ssao-quality"] == 3


def test_profile_holds_while_a_row_is_edited():
    """A row's backup (cancel) belongs to the profile it was opened in: no switch until it closes."""
    g = _booted()
    editing = [True]
    g.globals["*sandover-menu-editing-func*"] = lambda: editing[0]
    _xr(g)
    _frame(g)
    assert g.globals["*sandover-profile*"] == "flat"
    editing[0] = False
    _frame(g)
    assert g.globals["*sandover-profile*"] == "vr"


def test_repl_force_holds_a_profile():
    g = _booted()
    g.globals["*sandover-profile-force*"] = Sym("vr")
    calls = _frame(g)
    assert g.globals["*sandover-profile*"] == "vr"
    # without an XR session the render scale runs through temporal AA, as on the flat screen
    assert "(pc-set-aa 1 666667)" in calls
    _xr(g)
    g.globals["*sandover-profile-force*"] = Sym("flat")
    _frame(g)
    assert g.globals["*sandover-profile*"] == "flat"


def test_rows_do_the_same_in_both_profiles():
    """No hidden coupling: equal values give equal effective settings in either profile, and in
    debug stereo (the same engine state without a session)."""
    g = _booted()
    _xr(g)
    _frame(g)
    s = _settings(g)
    s["sandover"].update({k: v for k, v in s["sandover-other"].items() if k != "msaa"})
    vr = Struct()
    g.call("sandover-effective!", vr, s["sandover"])
    _xr(g, running=False)
    g.temporal_state = 2  # the same engine state without a session: debug stereo
    flat = Struct()
    g.call("sandover-effective!", flat, s["sandover-other"])
    assert vr == flat
    # temporal AA runs per eye: the upscaler and render scale apply; contact shadows do not run
    assert vr["upscaler"] == s["sandover"]["upscaler"] >= 0
    assert vr["render-scale"] == s["sandover"]["render-scale"]
    assert vr["contact-shadows?"] is False


def test_vanilla_rendering_in_vr_runs_the_eyes_at_their_size():
    g = _booted()
    _settings(g)["sandover-other"]["vanilla?"] = True
    _xr(g)
    calls = _frame(g)
    # upstream's path: the scale stays at 100% (what the flat profile pushed), no VR scale
    assert "(pc-set-sandover-path 0)" in calls and not any(c.startswith("(pc-set-aa 1") for c in calls)


def test_plain_stereo_runs_the_flat_profile_per_eye():
    """The debug side-by-side stereo is not an XR session: the flat profile, whose upscaler and
    render scale then run per eye."""
    g = _booted()
    g.temporal_state = 2
    _settings(g)["sandover"]["render-scale"] = 2
    calls = _frame(g)
    assert g.globals["*sandover-profile*"] == "flat" and "(pc-set-aa 1 666667)" in calls


def test_engine_applies_the_render_scale_to_the_xr_eye_frame():
    src = (REPO / "game" / "graphics" / "pipelines" / "opengl.cpp").read_text(encoding="utf-8")
    block = src[src.index("auto set_xr_eye_size = [&]() {"):]
    block = block[:block.index("};")]
    assert "Gfx::g_global_settings.render_scale" in block
    assert "xr_targets->render_w * rs" in block and "options.display_w = xr_targets->render_w" in block
    # both XR places (the frame and a screenshot's restore) size the eyes through it, and the
    # screenshot flag no longer bypasses the scale
    assert src.count("set_xr_eye_size();") == 2
    assert "options.xr && !options.internal_res_screenshot" not in src
    # temporal AA does not scale an XR eye frame a second time
    assert "if (rs < 0.999f && !options.xr) {" in src
    assert "Gfx::g_xr_session_running = xr::state() == xr::State::RUNNING;" in src
    km = bridge_text(REPO)
    assert '{"pc-get-xr-state", (void*)pc_get_xr_state, BridgeScope::Jak1}' in km
    defs = (REPO / "goal_src" / "jak1" / "kernel-defs.gc").read_text(encoding="utf-8")
    assert "(define-extern pc-get-xr-state (function int int))" in defs


# ---- the settings table: reset, boot state, reader and writer ------------------------------------

#: what master's sandover-boot-state! set (not audio-output, audio-device or msaa: the boot state leaves them)
MASTER_BOOT = {
    "vanilla?": True, "ssao?": False, "tile-lights?": False, "bloom?": False, "shadows?": False, "pbr?": False,
    "pbr-look": -1, "fur?": False, "water?": False, "vista?": False, "hdr-target?": False, "tonemap": 0,
    "hdr-output": 2, "paper-white": 200.0, "hdr-peak": -1.0, "hdr-black": -1.0, "render-scale": 0,
    "upscaler": -1, "ssao-quality": 1, "shadow-quality": 2, "sun?": False, "glint?": False,
    "contact-shadows?": False, "motion-blur": 0, "camera-blur": 0,
}

#: what master's sandover-settings-reset! set
MASTER_RESET = {
    "vanilla?": False, "ssao?": True, "tile-lights?": True, "bloom?": True, "shadows?": True, "pbr?": True,
    "pbr-look": 1, "fur?": True, "water?": True, "vista?": True, "hdr-target?": True, "tonemap": 2,
    "hdr-output": 1, "paper-white": 200.0, "hdr-peak": -1.0, "hdr-black": -1.0, "render-scale": 0, "upscaler": 0,
    "ssao-quality": 1, "shadow-quality": 2, "sun?": True, "glint?": True, "contact-shadows?": False,
    "motion-blur": 2, "camera-blur": 1, "audio-output": 0, "msaa": PC_DEFAULT_MSAA,
}

#: the file key of each field whose name differs
KEY_OF = {"vanilla?": "vanilla-rendering?", "pbr?": "materials?", "pbr-look": "material-look", "vista?": "neighbours?",
          "hdr-target?": "fp16-target?", "paper-white": "paper-white-nits", "hdr-peak": "hdr-peak-nits",
          "hdr-black": "hdr-black-nits"}


def test_reset_and_boot_state_are_masters_over_the_table():
    g = Goal(list(_sources())[::-1])
    s = Struct()
    g.call("sandover-settings-reset!", s)
    assert {k: v for k, v in s.items() if k != "audio-device"} == MASTER_RESET
    assert s["audio-device"] == {"0": 0}
    for k, v in MASTER_RESET.items():
        assert type(s[k]) is type(v), k  # symbols stay booleans, floats stay floats
    boot = Struct()
    g.call("sandover-boot-state!", boot)
    assert dict(boot) == MASTER_BOOT
    assert all(type(boot[k]) is type(v) for k, v in MASTER_BOOT.items())
    # the boot state leaves what it does not name: audio, msaa
    taken = Struct(**{"audio-output": 3, "msaa": 8, "audio-device": Struct({"0": 65})})
    g.call("sandover-boot-state!", taken)
    assert taken["audio-output"] == 3 and taken["msaa"] == 8 and taken["audio-device"] == {"0": 65}
    vr = Struct()
    g.call("sandover-settings-reset-vr!", vr)
    assert {k: v for k, v in vr.items() if k != "audio-device"} == MASTER_RESET | {"render-scale": 2}


KINDS = {0: "bool", 1: "enum", 2: "intmap", 3: "float", 4: "word", 5: "bytes", 6: "int"}


def _to_fields(values: dict) -> Struct:
    """A sandover-settings Struct holding the mirror's file values."""
    s = Struct(msaa=0, **{"audio-device": Struct({"0": 0})})
    for r in settings_table.fields():
        if r["key"] not in values:
            continue
        v = values[r["key"]]
        field = next((f for f, k in KEY_OF.items() if k == r["key"]), r["key"])
        kind = KINDS[r["kind"]]
        s[field] = {"bool": lambda: v, "enum": lambda: r["names"].index(v), "intmap": lambda: r["ints"].index(v),
                    "float": lambda: float(v), "word": lambda: r["words"].index(v),
                    "bytes": lambda: Struct({"0": 1 if v else 0, "name": v}), "int": lambda: v}[kind]()
    return s


def _write(s: Struct, msaa: int = -1) -> str:
    """The text sandover-settings-write prints for a settings struct, run from the GOAL source."""
    g = Goal(list(_sources())[::-1])
    out = []
    g.hooks = {"file-stream-valid?": lambda f: True, "file-stream-close": lambda f: out.append(f.get("text", ""))}
    assert g.call("sandover-settings-write", s, "x.gc", msaa) is True
    return out[0]


def test_writer_prints_the_files_the_tools_expect():
    from sandover import settings

    dev = "OpenAL Soft on Speakers (Steam Streaming Speakers)"
    cases = [
        {},
        settings.ALL_OFF,
        {"material-look": "cinematic", "tonemap": "filmic", "hdr-output": "scrgb", "paper-white-nits": 150,
         "hdr-peak-nits": 1000, "hdr-black-nits": 0.05, "render-scale": 50, "upscaler": "dlss",
         "ssao-quality": "ultra", "shadow-quality": "low", "motion-blur": "strong", "camera-blur": "always",
         "audio-output": "5.1", "audio-device": dev, "contact-shadows?": True},
        {"upscaler": "fsr4", "audio-output": "spatial", "ssao-quality": "high", "sun?": False},
        {"hdr-peak-nits": 400, "hdr-black-nits": 1.0, "paper-white-nits": 80, "audio-output": "headphones"},
    ]
    for values in cases:
        assert _write(_to_fields(settings.DEFAULTS | values)) == settings.dumps(values), values
    # the VR file adds the MSAA key at the end, and only for a non-negative value
    vr = settings.VR_DEFAULTS | {"render-scale": 50}
    assert _write(_to_fields(vr), msaa=4) == settings.dumps(vr | {"msaa": 4}, vr=True)
    assert "msaa" not in _write(_to_fields(vr), msaa=-1)


def test_writer_prints_a_value_outside_a_list_as_its_fallback():
    """Master's writer had an else branch per list: an index outside the names writes the same name."""
    from sandover import settings

    s = _to_fields(settings.DEFAULTS)
    s.update({"pbr-look": -1, "tonemap": 7, "hdr-output": 9, "upscaler": -2, "ssao-quality": 8, "shadow-quality": 5,
              "motion-blur": 4, "camera-blur": 3, "render-scale": 6, "audio-output": 9})
    text = _write(s)
    for line in ("(material-look bold)", "(tonemap toon)", "(hdr-output scrgb)", "(upscaler fsr3)", "(ssao-quality medium)",
                 "(shadow-quality high)", "(motion-blur medium)", "(camera-blur vehicles)", "(render-scale 100)",
                 "(audio-output auto)"):
        assert line in text, line


def _read_key(key: str, raw: str, version: int = 4, **start):
    """Run sandover-settings-read-key for ``key`` with the value token ``raw``; (fields, skipped?)."""
    g = Goal(list(_sources())[::-1])
    s = Struct()
    g.call("sandover-settings-reset!", s)
    s.update(start)
    g.globals["*pc-temp-string*"] = key
    g.globals["*sandover-settings-file-version*"] = version
    skipped = []

    def read_word(f):
        g.globals["*pc-temp-string*"] = raw

    def read_quoted(f, dst, size):
        text = raw[1:-1] if raw.startswith('"') else ""
        arr = dst.array()
        arr["name"], arr["0"] = text[:size - 1], 1 if text else 0

    g.hooks = {
        "sandover-skip-value": lambda f: skipped.append(key),
        "sandover-read-name": lambda f: Sym(raw.strip('"')),  # quotes are dropped like the game
        "file-stream-read-int": lambda f: int(raw),
        "file-stream-read-float": lambda f: float(raw),
        "file-stream-read-symbol": lambda f: raw != "#f",
        "file-stream-seek-past-whitespace": lambda f: None,
        "file-stream-read-word": read_word,
        "sandover-read-quoted": read_quoted,
    }
    g.call("sandover-settings-read-key", s, Struct())
    return s, bool(skipped)


@pytest.mark.parametrize("key, raw, version, field, want", [
    ("ssao?", "#f", 4, "ssao?", False),
    ("neighbours?", "#t", 4, "vista?", True),
    ("materials?", "#f", 4, "pbr?", False),
    ("material-look", "faithful", 4, "pbr-look", 0),
    ("material-look", '"cinematic"', 4, "pbr-look", 2),
    ("material-look", "aces", 4, "pbr-look", 1),  # no match: the field keeps its value
    ("tonemap", '"filmic"', 4, "tonemap", 1),
    ("hdr-output", "off", 4, "hdr-output", 2),
    ("hdr-output", "scrgb", 4, "hdr-output", 0),
    ("upscaler", "fsr4", 4, "upscaler", 2),
    ("upscaler", "bilinear", 4, "upscaler", 0),
    ("ssao-quality", "low", 3, "ssao-quality", 0),
    ("ssao-quality", "medium", 3, "ssao-quality", 0),
    ("ssao-quality", "high", 3, "ssao-quality", 1),
    ("ssao-quality", "ultra", 3, "ssao-quality", 3),
    ("ssao-quality", "medium", 4, "ssao-quality", 1),
    ("ssao-quality", "high", 4, "ssao-quality", 2),
    ("ssao-quality", "ultra", 4, "ssao-quality", 3),
    ("shadow-quality", "ultra", 4, "shadow-quality", 2),
    ("motion-blur", "strong", 4, "motion-blur", 3),
    ("camera-blur", "always", 4, "camera-blur", 2),
    ("render-scale", "100", 4, "render-scale", 0),
    ("render-scale", "77", 4, "render-scale", 1),
    ("render-scale", "33", 4, "render-scale", 5),
    ("render-scale", "80", 4, "render-scale", 0),  # no match: keeps its value
    ("paper-white-nits", "150", 4, "paper-white", 150.0),
    ("paper-white-nits", "1000", 4, "paper-white", 400.0),
    ("paper-white-nits", "40", 4, "paper-white", 80.0),
    ("paper-white-nits", "-5", 4, "paper-white", 80.0),
    ("hdr-peak-nits", "20000", 4, "hdr-peak", 10000.0),
    ("hdr-peak-nits", "100", 4, "hdr-peak", 400.0),
    ("hdr-peak-nits", "1000", 4, "hdr-peak", 1000.0),
    ("hdr-peak-nits", "-5", 4, "hdr-peak", -1.0),
    ("hdr-black-nits", "5.0", 4, "hdr-black", 1.0),
    ("hdr-black-nits", "0.05", 4, "hdr-black", 0.05),
    ("hdr-black-nits", "-1.000", 4, "hdr-black", -1.0),
    ("audio-output", "5.1", 4, "audio-output", 2),
    ("audio-output", "spatial", 4, "audio-output", 5),
    ("audio-output", "bogus", 4, "audio-output", 0),
    ("msaa", "8", 4, "msaa", 8),
])
def test_reader_applies_each_rule(key, raw, version, field, want):
    s, skipped = _read_key(key, raw, version)
    assert not skipped and s[field] == want and type(s[field]) is type(want)


def test_reader_keeps_a_value_that_matches_nothing():
    s, _ = _read_key("render-scale", "80", **{"render-scale": 3})
    assert s["render-scale"] == 3
    s, _ = _read_key("upscaler", "fsr", **{"upscaler": 2})
    assert s["upscaler"] == 2


def test_reader_reads_the_device_and_skips_unknown_keys():
    s, skipped = _read_key("audio-device", '"OpenAL Soft on X"')
    assert not skipped and s["audio-device"]["name"] == "OpenAL Soft on X" and s["audio-device"]["0"] == 1
    s, skipped = _read_key("audio-device", '"' + "x" * 200 + '"')
    assert len(s["audio-device"]["name"]) == 127  # 128 bytes with the terminator
    s, skipped = _read_key("audio-device", "default")
    assert s["audio-device"]["0"] == 0
    for key in ("future-thing", "antialias", "smaa?", "Ssao?"):
        _, skipped = _read_key(key, "1")
        assert skipped, key


def test_field_offsets_match_the_type():
    """The table's byte offsets are literals (a static table cannot call offset-of): they must be the
    layout of the sandover-settings type, in the order of the type, one row per field."""
    layout = settings_table.layout()
    rows = settings_table.fields()
    names = [next((f for f, k in KEY_OF.items() if k == r["key"]), r["key"]) for r in rows]
    assert names == [n for n in layout if n != "<size>"]
    assert [r["offset"] for r in rows] == [layout[n] for n in names]
    audio = next(r for r in rows if r["key"] == "audio-device")
    assert layout["msaa"] - layout["audio-device"] == audio["size"]


def test_layout_sizes_and_aligns_each_type():
    src = """(deftype sandover-settings (structure)
  ((a int16)
   (b int64)
   (c int32 4)
   (d uint8 3)
   (e float))
  :pack-me)"""
    assert settings_table.layout(src) == {"a": 0, "b": 8, "c": 16, "d": 32, "e": 36, "<size>": 40}


def test_layout_rejects_an_unknown_type():
    src = "(deftype sandover-settings (structure)\n  ((a foo)\n   (b int32))\n  :pack-me)"
    with pytest.raises(ValueError):
        settings_table.layout(src)
