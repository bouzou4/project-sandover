"""Player bug reports: validate a bundle and import it as a snapshot-run folder.

gk writes a bundle per report (``game/graphics/bug_report.h``): a folder ``<id>/`` with
``screenshot.png``, ``report.json`` (schema ``bug_report.schema.json`` next to this file) and,
for frames wider than 1920, ``preview.png``, plus ``sandover-bug-<id>.zip`` holding the same files
under ``sandover-bug-<id>/``. ``import`` turns one into ``$SANDOVER_DATA/runs/bug-<id>/`` in the
triage layout (``0001.png`` / ``0001.json``), so ``sandover-triage`` and
``sandover-capture from-snapshot`` work on it unchanged. The player's Sandover settings are
re-created by the sidecar's ``toggles``: the game reads ``bundle/sandover.gc`` and pushes it.

CLI::

    sandover-bug import <zip-or-folder> [--runs-dir DIR]
    sandover-bug validate <zip-or-folder-or-report.json>
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import zipfile
from dataclasses import dataclass, field
from pathlib import Path

from sandover.paths import data_dir

SCHEMA_PATH = Path(__file__).with_name("bug_report.schema.json")
REPORT = "report.json"
SCREENSHOT = "screenshot.png"
PREVIEW = "preview.png"
# what from-snapshot needs to put the rig camera where the player's was
ESSENTIAL = ("level", "continue", "jak_pos", "cam_pos", "cam_rows", "hour")
_PRIVATE = re.compile(r"(?<![A-Za-z0-9_])(?:[A-Za-z]:[\\/]|\\\\[^\\/\s]+[\\/])")
_USER_DIR = re.compile(r"(?i)(?:^|[\\/])(?:users|home|documents and settings)[\\/](?!<)[^\\/\s\"'<>|*?]+")
_ORG = re.compile(r"(?:OneDrive|SharePoint) - (?!<org>)\S")

# Mirror of bug_report::sanitize_with in game/graphics/bug_report.cpp; the tests check that these
# are the C++ patterns and run the sample set through the rules.
ABS_PATH = re.compile(
    r""""((?:[A-Za-z]:|\\\\[^\\/\s"']+)[\\/][^"\x01-\x1f]*)|'((?:[A-Za-z]:|\\\\[^\\/\s"']+)[\\/][^'\x01-\x1f]*)|(?:^|[^A-Za-z0-9_])((?:[A-Za-z]:|\\\\[^\\/\s"']+)[\\/][^"<>|*?\x01-\x1f]*)""")
UNIX_HOME = re.compile(r"""(^|[^A-Za-z0-9_.~-])/(home|Users)/[^/\s"'<>|*?]+""")
ORG_FOLDER = re.compile(r"""(OneDrive|SharePoint) - [^\\/"<>|*?\x01-\x1f]+""")
_ASCII_LOWER = str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz")


def sanitize_rules(env: dict[str, str], folders: list[tuple[str, str]] = ()) -> list[tuple[str, str, bool]]:
    """(from, to, whole word) in the order gk applies them. ``folders`` stands for the run, data,
    config, install and project folders gk knows, as (path, label)."""
    paths: list[tuple[str, str, bool]] = []

    def add(p: str, label: str):
        while len(p) > 1 and p[-1] in "/\\":
            p = p[:-1]
        if len(p) < 3:
            return
        paths.append((p.replace("\\", "/"), label, False))
        paths.append((p.replace("/", "\\"), label, False))

    for p, label in folders:
        add(p, label)
    for var, label in (("APPDATA", "<appdata>"), ("LOCALAPPDATA", "<localappdata>"), ("TEMP", "<temp>"),
                       ("TMP", "<temp>"), ("OneDriveCommercial", "<onedrive>"),
                       ("OneDriveConsumer", "<onedrive>"), ("OneDrive", "<onedrive>"),
                       ("USERPROFILE", "<home>"), ("HOME", "<home>")):
        add(env.get(var, ""), label)
    paths.sort(key=lambda r: -len(r[0]))
    names: list[tuple[str, str, bool]] = []
    for var in ("USERPROFILE", "HOME"):
        parts = [x for x in re.split(r"[\\/]", env.get(var, "")) if x and ":" not in x]
        for k, part in enumerate(parts):
            if len(part) >= 3 and part.translate(_ASCII_LOWER) not in ("users", "home", "documents and settings"):
                names.append((part, "<user>" if k + 1 == len(parts) else "<profile>", True))
    for var in ("USERNAME", "USER", "LOGNAME"):
        if len(env.get(var, "")) >= 2:
            names.append((env[var], "<user>", True))
    for var in ("COMPUTERNAME", "HOSTNAME", "USERDOMAIN"):
        if len(env.get(var, "")) >= 2:
            names.append((env[var], "<domain>" if var == "USERDOMAIN" else "<host>", True))
    names.sort(key=lambda r: -len(r[0]))
    return paths + names


def _replace_ci(text: str, old: str, new: str, word: bool) -> str:
    if not old:
        return text
    needle, hay = old.translate(_ASCII_LOWER), text.translate(_ASCII_LOWER)

    def alnum(c: str) -> bool:
        return c.isascii() and c.isalnum()

    pos = 0
    while (pos := hay.find(needle, pos)) >= 0:
        end = pos + len(needle)
        if word and ((pos > 0 and alnum(hay[pos - 1])) or (end < len(hay) and alnum(hay[end]))):
            pos = end
            continue
        text = text[:pos] + new + text[end:]
        hay = hay[:pos] + new.translate(_ASCII_LOWER) + hay[end:]
        pos += len(new)
    return text


def sanitize(text: str, rules: list[tuple[str, str, bool]]) -> str:
    """What gk writes for ``text`` (see ``sanitize_rules``)."""
    for old, new, word in rules:
        text = _replace_ci(text, old, new, word)

    def cut(m: re.Match) -> str:
        g = next(k for k in (1, 2, 3) if m.group(k) is not None)
        p = m.group(g).rstrip("/\\")
        i = max(p.rfind("/"), p.rfind("\\"))
        return m.group(0)[: m.start(g) - m.start()] + "<path>/" + (p[i + 1:] if i >= 0 else "")

    text = ABS_PATH.sub(cut, text)
    text = UNIX_HOME.sub(r"\1/\2/<user>", text)
    return ORG_FOLDER.sub(r"\1 - <org>", text)
_TYPES = {"object": dict, "array": list, "string": str, "boolean": bool, "null": type(None)}


def schema() -> dict:
    return json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))


def _is(value, kind: str) -> bool:
    if kind == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if kind == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    return isinstance(value, _TYPES[kind])


def validate(doc, sch: dict | None = None, root: dict | None = None, path: str = "") -> list[str]:
    """Problems of ``doc`` against the subset of JSON Schema the bundle schema uses
    (type, const, enum, required, properties, items, minItems/maxItems, minimum/maximum, $ref)."""
    root = root or sch or schema()
    sch = sch or root
    if "$ref" in sch:
        ref = sch["$ref"]
        if not ref.startswith("#/"):
            raise ValueError(f"unsupported $ref {ref}")
        target = root
        for part in ref[2:].split("/"):
            target = target[part]
        sch = {**target, **{k: v for k, v in sch.items() if k != "$ref"}}
    where = path or "report"
    out: list[str] = []
    kinds = sch.get("type")
    if kinds is not None:
        kinds = [kinds] if isinstance(kinds, str) else kinds
        if not any(_is(doc, k) for k in kinds):
            return [f"{where}: expected {' or '.join(kinds)}, got {type(doc).__name__}"]
    if "const" in sch and doc != sch["const"]:
        out.append(f"{where}: expected {sch['const']!r}")
    if "enum" in sch and doc not in sch["enum"]:
        out.append(f"{where}: {doc!r} is not one of {sch['enum']}")
    if isinstance(doc, dict):
        for key in sch.get("required", []):
            if key not in doc:
                out.append(f"{path + '.' if path else ''}{key}: missing")
        for key, sub in sch.get("properties", {}).items():
            if key in doc:
                out += validate(doc[key], sub, root, f"{path + '.' if path else ''}{key}")
    if isinstance(doc, list):
        if "minItems" in sch and len(doc) < sch["minItems"]:
            out.append(f"{where}: {len(doc)} items, expected at least {sch['minItems']}")
        if "maxItems" in sch and len(doc) > sch["maxItems"]:
            out.append(f"{where}: {len(doc)} items, expected at most {sch['maxItems']}")
        if "items" in sch:
            for k, item in enumerate(doc):
                out += validate(item, sch["items"], root, f"{where}[{k}]")
    if _is(doc, "number"):
        if "minimum" in sch and doc < sch["minimum"]:
            out.append(f"{where}: {doc} is below {sch['minimum']}")
        if "maximum" in sch and doc > sch["maximum"]:
            out.append(f"{where}: {doc} is above {sch['maximum']}")
    return out


def private_paths(doc) -> list[str]:
    """Strings in ``doc`` that still hold an absolute Windows path (gk should have replaced them)."""
    found: list[str] = []

    def walk(v):
        if isinstance(v, str):
            if _PRIVATE.search(v):
                found.append(v[:120])
        elif isinstance(v, dict):
            for x in v.values():
                walk(x)
        elif isinstance(v, list):
            for x in v:
                walk(x)

    walk(doc)
    return found


def _strings(v):
    if isinstance(v, str):
        yield v
    elif isinstance(v, dict):
        for x in v.values():
            yield from _strings(x)
    elif isinstance(v, list):
        for x in v:
            yield from _strings(x)


def privacy_problems(doc, env: dict[str, str] | None = None) -> list[str]:
    """What gk should have removed from ``doc``: absolute Windows paths, user folders, OneDrive or
    SharePoint organisation folders, and this machine's user and host names (``env``)."""
    env = dict(os.environ) if env is None else env
    names = [env[v] for v in ("USERNAME", "USER", "LOGNAME", "COMPUTERNAME", "HOSTNAME")
             if len(env.get(v, "")) >= 3]
    local = [re.compile(r"(?i)(?<![A-Za-z0-9])" + re.escape(n) + r"(?![A-Za-z0-9])") for n in names]
    out: list[str] = []
    for s in _strings(doc):
        if _PRIVATE.search(s):
            out.append(f"absolute path left in: {s[:120]}")
        if m := _USER_DIR.search(s):
            out.append(f"user folder left in: {m.group(0)[:120]}")
        if _ORG.search(s):
            out.append(f"organisation folder left in: {s[:120]}")
        if any(r.search(s) for r in local):
            out.append(f"local user or machine name left in: {s[:120]}")
    return out


@dataclass
class Bundle:
    report: dict
    screenshot: bytes | None
    preview: bytes | None = None
    source: str = ""
    problems: list[str] = field(default_factory=list)

    @property
    def missing_essentials(self) -> list[str]:
        return [k for k in ESSENTIAL if k not in self.report]


def _pick(names: list[str], leaf: str) -> str | None:
    hits = sorted((n for n in names if n == leaf or n.endswith("/" + leaf)), key=lambda n: n.count("/"))
    return hits[0] if hits else None


def load(src: Path, env: dict[str, str] | None = None) -> Bundle:
    """A bundle from its zip, its folder, or a bare ``report.json``; ``env`` as in
    ``privacy_problems``."""
    src = Path(src)
    if src.is_file() and src.suffix.lower() == ".zip":
        with zipfile.ZipFile(src) as z:
            names = [n for n in z.namelist() if not n.endswith("/")]
            rep = _pick(names, REPORT)
            if rep is None:
                raise ValueError(f"{src}: no {REPORT} in the zip")
            prefix = rep[: -len(REPORT)]

            def read(leaf: str) -> bytes | None:
                return z.read(prefix + leaf) if prefix + leaf in names else None

            report = json.loads(z.read(rep).decode("utf-8"))
            b = Bundle(report, read(SCREENSHOT), read(PREVIEW), str(src))
    elif src.is_dir():
        if not (src / REPORT).is_file():
            raise ValueError(f"{src}: no {REPORT} in the folder")
        report = json.loads((src / REPORT).read_text(encoding="utf-8"))
        shot, prev = src / SCREENSHOT, src / PREVIEW
        b = Bundle(report, shot.read_bytes() if shot.is_file() else None,
                   prev.read_bytes() if prev.is_file() else None, str(src))
    elif src.is_file():
        report = json.loads(src.read_text(encoding="utf-8"))
        shot = src.with_name(SCREENSHOT)
        b = Bundle(report, shot.read_bytes() if shot.is_file() else None, None, str(src))
    else:
        raise FileNotFoundError(src)
    b.problems = validate(b.report)
    if b.screenshot is None:
        b.problems.append(f"{SCREENSHOT}: missing")
    b.problems += privacy_problems(b.report, env)
    return b


_QUOTED = re.compile(r'^(\s*\([a-z0-9?-]+ )"([a-z0-9-]*)"\)', re.M)


def unquote_settings(text: str) -> str:
    """``sandover.gc`` with list values as bare symbols: builds before the bug-report one wrote
    them quoted, which their own reader (and the rig's) does not understand."""
    return _QUOTED.sub(r"\1\2)", text)


def settings_forms(gc_path: Path) -> list[str]:
    """GOAL forms that load the player's Sandover settings from ``gc_path`` over the defaults
    and push them."""
    p = gc_path.resolve().as_posix()
    return ["(sandover-settings-reset! (-> *pc-settings* sandover))",
            f'(sandover-settings-read (-> *pc-settings* sandover) "{p}")',
            "(sandover-settings-reapply)", "(sandover-settings-update)"]


def _run_dir(runs: Path, rid: str) -> Path:
    safe = re.sub(r"[^A-Za-z0-9_.-]", "_", rid) or "unknown"
    d = runs / f"bug-{safe}"
    k = 2
    while d.exists():
        d = runs / f"bug-{safe}-{k}"
        k += 1
    return d


def import_bundle(src: Path, runs_dir: Path | None = None) -> tuple[Path, Bundle]:
    """Unpack ``src`` into a new ``<runs>/bug-<id>/``; raises ValueError if the camera or level
    cannot be recovered. Other problems are returned in ``bundle.problems``."""
    b = load(src)
    if b.missing_essentials or b.screenshot is None:
        raise ValueError("cannot import, missing: " + ", ".join(
            b.missing_essentials + ([SCREENSHOT] if b.screenshot is None else [])))
    run = _run_dir(Path(runs_dir) if runs_dir else data_dir() / "runs", str(b.report.get("id", "unknown")))
    keep = run / "bundle"
    keep.mkdir(parents=True)
    (keep / REPORT).write_text(json.dumps(b.report, indent=2) + "\n", encoding="utf-8")
    if b.preview is not None:
        (keep / PREVIEW).write_bytes(b.preview)
    side = dict(b.report)
    side.update(index=1, status="kept", note=side.get("note"), tags=list(side.get("tags") or []),
                annotations=list(side.get("annotations") or []), bug_report_source=Path(b.source).name)
    sandover = ((b.report.get("settings") or {}).get("sandover") or {})
    text = sandover.get("text") if isinstance(sandover, dict) else None
    if text:
        gc = keep / "sandover.gc"
        gc.write_text(unquote_settings(text), encoding="utf-8", newline="\n")
        side["toggles"] = settings_forms(gc)
    else:
        side["toggles"] = list(side.get("toggles") or [])
    (run / "0001.png").write_bytes(b.screenshot)
    tmp = run / "0001.json.tmp"
    tmp.write_text(json.dumps(side, indent=2) + "\n", encoding="utf-8")
    tmp.replace(run / "0001.json")
    return run, b


def _summary(b: Bundle) -> str:
    r = b.report
    build = r.get("build") or {}
    disp = r.get("display") or {}
    hdr = disp.get("hdr") or {}
    gpu = (r.get("system") or {}).get("gpu") or {}
    hour = r.get("hour")
    return "\n".join([
        f"report {r.get('id')} ({r.get('source')}), build {build.get('id')} commit {build.get('commit')}",
        f"level {r.get('level')} continue {r.get('continue')} hour {hour:.2f}" if isinstance(hour, (int, float))
        else f"level {r.get('level')} continue {r.get('continue')}",
        f"os {(r.get('system') or {}).get('os')}; gpu {gpu.get('renderer')} ({gpu.get('driver')})",
        f"display {disp.get('mode')} {disp.get('window')} game {disp.get('game_resolution')} scale "
        f"{disp.get('render_scale')}; HDR presenting {hdr.get('presenting')} peak {hdr.get('peak_nits')}",
    ])


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="sandover-bug", description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    im = sub.add_parser("import", help="unpack a bundle into $SANDOVER_DATA/runs/bug-<id>/")
    im.add_argument("bundle", type=Path, help="sandover-bug-<id>.zip or its folder")
    im.add_argument("--runs-dir", type=Path, help="default: $SANDOVER_DATA/runs")
    va = sub.add_parser("validate", help="check a bundle against the sidecar schema")
    va.add_argument("bundle", type=Path)
    a = ap.parse_args(argv)
    try:
        if a.cmd == "validate":
            b = load(a.bundle)
            print(_summary(b))
            for p in b.problems:
                print(f"PROBLEM {p}")
            print("ok" if not b.problems else f"{len(b.problems)} problem(s)")
            return 0 if not b.problems else 1
        run, b = import_bundle(a.bundle, a.runs_dir)
    except (ValueError, OSError, zipfile.BadZipFile, json.JSONDecodeError) as exc:
        print(f"sandover-bug: {exc}", file=sys.stderr)
        return 1
    print(_summary(b))
    for p in b.problems:
        print(f"PROBLEM {p}")
    print(f"imported to {run}")
    print(f"re-shoot: sandover-capture from-snapshot {run / '0001.json'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
