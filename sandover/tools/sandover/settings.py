"""The Sandover settings files, ``sandover.gc`` (flat screen) and ``sandover-vr.gc`` (the VR profile,
used while an OpenXR session runs), next to the stock ``pc-settings.gc``.

The game reads and writes it (``goal_src/jak1/pc/pckernel.gc``); this module mirrors that format
so tools can seed, read and check it. Layout::

    (sandover 3
      (ssao? #t)
      (material-look bold)
      (paper-white-nits 200)
      (hdr-peak-nits -1)
      )

Both profiles hold the same keys; the VR file adds ``msaa`` (the flat screen's MSAA is the stock row
in ``pc-settings.gc``). The leading integer is the schema version. Unknown keys are skipped by the game's reader (so an
older build reads a newer file) and by :func:`parse`; missing keys keep their default.

CLI::

    sandover-settings check <file>            # report problems, exit 1 if any
    sandover-settings seed-off <config dir>   # write all-off files, both profiles (vanilla parity)
    sandover-settings show <file>             # effective values after defaults
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import NamedTuple

VERSION = 4


class Range(NamedTuple):
    """A numeric key: the game clamps to ``[lo, hi]``; with ``auto``, a negative value means
    "what the display reports" and reads as -1. ``digits`` is the game's write precision."""

    lo: float
    hi: float
    auto: bool = False
    digits: int = 0


#: key -> allowed values (booleans are ``#t``/``#f``); order is the game's write order
KEYS: dict[str, tuple[str, ...] | type | Range] = {
    "vanilla-rendering?": bool,
    "ssao?": bool,
    "tile-lights?": bool,
    "bloom?": bool,
    "shadows?": bool,
    "materials?": bool,
    "material-look": ("faithful", "bold", "cinematic"),
    "fur?": bool,
    "water?": bool,
    "neighbours?": bool,
    "fp16-target?": bool,
    "tonemap": ("clamp", "filmic", "toon"),
    "hdr-output": ("scrgb", "pq", "off"),
    "paper-white-nits": Range(80, 400),
    "hdr-peak-nits": Range(400, 10000, auto=True),
    "hdr-black-nits": Range(0, 1, auto=True, digits=3),
    "render-scale": (100, 77, 67, 59, 50, 33),
    "upscaler": ("fsr3", "dlss", "fsr4"),
    "ssao-quality": ("low", "medium", "high", "ultra"),
    "shadow-quality": ("low", "medium", "high"),
    "sun?": bool,
    "glint?": bool,
    "contact-shadows?": bool,
    "motion-blur": ("off", "weak", "medium", "strong"),
    "camera-blur": ("off", "vehicles", "always"),
    # spatial (Windows Spatial Sound: Atmos, Sonic, DTS) is read by gk at start, before OpenAL
    "audio-output": ("auto", "stereo", "5.1", "7.1", "headphones", "spatial"),
    "audio-device": str,
}

#: the game's defaults (``sandover-settings-reset!``): the play build's look, everything on
DEFAULTS: dict[str, object] = {
    "vanilla-rendering?": False, "ssao?": True, "tile-lights?": True, "bloom?": True, "shadows?": True, "materials?": True,
    "material-look": "bold", "fur?": True, "water?": True, "neighbours?": True,
    "fp16-target?": True, "tonemap": "toon", "hdr-output": "pq", "paper-white-nits": 200,
    "hdr-peak-nits": -1, "hdr-black-nits": -1.0,
    "render-scale": 100, "upscaler": "fsr3",
    "ssao-quality": "medium", "shadow-quality": "high", "sun?": True, "glint?": True,
    "contact-shadows?": False,
    "motion-blur": "medium", "camera-blur": "vehicles",
    "audio-output": "auto", "audio-device": "",
}

#: keys only the VR profile's file holds: the stock MSAA row's value while the VR profile runs
VR_KEYS: dict[str, tuple[int, ...]] = {"msaa": (1, 2, 4, 8, 16)}

#: the VR profile's render scale: 67% of the eye frame, under 4K-class pixels per eye (8.3 Mpx) on a
#: PSVR2 (each eye is drawn at 4491x3468 at 100%, 3009x2324 or 7.0 Mpx at 67%)
VR_RENDER_SCALE = 67

#: the VR profile's defaults (``sandover-settings-reset-vr!``): the flat defaults but the render
#: scale; a file without ``msaa`` starts from the flat screen's MSAA
VR_DEFAULTS: dict[str, object] = {**DEFAULTS, "render-scale": VR_RENDER_SCALE}

#: the engine's boot state: vanilla rendering, nothing is pushed, so a boot matches upstream (rig and
#: parity slots)
ALL_OFF: dict[str, object] = {
    **{k: False for k, v in KEYS.items() if v is bool}, "vanilla-rendering?": True,
    "material-look": "bold", "tonemap": "clamp", "hdr-output": "off", "paper-white-nits": 200,
    "hdr-peak-nits": -1, "hdr-black-nits": -1.0,
    "render-scale": 100, "upscaler": "fsr3",
    "ssao-quality": "medium", "shadow-quality": "high", "motion-blur": "off",
    "camera-blur": "off", "audio-output": "stereo", "audio-device": "",
}

#: ambient occlusion tiers before version 4 (deep-low, deep-medium, deep) -> today's names
SSAO_QUALITY_V3 = {"low": "low", "medium": "low", "high": "medium"}

FILE_NAME = "sandover.gc"
VR_FILE_NAME = "sandover-vr.gc"
_TOKEN = re.compile(r'"[^"]*"|[()]|[^\s()]+')


def settings_path(config_dir: Path, vr: bool = False) -> Path:
    """``sandover.gc`` (or ``sandover-vr.gc``) for a gk ``--config-path`` directory."""
    return Path(config_dir) / "OpenGOAL" / "jak1" / "settings" / (VR_FILE_NAME if vr else FILE_NAME)


def is_vr_file(path: Path) -> bool:
    return Path(path).name == VR_FILE_NAME


def _atom(value: object) -> str:
    if isinstance(value, bool):
        return "#t" if value else "#f"
    return str(value)


#: the longest device name the game keeps (its field holds 128 bytes with the terminator)
DEVICE_NAME_BYTES = 127


def _write_atom(key: str, value: object) -> str:
    kind = KEYS.get(key) or VR_KEYS[key]
    if kind is str:
        return '"' + str(value).replace('"', "") + '"'
    if isinstance(kind, Range):
        return f"{float(value):.{kind.digits}f}" if kind.digits else str(int(value))
    return _atom(value)


def dumps(values: dict[str, object] | None = None, vr: bool = False) -> str:
    """The file text for ``values`` (missing keys take :data:`DEFAULTS`, or :data:`VR_DEFAULTS`).
    The VR file writes ``msaa`` when ``values`` has it, as the game does."""
    v = {**(VR_DEFAULTS if vr else DEFAULTS), **(values or {})}
    lines = [f"(sandover {VERSION}"]
    lines += [f"  ({k} {_write_atom(k, v[k])})" for k in KEYS]
    if vr:
        lines += [f"  ({k} {_write_atom(k, v[k])})" for k in VR_KEYS if k in v]
    lines.append("  )")
    return "\n".join(lines) + "\n"


def write(path: Path, values: dict[str, object] | None = None) -> Path:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(dumps(values, vr=is_vr_file(path)), encoding="utf-8", newline="\n")
    return path


def seed_off(config_dir: Path) -> Path:
    """Write all-off ``sandover.gc`` and ``sandover-vr.gc`` into a gk config dir so the game pushes
    nothing at boot in either profile; the VR file names no MSAA, so it takes the slot's. Returns
    the flat file."""
    write(settings_path(config_dir, vr=True), ALL_OFF)
    return write(settings_path(config_dir), ALL_OFF)


def _parse_tree(text: str):
    stack: list[list] = [[]]
    for tok in _TOKEN.findall(text):
        if tok == "(":
            stack.append([])
        elif tok == ")":
            if len(stack) == 1:
                raise ValueError("unbalanced )")
            done = stack.pop()
            stack[-1].append(done)
        else:
            stack[-1].append(tok)
    if len(stack) != 1:
        raise ValueError("unbalanced (")
    return stack[0]


def _value(key: str, raw: str) -> object:
    kind = KEYS[key]
    if kind is str:
        # a bare word reads as empty; the game cuts the name at its field size
        text = raw[1:-1] if len(raw) >= 2 and raw[0] == raw[-1] == '"' else ""
        return text.encode("utf-8")[:DEVICE_NAME_BYTES].decode("utf-8", "ignore")
    if kind is bool:
        return raw != "#f"  # the game reads any symbol but #f as on
    if isinstance(kind, Range):
        v = float(raw) if kind.digits else int(float(raw))
        if kind.auto and v < 0:
            return -1.0 if kind.digits else -1
        # the black reader only caps the top: a value under 0 means the display's
        return min(kind.hi, v) if kind.auto and kind.digits else max(kind.lo, min(kind.hi, v))
    if isinstance(kind[0], int):
        return int(raw, 0)
    return raw


def parse(text: str, vr: bool = False) -> tuple[int, dict[str, object], list[str]]:
    """``(version, values, problems)``. Values start from :data:`DEFAULTS` (:data:`VR_DEFAULTS`
    with ``vr``); unknown keys are skipped and reported, like the game's reader. Raises
    ``ValueError`` if the game would reject the file (not a single ``(sandover N ...)`` form)."""
    tree = _parse_tree(text)
    if len(tree) != 1 or not isinstance(tree[0], list) or not tree[0] or tree[0][0] != "sandover":
        raise ValueError("not a (sandover N ...) form")
    form = tree[0]
    try:
        version = int(form[1], 0)
    except (IndexError, TypeError, ValueError):
        raise ValueError("missing version") from None
    values = dict(VR_DEFAULTS if vr else DEFAULTS)
    problems: list[str] = []
    if version > VERSION:
        problems.append(f"version {version} is newer than {VERSION}: unknown keys are skipped")
    for entry in form[2:]:
        if not isinstance(entry, list) or not entry or isinstance(entry[0], list):
            raise ValueError(f"not a (key value) pair: {entry!r}")
        key = entry[0]
        if vr and key in VR_KEYS:
            if len(entry) != 2 or isinstance(entry[1], list):
                raise ValueError(f"{key}: expected one value")
            try:
                v = int(entry[1], 0)
            except ValueError:
                raise ValueError(f"{key}: {entry[1]!r} is not an integer") from None
            if v not in VR_KEYS[key]:
                problems.append(f"{key}: {v} is not one of {VR_KEYS[key]} (the game resets it to 2x)")
            values[key] = v
            continue
        if key not in KEYS:
            problems.append(f"unknown key {key!r} (skipped)")
            continue
        if len(entry) != 2 or isinstance(entry[1], list):
            raise ValueError(f"{key}: expected one value")
        kind, raw = KEYS[key], entry[1]
        if key == "ssao-quality" and version < 4:
            raw = SSAO_QUALITY_V3.get(raw, raw)
        if kind is str:
            if not (len(raw) >= 2 and raw[0] == raw[-1] == '"'):
                problems.append(f"{key}: {raw!r} is not a quoted name (read as the Windows default)")
        elif kind is bool:
            if raw not in ("#t", "#f"):
                problems.append(f"{key}: {raw!r} is not #t or #f (read as on)")
        elif isinstance(kind, Range):
            try:
                v = float(raw)
            except ValueError:
                raise ValueError(f"{key}: {raw!r} is not a number") from None
            if not (kind.lo <= v <= kind.hi or (kind.auto and v < 0)):
                problems.append(f"{key}: {raw} is outside {kind.lo}..{kind.hi} (clamped)")
        elif isinstance(kind[0], int):
            try:
                if int(raw, 0) not in kind:
                    problems.append(f"{key}: {raw} is not one of {kind} (value kept)")
                    continue
            except ValueError:
                raise ValueError(f"{key}: {raw!r} is not an integer") from None
        elif raw not in kind:
            problems.append(f"{key}: {raw!r} is not one of {kind} (value kept)")
            continue
        values[key] = _value(key, raw)
    if version < 2 and values["hdr-output"] == "scrgb":
        values["hdr-output"] = "pq"  # version 1 saved the old scRGB default
    if version < 3:
        values["sun?"] = values["neighbours?"]  # the sun rode on the neighbours switch
        values["material-look"], values["tonemap"] = "bold", "toon"  # the picked looks; rows removed
    return version, values, problems


def check(path: Path) -> list[str]:
    """Problems in a file; a file the game would reject yields one problem starting ``rejected``."""
    try:
        _, _, problems = parse(Path(path).read_text(encoding="utf-8"), vr=is_vr_file(path))
    except ValueError as e:
        return [f"rejected: {e}"]
    return problems


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="sandover-settings", description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("check").add_argument("file", type=Path)
    sub.add_parser("show").add_argument("file", type=Path)
    sub.add_parser("seed-off").add_argument("config_dir", type=Path)
    a = ap.parse_args(argv)
    if a.cmd == "check":
        problems = check(a.file)
        for p in problems:
            print(p)
        return 1 if problems else 0
    if a.cmd == "show":
        _, values, problems = parse(a.file.read_text(encoding="utf-8"), vr=is_vr_file(a.file))
        for p in problems:
            print(f"# {p}")
        for k, v in values.items():
            print(f"{k} {_atom(v)}")
        return 0
    print(seed_off(a.config_dir))
    return 0


if __name__ == "__main__":
    sys.exit(main())
