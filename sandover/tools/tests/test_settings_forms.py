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

REPO = Path(__file__).resolve().parents[3]
PCKERNEL = REPO / "goal_src" / "jak1" / "pc" / "pckernel.gc"
IMPL = REPO / "goal_src" / "jak1" / "pc" / "pckernel-impl.gc"
PC_DEFAULT_MSAA = int(re.search(r"^\(defconstant PC_DEFAULT_MSAA (\d+)\)",
                                (REPO / "goal_src" / "jak1" / "pc" / "pckernel-h.gc").read_text(encoding="utf-8"),
                                flags=re.M).group(1))

#: what master's reapply sent (its non-field resets), in order
MASTER_REAPPLY = [
    "(pc-set-lava 15 1)",
    "(pc-set-emissive-lights 37 1)",
    "(pc-set-smaa 4 1)",
    "(pc-set-render-scale 3 1)",
    "(pc-set-shadow-option 20 1000000)",
    "(pc-set-sun-option 19 1)",
]

#: what master's first settings update sent after reset! and reapply (the play look), in order
MASTER_PLAY = [
    "(pc-set-ssao-preset 4)",
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
    "(pc-set-smaa 0 2000000)",  # SMAA row on over 2x MSAA, new since master
]

#: reapply resets of the sub-options added with the tier rows, of the world sun map (32-38) and of
#: the water glint's parameters (95-97) and of the sky bodies (sun options 20-28)
NEW_REAPPLY = ["(pc-set-shadow-option 40 2000000)", "(pc-set-shadow-option 39 1)",
               "(pc-set-sun-option 29 1)",
               "(pc-set-water 98 1)"]


# ---- reader -----------------------------------------------------------------------------------

_TOKEN = re.compile(r'"(?:\\.|[^"\\])*"|[()\'`]|,@|,|[^\s()\'`,]+')


def _strip_comments(text: str) -> str:
    text = re.sub(r"#\|.*?\|#", " ", text, flags=re.S)
    out = []
    for line in text.split("\n"):
        cut, in_str = len(line), False
        for i, c in enumerate(line):
            if c == '"' and (i == 0 or line[i - 1] != "\\"):
                in_str = not in_str
            elif c == ";" and not in_str:
                cut = i
                break
        out.append(line[:cut])
    return "\n".join(out)


class Sym(str):
    pass


def _atom(tok: str):
    if tok.startswith('"'):
        return tok[1:-1]
    if tok == "#t":
        return True
    if tok == "#f":
        return False
    if re.fullmatch(r"-?\d+", tok):
        return int(tok)
    if re.fullmatch(r"#x[0-9a-fA-F]+", tok):
        return int(tok[2:], 16)
    if re.fullmatch(r"-?\d*\.\d+(e-?\d+)?", tok):
        return float(tok)
    return Sym(tok)


def read_forms(text: str) -> list:
    toks = _TOKEN.findall(_strip_comments(text))
    pos = 0

    def one():
        nonlocal pos
        t = toks[pos]
        pos += 1
        if t == "(":
            lst = []
            while toks[pos] != ")":
                lst.append(one())
            pos += 1
            return lst
        if t in ("'", "`"):
            return [Sym("quote"), one()]
        if t in (",", ",@"):
            return [Sym("unquote"), one()]
        return _atom(t)

    forms = []
    while pos < len(toks):
        forms.append(one())
    return forms


# ---- evaluator --------------------------------------------------------------------------------


class Struct(dict):
    pass


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
        for text in sources:
            for f in read_forms(text):
                if isinstance(f, list) and f and f[0] == "defun":
                    self.defuns[f[1]] = f
                elif isinstance(f, list) and f and f[0] == "defmacro":
                    self.macros[f[1]] = f
        self.calls: list[str] = []
        self.globals = {
            "*pc-settings*": Struct(sandover=Struct(),
                                    **{"gfx-msaa": PC_DEFAULT_MSAA if gfx_msaa is None else gfx_msaa}),
            "PC_DEFAULT_MSAA": PC_DEFAULT_MSAA,
            "*sandover-applied*": Struct(),
            "*sandover-calibration-pattern*": 0,
            "*sandover-calibration-pattern-func*": False,
        }

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

    def deref(self, obj, path):
        for p in path:
            obj = obj[p]
        return obj

    def ev(self, x, env):
        if isinstance(x, Sym):
            return None if x == "none" else self.lookup(x, env)
        if not isinstance(x, list):
            return x
        op, args = x[0], x[1:]
        if isinstance(op, list):
            raise NotImplementedError(op)
        if op == "quote":
            return Sym(args[0])
        if op == "none":
            return None
        if op == "->":
            return self.deref(self.ev(args[0], env), [str(a) for a in args[1:]])
        if op == "&->":
            return Sym("<time-of-day>") if args[0] == "*time-of-day-context*" else Sym("<ptr>")
        if op == "set!":
            tgt, v = args[0], self.ev(args[1], env)
            if isinstance(tgt, Sym):
                (env if tgt in env else self.globals)[tgt] = v
            else:
                self.deref(self.ev(tgt[1], env), [str(a) for a in tgt[2:-1]])[str(tgt[-1])] = v
            return v
        if op == "let":
            inner = dict(env)
            for name, val in args[0]:
                inner[name] = self.ev(val, env)
            return self.block(args[1:], inner)
        if op == "new":
            return Struct()
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
        if op in self.defuns:
            return self.call(op, *[self.ev(a, env) for a in args])
        vals = [self.ev(a, env) for a in args]
        if str(op).startswith("pc-set-"):
            self.calls.append("(" + " ".join([op] + [_fmt(v) for v in vals]) + ")")
            return None
        return self._prim(op, vals)

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
            "zero?": lambda: v[0] == 0, "nonzero?": lambda: v[0] != 0,
            "+": lambda: sum(v), "-": lambda: v[0] - sum(v[1:]) if len(v) > 1 else -v[0],
            "*": lambda: v[0] * v[1], "fmax": lambda: max(v), "fmin": lambda: min(v),
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
    assert "(pc-set-ssao-preset 6)" in update and "(pc-set-ssao-preset 4)" not in update
    assert "(pc-set-shadow-option 40 1000000)" in update
    _, update = play_forms(*_sources(), **{"ssao-quality": 1, "ssao?": False, "shadows?": False})
    assert not any("ssao-preset" in f or "shadow-option 40" in f for f in update)


@pytest.mark.parametrize("msaa,mode", [(1, 1), (2, 2), (4, 4), (8, 4)])
def test_smaa_row_and_msaa_setting_pick_the_antialias_mode(msaa, mode):
    pck, impl = _sources()
    g = Goal([impl, pck], gfx_msaa=msaa)
    s = g.globals["*pc-settings*"]["sandover"]
    g.call("sandover-settings-reset!", s)
    s["smaa?"] = True
    g.call("sandover-settings-reapply")
    g.calls.clear()
    g.call("sandover-settings-update")
    assert f"(pc-set-smaa 0 {mode * 1000000})" in g.calls
    # an explicit mode (file, REPL, rig) wins over the row
    s["antialias"] = 3
    g.calls.clear()
    g.call("sandover-settings-update")
    assert g.calls == ["(pc-set-smaa 0 3000000)"]


def test_first_boot_takes_sandovers_msaa_over_a_stock_saved_one():
    """A stock OpenGOAL saves (msaa 1); with no sandover.gc yet, the first Sandover boot runs 2x + SMAA."""
    g = Goal(list(reversed(_sources())), gfx_msaa=1)
    s = g.globals["*pc-settings*"]
    g.call("sandover-first-boot!", s)
    assert s["gfx-msaa"] == PC_DEFAULT_MSAA == 2
    g.call("sandover-settings-reapply")
    g.calls.clear()
    g.call("sandover-settings-update")
    assert "(pc-set-smaa 0 2000000)" in g.calls
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


def test_light_shafts_follow_the_materials_switch():
    _, update = play_forms(*_sources(), **{"pbr?": False})
    assert not any("pc-set-hide-shafts" in f for f in update)
