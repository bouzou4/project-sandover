"""Parse ``level-info.gc`` (bsphere, continue points) and ``load-boundary-data.gc`` (load
pairs) into ``$SANDOVER_DATA/vista/level_info.json``. Units in the output are metres.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

from sandover.census import repo_root, sandover_data_dir

UNITS_PER_M = 4096.0
TOKEN = re.compile(r';[^\n]*|"[^"]*"|[()]|\'|[^\s()";\']+|\n')


def parse_sexprs(text: str) -> tuple[list, dict[int, int]]:
    """Minimal reader: lists, atoms and strings; quotes dropped. Returns exprs and list lines."""
    stack: list[list] = [[]]
    lines: dict[int, int] = {}
    line = 1
    for m in TOKEN.finditer(text):
        t = m.group()
        if t == "\n":
            line += 1
        elif t.startswith(";") or t == "'":
            continue
        elif t == "(":
            stack.append([])
            lines[id(stack[-1])] = line
        elif t == ")":
            done = stack.pop()
            stack[-1].append(done)
        else:
            stack[-1].append(t)
    return stack[0], lines


def kwargs(lst: list) -> dict:
    return {lst[i][1:]: lst[i + 1] for i in range(len(lst) - 1)
            if isinstance(lst[i], str) and lst[i].startswith(":")}


def atom(v):
    if v in (None, "#f"):
        return None
    if v == "#t":
        return True
    if isinstance(v, str) and v.startswith('"'):
        return v[1:-1]
    if isinstance(v, list):
        return [atom(x) for x in v]
    return v


def vec_m(v: list | None) -> list[float] | None:
    if v is None:
        return None
    d = kwargs(v)
    out = []
    for axis in "xyzw":
        x = d.get(axis, "0")
        out.append(float(x[1]) if isinstance(x, list) else float(x) / UNITS_PER_M)
    return [round(x, 3) for x in out]


def parse(level_info: Path, load_boundary: Path) -> dict:
    exprs, lines = parse_sexprs(level_info.read_text(encoding="utf-8"))
    levels = {}
    pairs: dict[frozenset, list] = {}
    for e in exprs:
        if not (isinstance(e, list) and len(e) >= 3 and e[0] == "define"
                and isinstance(e[2], list) and "level-load-info" in e[2]):
            continue
        d = kwargs(e[2])
        conts = []
        for cp in d.get("continues") or []:
            c = kwargs(cp)
            lev0, lev1 = atom(c.get("lev0")), atom(c.get("lev1"))
            conts.append({"name": atom(c.get("name")), "line": lines.get(id(cp)),
                          "trans_m": vec_m(c.get("trans"))[:3],
                          "camera_trans_m": (vec_m(c.get("camera-trans")) or [None] * 3)[:3],
                          "lev0": lev0, "lev1": lev1,
                          "disp0": atom(c.get("disp0")), "disp1": atom(c.get("disp1")),
                          "vis_nick": atom(c.get("vis-nick")),
                          "load_commands": atom(c.get("load-commands") or [])})
            if lev0 and lev1 and lev0 != lev1:
                pairs.setdefault(frozenset((lev0, lev1)), [lev0, lev1, []])[2].append(
                    f"level-info.gc:{lines.get(id(cp))}")
        bs = vec_m(d.get("bsphere"))
        levels[e[1]] = {"line": lines.get(id(e)), "bsphere_m": bs, "sky": atom(d.get("sky")),
                        "run_packages": atom(d.get("run-packages") or []), "continues": conts}
    load_re = re.compile(r"\(load\s+([a-z0-9-]+)\s+([a-z0-9-]+)\)")
    for i, ln in enumerate(load_boundary.read_text(encoding="utf-8").splitlines(), 1):
        for a, b in load_re.findall(ln):
            if a != b:
                pairs.setdefault(frozenset((a, b)), [a, b, []])[2].append(
                    f"load-boundary-data.gc:{i}")
    return {"units": "metres (game units / 4096)", "levels": levels,
            "adjacent_pairs": sorted(pairs.values())}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="sandover-vista-level-info", description=__doc__)
    lv = repo_root() / "goal_src" / "jak1" / "engine" / "level"
    ap.add_argument("--level-info", type=Path, default=lv / "level-info.gc")
    ap.add_argument("--load-boundary", type=Path, default=lv / "load-boundary-data.gc")
    ap.add_argument("--out", type=Path, default=sandover_data_dir() / "vista")
    a = ap.parse_args(argv)
    res = parse(a.level_info, a.load_boundary)
    a.out.mkdir(parents=True, exist_ok=True)
    (a.out / "level_info.json").write_text(json.dumps(res, indent=1))
    print(f"{len(res['levels'])} levels, {len(res['adjacent_pairs'])} adjacent pairs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
