"""The settings tables of the GOAL source, for the tests: ``*sandover-fields*`` (pckernel-impl.gc, one
row per ``sandover-settings`` field) and ``*sandover-menu*`` (progress-pc.gc, the page's rows), and the
byte layout of the ``sandover-settings`` type the rows' offsets must match.

A small reader of the GOAL subset those forms use; the evaluator in ``tests/test_settings_forms.py``
runs the table loops over the same rows.
"""

from __future__ import annotations

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
IMPL = REPO / "goal_src" / "jak1" / "pc" / "pckernel-impl.gc"
MENU = REPO / "goal_src" / "jak1" / "pc" / "progress-pc.gc"
PCKERNEL = REPO / "goal_src" / "jak1" / "pc" / "pckernel.gc"

_TOKEN = re.compile(r'"(?:\\.|[^"\\])*"|[()\'`]|,@|,|[^\s()\'`,]+')


class Sym(str):
    """A GOAL symbol (as opposed to a string)."""


def strip_comments(text: str) -> str:
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


def atom(tok: str):
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
    toks = _TOKEN.findall(strip_comments(text))
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
        return atom(t)

    forms = []
    while pos < len(toks):
        forms.append(one())
    return forms


def constants(text: str) -> dict:
    out: dict = {}
    for f in read_forms(text):
        if isinstance(f, list) and f and f[0] == "defconstant":
            out[f[1]] = f[2]
    return out


def _value(x, consts: dict):
    """A table cell: constants and ``(logior ...)`` resolved, a static boxed array as a list."""
    if isinstance(x, Sym):
        if x in consts:
            return consts[x]
        return str(x)
    if isinstance(x, list) and x and x[0] == "quote":
        return str(x[1])
    if isinstance(x, list) and x and x[0] == "logior":
        v = 0
        for a in x[1:]:
            v |= _value(a, consts)
        return v
    if isinstance(x, list) and x[:3] == [Sym("new"), [Sym("quote"), Sym("static")], [Sym("quote"), Sym("boxed-array")]]:
        rest = x[3:]
        assert rest[0] == ":type"
        return [_value(a, consts) for a in rest[2:]]
    if isinstance(x, list) and x and x[0] == "text-id":
        return str(x[1])
    return x


def _rows(text: str, table: str, row_type: str, consts: dict) -> list[dict]:
    for f in read_forms(text):
        if isinstance(f, list) and f[:2] == ["define", Sym(table)]:
            arr = f[2]
            assert arr[:3] == [Sym("new"), [Sym("quote"), Sym("static")], [Sym("quote"), Sym("boxed-array")]], table
            rows = []
            for row in arr[5:]:
                assert row[:3] == [Sym("new"), [Sym("quote"), Sym("static")], [Sym("quote"), Sym(row_type)]], row[:3]
                kv = row[3:]
                rows.append({str(k)[1:]: _value(v, consts) for k, v in zip(kv[::2], kv[1::2])})
            return rows
    raise KeyError(table)


def fields(text: str | None = None) -> list[dict]:
    """The rows of ``*sandover-fields*`` with the unset columns at 0, as the game reads them."""
    text = text if text is not None else IMPL.read_text(encoding="utf-8")
    consts = constants(text) | {"PC_DEFAULT_MSAA": _pc_default_msaa()}
    zero = {"kind": 0, "offset": 0, "fallback": 0, "lo": 0.0, "hi": 0.0, "flags": 0, "size": 0, "default": 0, "boot": 0}
    return [zero | r for r in _rows(text, "*sandover-fields*", "sandover-field", consts)]


def menu(text: str | None = None) -> list[dict]:
    """The rows of ``*sandover-menu*``: name, field key, parent key (None), carousel (text-id names or None)."""
    text = text if text is not None else MENU.read_text(encoding="utf-8")
    return [{"name": r["name"], "field": r["field"], "parent": r.get("parent"), "carousel": r.get("carousel")}
            for r in _rows(text, "*sandover-menu*", "sandover-menu-row", {})]


def _pc_default_msaa() -> int:
    h = (REPO / "goal_src" / "jak1" / "pc" / "pckernel-h.gc").read_text(encoding="utf-8")
    return int(re.search(r"^\(defconstant PC_DEFAULT_MSAA (\d+)\)", h, flags=re.M).group(1))


#: size (also the alignment) of each scalar field type in the GOAL ABI
_SIZE = {"uint8": 1, "int8": 1, "uint16": 2, "int16": 2, "int32": 4, "uint32": 4, "float": 4, "symbol": 4,
         "string": 4, "int64": 8, "uint64": 8}


def layout(text: str | None = None) -> dict[str, int]:
    """Byte offset of every field of ``sandover-settings`` (``:pack-me``: no padding but each field's own
    alignment); an unknown field type raises."""
    text = text if text is not None else IMPL.read_text(encoding="utf-8")
    block = text[text.index("(deftype sandover-settings"):]
    block = block[:block.index(":pack-me")]
    block = block[block.index("((") :]
    out, off = {}, 0
    for line in block.splitlines():
        m = re.match(r"^\s*\(\(?([a-z0-9?-]+)\s+(\S+?)(?:\s+(\d+))?\)+\s*(?:;.*)?$", line)
        if not m:
            if line.strip():
                raise ValueError(f"unreadable deftype line: {line!r}")
            continue
        name, typ, n = m.groups()
        if typ not in _SIZE:
            raise ValueError(f"unknown field type {typ!r} for {name}")
        size = _SIZE[typ]
        off = -(-off // size) * size
        out[name] = off
        off += size * (int(n) if n else 1)
    out["<size>"] = off
    return out
