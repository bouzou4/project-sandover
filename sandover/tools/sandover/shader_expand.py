"""Expand every shader program the way ``Shader::Shader`` does and compare two trees.

The splice table is read from ``Shader.cpp`` (``kSnippets``); a tree from before the table
existed gets the legacy order. Used to prove a shader refactor leaves each program's source
unchanged: ``dump`` a base and a head tree, then ``compare``.
"""

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

from sandover import paths

SHADER_DIR = Path("game/graphics/opengl_renderer/shaders")
CPP = Path("game/graphics/opengl_renderer")
VERT, FRAG, TESC, TESE = 1, 2, 4, 8
VERSIONS = {"jak1": ("1.0", "448.0"), "jak2": ("0.5", "416.0")}


@dataclass(frozen=True)
class Snippet:
    token: str
    file: str
    stages: int
    every: bool


_ALL = VERT | FRAG | TESC | TESE
# the order of the hand-written splices before the table existed
LEGACY = (
    Snippet("SSAO_NORMAL_VERT", "ssao_normal_vert.glsl", VERT, False),
    Snippet("SSAO_NORMAL_FRAG", "ssao_normal_frag.glsl", FRAG, False),
    Snippet("SSAO_FOG_T", "ssao_fog_t.glsl", FRAG, False),
    Snippet("ACTOR_SHADOW_FRAG", "actor_shadow_frag.glsl", FRAG, False),
    Snippet("CANOPY_FRAG", "canopy_frag.glsl", FRAG, True),
    Snippet("WORLD_SHADOW_FRAG", "world_shadow_frag.glsl", FRAG, True),
    Snippet("WORLD_SHADOW_VERT", "world_shadow_vert.glsl", VERT, True),
    Snippet("CAVE_DARK_FRAG", "cave_dark_frag.glsl", FRAG, False),
    Snippet("INSPECT_VERT", "inspect_vert.glsl", VERT, False),
    Snippet("INSPECT_FRAG", "inspect_frag.glsl", FRAG, False),
    Snippet("PBR_BG_VERT", "pbr_bg_vert.glsl", VERT, False),
    Snippet("PBR_BG_FRAG", "pbr_bg_frag.glsl", FRAG, False),
    Snippet("PBR_COMMON_FRAG", "pbr_common_frag.glsl", FRAG, False),
    Snippet("SWAMP_MIST_GLSL", "swamp_mist.glsl", FRAG, False),
    Snippet("SCREEN_HISTORY_GLSL", "screen_history.glsl", FRAG, False),
    Snippet("WATER_WAVES", "water_waves.glsl", _ALL, False),
    Snippet("WATER_SUN_GLINT", "water_sun_glint.glsl", FRAG, False),
    Snippet("XR_GS_VERT", "xr_gs.glsl", VERT | TESC | TESE, False),
    Snippet("TONEMAP_SDR_GLSL", "tonemap_sdr.glsl", FRAG, False),
    Snippet("LEVEL_HEIGHT_GLSL", "level_height.glsl", VERT | FRAG, True),
)

_STAGE = {"kStageVert": VERT, "kStageFrag": FRAG, "kStageTesc": TESC, "kStageTese": TESE}


def parse_table(shader_cpp: str) -> tuple[Snippet, ...]:
    """The ``kSnippets`` rows of Shader.cpp, or LEGACY for a tree that predates the table."""
    m = re.search(r"kSnippets\[\]\s*=\s*\{(.*?)\n\};", shader_cpp, re.S)
    if not m:
        return LEGACY
    rows = []
    for tok, fil, stages, every in re.findall(
        r'\{"(\w+)",\s*"([\w.]+)",\s*([\w| ]+?),\s*(true|false)\}', m.group(1)
    ):
        mask = 0
        for name in stages.split("|"):
            mask |= _STAGE[name.strip()]
        rows.append(Snippet(tok, fil, mask, every == "true"))
    return tuple(rows)


def splice(src: str, stage: int, table, read) -> str:
    """Apply the table to one stage's text: first use only, or every use with the file read once."""
    for sn in table:
        if not sn.stages & stage:
            continue
        at = src.find(sn.token)
        if at < 0:
            continue
        body = read(sn.file)
        if sn.every:
            src = body.join(src.split(sn.token))
        else:
            src = src[:at] + body + src[at + len(sn.token) :]
    return src


def programs(root: Path) -> list[tuple[str, str]]:
    """(shader name, fragment file stem) for every program the engine builds."""
    cpp = (root / CPP / "Shader.cpp").read_text()
    progs = []
    for m in re.finditer(r'at\(ShaderId::\w+\)\s*=\s*\{"(\w+)",\s*version(?:,\s*"(\w+)")?\}', cpp):
        progs.append((m.group(1), m.group(2) or m.group(1)))
    for f in sorted((root / CPP).glob("*.cpp")):
        for m in re.finditer(r'make_unique<Shader>\("(\w+)"', f.read_text()):
            progs.append((m.group(1), m.group(1)))
    seen, out = set(), []
    for p in progs:
        if p not in seen:
            seen.add(p)
            out.append(p)
    return out


def expand(root: Path, name: str, frag: str, version: str, table) -> dict[str, str]:
    """Spliced source of each stage of one program, before jitter injection (not modelled)."""
    height, scissor = VERSIONS[version]
    adjust = f"(512.0 / {scissor})"
    shaders = root / SHADER_DIR
    cache: dict[str, str] = {}

    def read(f):
        if f not in cache:
            cache[f] = (shaders / f).read_text()
        return cache[f]

    vert = splice((shaders / f"{name}.vert").read_text(), VERT, table, read)
    fragsrc = splice((shaders / f"{frag}.frag").read_text(), FRAG, table, read)
    out = {}
    tesc, tese = shaders / f"{name}.tesc", shaders / f"{name}.tese"
    if tesc.exists() and tese.exists():
        for key, p, stage in (("tesc", tesc, TESC), ("tese", tese, TESE)):
            t = splice(p.read_text(), stage, table, read)
            out[key] = t.replace("HEIGHT_SCALE", height).replace("SCISSOR_ADJUST", adjust)
    vert = vert.replace("HEIGHT_SCALE", height).replace("SCISSOR_HEIGHT", scissor)
    out["vert"] = vert.replace("SCISSOR_ADJUST", adjust)
    fragsrc = fragsrc.replace("SCISSOR_HEIGHT", scissor).replace("HEIGHT_SCALE", height)
    out["frag"] = fragsrc.replace("SCISSOR_ADJUST", adjust)
    return out


def dump(root: Path, out_dir: Path) -> int:
    table = parse_table((root / CPP / "Shader.cpp").read_text())
    n = 0
    for version in VERSIONS:
        for name, frag in programs(root):
            stem = name if frag == name else f"{name}+{frag}"
            for stage, text in expand(root, name, frag, version, table).items():
                dest = out_dir / version / f"{stem}.{stage}"
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_text(text, newline="\n")
                n += 1
    return n


def normalise(text: str) -> str:
    """Whitespace-insensitive form: each line stripped, blank lines dropped."""
    return "\n".join(s for s in (ln.strip() for ln in text.splitlines()) if s)


def compare(a: Path, b: Path) -> dict:
    names = sorted(p.relative_to(a).as_posix() for p in a.rglob("*") if p.is_file())
    other = sorted(p.relative_to(b).as_posix() for p in b.rglob("*") if p.is_file())
    res = {
        "files": len(names),
        "raw_identical": 0,
        "whitespace_only": [],
        "different": [],
        "only_in_base": sorted(set(names) - set(other)),
        "only_in_head": sorted(set(other) - set(names)),
    }
    for n in names:
        if n not in other:
            continue
        ta, tb = (a / n).read_text(), (b / n).read_text()
        if ta == tb:
            res["raw_identical"] += 1
        elif normalise(ta) == normalise(tb):
            res["whitespace_only"].append(n)
        else:
            res["different"].append(n)
    return res


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("dump", help="write every program's expanded stages under --out")
    d.add_argument("--root", type=Path, default=paths.root())
    d.add_argument("--out", type=Path, required=True)
    c = sub.add_parser("compare", help="compare two dumps after whitespace normalisation")
    c.add_argument("base", type=Path)
    c.add_argument("head", type=Path)
    c.add_argument("--json", type=Path)
    args = ap.parse_args(argv)
    if args.cmd == "dump":
        print(f"{dump(args.root, args.out)} stage files")
        return 0
    res = compare(args.base, args.head)
    if args.json:
        args.json.write_text(json.dumps(res, indent=1))
    print(
        f"{res['files']} files: {res['raw_identical']} byte-identical, "
        f"{len(res['whitespace_only'])} whitespace-only, {len(res['different'])} different, "
        f"{len(res['only_in_base'])} only in base, {len(res['only_in_head'])} only in head"
    )
    for n in res["whitespace_only"]:
        print("  whitespace:", n)
    for n in res["different"]:
        print("  DIFFERENT:", n)
    return 1 if res["different"] or res["only_in_base"] or res["only_in_head"] else 0


if __name__ == "__main__":
    sys.exit(main())
