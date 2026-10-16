"""Vanilla frame library: index every frame shot with the vanilla look, reuse it instead of re-shooting.

`sandover-vanilla index` writes $SANDOVER_DATA/vanilla/index.json; `find` looks a sidecar up;
the rig calls `lookup`/`reuse` before a vanilla-look capture and `register` after one.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import struct
import threading
from collections import Counter, defaultdict
from datetime import UTC, datetime
from pathlib import Path

from sandover.paths import data_dir, root

SCHEMA = 1
METER = 4096.0
ROT_STEP = 0.004  # rotation matrix element step, about the 0.25 degree tolerance
SINCE_FILE = "since.txt"  # commit or ISO time: older captures are not served (the rig changed)
ROOTS = ("rounds", "runs", "shots")
LOOK_KEYS = {"goal", "goal_after_load", "play_look", "forms", "ab_mask"}
PLAIN_KEYS = {"snapshot", "cutscenes"}
ORIGINAL_RX = re.compile(r"original|vanilla", re.I)
_LOCK = threading.Lock()


def tolerances() -> tuple[float, float]:
    """The rig's camera tolerances (m, deg); imported late because capture imports this module."""
    from sandover.rig import capture

    return capture.CAMERA_TOL_M, capture.CAMERA_TOL_DEG


def index_path() -> Path:
    return data_dir() / "vanilla" / "index.json"


def _q(v: float, step: float) -> float:
    return round(round(v / step) * step, 6)


def frame_flags(meta: dict) -> str:
    f = "".join(str(int(bool(meta.get(k)))) for k in ("effects", "wind", "particles", "jak"))
    jp = meta.get("jak_pos")
    jp = ",".join(f"{_q(c / METER, tolerances()[0]):g}" for c in jp) if jp else "-"
    return (f"fx{f}|j{jp}|msaa{meta.get('msaa', 4)}|{meta.get('actors', 'freeze')}"
            f"|s{meta.get('settle_frames', 60)}")


def coarse_key(meta: dict) -> str:
    """Bucket of every frame that could match: continue, hour, resolution and scene flags."""
    w, h = meta["resolution"]
    return f"{meta['continue']}|h{round(float(meta['hour']), 2):.2f}|{w}x{h}|{frame_flags(meta)}"


def pose(meta: dict) -> tuple[list[float], list[float]] | None:
    t, r = meta.get("camera_trans"), meta.get("camera_rot")
    if not t or not r:
        req = meta.get("camera_requested") or {}
        t, r = req.get("trans"), req.get("rot")
    return (list(t), list(r)) if t and r and len(t) == 3 and len(r) == 9 else None


def fingerprint(meta: dict) -> str:
    """Camera fingerprint: continue, hour (0.01), position (0.05 m), rotation (~0.25 deg), size."""
    p = pose(meta)
    if p is None:
        raise ValueError("sidecar has no camera pose")
    t, r = p
    pos = ",".join(f"{_q(c / METER, tolerances()[0]):g}" for c in t)
    rot = ",".join(f"{_q(c, ROT_STEP):g}" for c in r)
    return f"{coarse_key(meta)}|p{pos}|r{rot}"


def camera_error(a: tuple, b: tuple) -> tuple[float, float]:
    from sandover.rig import capture

    return capture.camera_error(a[0], a[1], b[0], b[1])


def _plain_toggles(tg: dict) -> bool:
    return not (set(tg) & LOOK_KEYS) and set(tg) <= PLAIN_KEYS


def is_vanilla_sidecar(meta: dict) -> bool:
    """True when the capture ran with no Sandover forms, presets, env overrides or render scale.
    Folder names, arm labels and a `vanilla` flag are hints only; this is the rule."""
    if not _plain_toggles(meta.get("toggles") or {}):
        return False
    env = meta.get("env") or {}
    if any(env.get(k) for k in ("SANDOVER_MATERIALS", "SANDOVER_PBR_MAPS", "SANDOVER_PBR_PRESET")):
        return False
    if (meta.get("post_load") or {}).get("applied"):
        return False
    return meta.get("render_scale") is None and meta.get("upscaler") is None


def vanilla_run(toggles: dict | None, setup: list[str] | None, shot) -> bool:
    """Will this shot be taken with the vanilla look (no forms, presets, waits or render scale)?
    An HDR capture needs a `pc-set-hdr-target` form, so it never counts as vanilla."""
    if setup or shot.goals_after_load or shot.wait_vista or shot.wait_log:
        return False
    if shot.render_scale is not None or shot.upscaler is not None:
        return False
    return _plain_toggles(toggles or {})


def reuse_allowed(bin_name: str | None, env: dict | None) -> bool:
    """Reuse is off when a bin or an env override is requested: a check of a new build must shoot."""
    keys = ("SANDOVER_MATERIALS", "SANDOVER_PBR_MAPS", "SANDOVER_PBR_PRESET")
    return not bin_name and not any((env or {}).get(k) for k in keys)


def png_size(path: Path) -> tuple[int, int] | None:
    try:
        with open(path, "rb") as f:
            head = f.read(24)
        if head[:8] == b"\x89PNG\r\n\x1a\n":
            return struct.unpack(">II", head[16:24])
    except OSError:
        pass
    return None


def _rel(p: Path) -> str:
    try:
        return p.resolve().relative_to(data_dir().resolve()).as_posix()
    except ValueError:
        return p.as_posix()


def _label_of(png: Path) -> str:
    """The round, run or set a frame belongs to: its leading path components under the data dir."""
    parts = _rel(png).split("/")
    return "/".join(parts[:3] if parts[0] == "rounds" else parts[:2])


def _read(p: Path) -> dict | None:
    try:
        d = json.loads(p.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return d if isinstance(d, dict) else None


def _is_sidecar(d: dict | None) -> bool:
    return bool(d) and "continue" in d and "hour" in d and "resolution" in d and pose(d) is not None \
        and d.get("camera") != "follow"


def _manifest_vanilla(rd: Path, man: dict) -> set[Path]:
    """PNGs a round manifest labels as the original game (a hint, checked against the sidecar)."""
    out = {rd / rel for rel in (man.get("vanilla") or {}).values()}
    for letter, c in (man.get("candidates") or {}).items():
        if ORIGINAL_RX.search(f"{c.get('label', '')} {c.get('preset', '')}"):
            out |= {rd / rel for rel in (man.get("images", {}).get(letter) or {}).values()}
    return out


def _entry(png: Path, meta: dict) -> dict:
    exr, js = png.with_suffix(".exr"), png.with_suffix(".json")
    proj = (meta.get("env") or {}).get("SANDOVER_GK_PROJ_PATH")
    size = png_size(png)
    own = js.exists()
    return {
        "png": _rel(png), "exr": _rel(exr) if exr.exists() else None,
        "json": _rel(js) if own else None,
        "bin": {"sha": meta.get("sha"), "root": Path(proj).name if proj else None},
        "resolution": list(size) if size else list(meta["resolution"]),
        "camera": meta.get("camera"), "continue": meta["continue"], "hour": meta["hour"],
        "trans": pose(meta)[0], "rot": pose(meta)[1],
        "coarse": coarse_key(meta), "round": _label_of(png),
        "capture": str(meta.get("timestamp")) if own and meta.get("timestamp")
        else f"mtime:{int(png.stat().st_mtime)}",
    }


def scan(data: Path | None = None, say=print) -> list[tuple[str, dict]]:
    """Every vanilla-look frame under rounds/, runs/ and shots/ as (fingerprint, entry). A frame
    is in only if its own sidecar passes :func:`is_vanilla_sidecar`; labels are hints, and a hint
    that fails the check is reported as RIG:VANILLA-MISMATCH."""
    data = data or data_dir()
    since = cutoff()
    out = []
    for top in ROOTS:
        for dirpath, _dirs, files in os.walk(data / top):
            d, names = Path(dirpath), set(files)
            hints: set[Path] = set()
            if "manifest.json" in names:
                man = _read(d / "manifest.json")
                hints = _manifest_vanilla(d, man) if man and "candidates" in man else set()
            named = any(c == "vanilla" or c.endswith("-vanilla") or c.startswith("vanilla-")
                        for c in _rel(d / "x").split("/")[:-1])
            for f in files:
                if not f.endswith(".json") or f == "manifest.json" or f[:-5] + ".png" not in names:
                    continue
                meta, png = _read(d / f), d / (f[:-5] + ".png")
                if not _is_sidecar(meta):
                    continue
                if not is_vanilla_sidecar(meta):
                    if named or png in hints:
                        say(f"RIG:VANILLA-MISMATCH {_rel(png)}: labelled vanilla, sidecar has forms or overrides")
                    continue
                if since and not newer_than(meta, since):
                    continue
                try:
                    out.append((fingerprint(meta), _entry(png, meta)))
                except (ValueError, KeyError, OSError):
                    continue
    return out


def build(data: Path | None = None) -> dict:
    groups: dict[str, list[dict]] = defaultdict(list)
    for fp, e in scan(data):
        groups[fp].append(e)
    frames = {}
    for fp, es in groups.items():
        es.sort(key=lambda e: e["capture"], reverse=True)  # newest first, as register keeps it
        best = dict(es[0])
        best["copies"] = [e["png"] for e in es[1:]]
        best["captures"] = len({e["capture"] for e in es})
        # a shoot of the same camera beyond the first, charged to the round that did it
        older = {}
        for e in es[1:]:
            if e["capture"] != best["capture"]:
                older.setdefault(e["capture"], e["round"])  # one per distinct shoot, copies excluded
        best["extra_rounds"] = list(older.values())
        frames[fp] = best
    return {"schema": SCHEMA, "built": datetime.now(UTC).isoformat(timespec="seconds"),
            "tolerance": dict(zip(("m", "deg"), tolerances())), "since": cutoff(), "frames": dict(sorted(frames.items()))}


def stats(idx: dict, top: int = 10) -> dict:
    fr = list(idx["frames"].values())
    waste = Counter(r for f in fr for r in f["extra_rounds"])
    return {"unique": len(fr), "pngs": sum(1 + len(f["copies"]) for f in fr),
            "duplicate_pngs": sum(len(f["copies"]) for f in fr),
            "duplicate_captures": sum(f["captures"] - 1 for f in fr),
            "refired": sum(1 for f in fr if f["captures"] > 1), "top_offenders": waste.most_common(top)}


def cutoff() -> str | None:
    """ISO time before which captures are not served: `since.txt` in the library folder."""
    try:
        v = (index_path().parent / SINCE_FILE).read_text(encoding="utf-8").strip()
    except OSError:
        return None
    return v or None


def resolve_since(value: str) -> str:
    """An ISO time as is, or the commit time of a commit-ish (git, run in the repo)."""
    import subprocess

    try:
        datetime.fromisoformat(value)
        return value
    except ValueError:
        pass
    r = subprocess.run(["git", "-C", str(root()), "show", "-s", "--format=%cI", value],
                       capture_output=True, text=True, check=False)
    if r.returncode or not r.stdout.strip():
        raise ValueError(f"not a time or a commit: {value}")
    return r.stdout.strip()


def newer_than(meta: dict, since: str) -> bool:
    try:
        return datetime.fromisoformat(str(meta["timestamp"])) >= datetime.fromisoformat(since)
    except (KeyError, ValueError, TypeError):
        return False  # no capture time: cannot show it postdates the cutoff


def write_index(idx: dict) -> Path:
    p = index_path()
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_suffix(".tmp")
    tmp.write_text(json.dumps(idx, indent=1) + "\n", encoding="utf-8")
    tmp.replace(p)
    return p


def load_index() -> dict:
    d = _read(index_path())
    return d if d and d.get("schema") == SCHEMA else {"schema": SCHEMA, "frames": {}}


def lookup(meta: dict, idx: dict | None = None) -> dict | None:
    """The indexed frame matching a sidecar-shaped dict within the rig's camera tolerance."""
    idx = idx or load_index()
    p = pose(meta)
    coarse, best, (tol_m, tol_deg) = coarse_key(meta), None, tolerances()
    for e in idx["frames"].values():
        if e["coarse"] != coarse:
            continue
        if p is None:  # continue-camera shot: the camera is the continue point's, whatever its pose
            if meta.get("camera") == "continue" and e["camera"] == "continue":
                return e
            continue
        d, a = camera_error(p, (e["trans"], e["rot"]))
        if d <= tol_m / 2 and a <= tol_deg / 2 and (best is None or d < best[0]):
            best = (d, e)
    return best[1] if best else None


def shot_meta(shot) -> dict | None:
    """A sidecar-shaped dict for a rig Shot, or None when its camera is not known up front."""
    if shot.pan or not (isinstance(shot.camera, dict) or shot.camera == "from_continue"):
        return None
    m = {"continue": shot.continue_point, "hour": shot.hour, "resolution": list(shot.resolution),
            "effects": shot.effects, "wind": shot.wind, "particles": shot.particles, "jak": shot.jak,
            "jak_pos": list(shot.jak_pos) if shot.jak_pos else None, "msaa": shot.msaa,
            "actors": shot.actors, "settle_frames": shot.settle_frames,
            "camera": "continue"}
    if isinstance(shot.camera, dict):
        m.update(camera="explicit", camera_trans=list(shot.camera["pos"]), camera_rot=list(shot.camera["rot"]))
    return m


def reuse(entry: dict, out_png: Path, name: str, say=print) -> bool:
    """Copy an indexed frame to ``out_png`` (+ EXR, JSON); the library files are never touched, and
    a copy (not a link) means editing a reused frame cannot change the library. False when the
    source is gone; a destination that is the source itself is left alone."""
    data = data_dir()
    src = data / entry["png"]
    if not src.exists():
        return False
    if out_png.exists() and os.path.samefile(src, out_png):
        say(f"RIG:VANILLA-REUSE {src} (already in place)")
        return True
    out_png.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, out_png)
    if entry.get("exr") and (data / entry["exr"]).exists():
        shutil.copyfile(data / entry["exr"], out_png.with_suffix(".exr"))
    js = data / entry["json"] if entry.get("json") else None
    meta = (_read(js) if js and js.exists() else None) or {"continue": entry["continue"], "hour": entry["hour"]}
    out_png.with_suffix(".json").write_text(
        json.dumps({**meta, "name": name, "vanilla_reuse": entry["png"]}, indent=2) + "\n", encoding="utf-8")
    say(f"RIG:VANILLA-REUSE {src}")
    return True


def register(png: Path, say=print) -> bool:
    """Add a frame the rig just shot with the vanilla look to the index (no rescan). Refuses a
    sidecar that is not strictly vanilla; the index is locked across processes."""
    meta = _read(png.with_suffix(".json"))
    if not _is_sidecar(meta) or not png.exists():
        return False
    if not is_vanilla_sidecar(meta):
        say(f"RIG:VANILLA-MISMATCH {png.name}: not registered, sidecar has forms or overrides")
        return False
    if (meta.get("camera_check") or {}).get("ok") is False or meta.get("vanilla_reuse"):
        return False
    from sandover import locks

    with _LOCK, locks.hold("vanilla-index", "sandover-vanilla register", timeout=60):
        idx = load_index()
        fp, e = fingerprint(meta), _entry(png, meta)
        old = idx["frames"].get(fp)
        e.update(copies=[], captures=1, extra_rounds=[])
        if old:
            e["copies"] = [old["png"], *old["copies"]]
            e["captures"] = old["captures"] + 1
            e["extra_rounds"] = [old["round"], *old["extra_rounds"]]
        idx["frames"][fp] = e
        write_index(idx)
    say(f"RIG:VANILLA-INDEXED {fp}")
    return True


def main() -> int:
    ap = argparse.ArgumentParser(prog="sandover-vanilla", description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    i = sub.add_parser("index", help="scan rounds/, runs/ and shots/ and write vanilla/index.json")
    i.add_argument("--top", type=int, default=10, help="offending rounds to list")
    i.add_argument("--since", metavar="COMMIT|ISO", help="only frames shot at or after this commit's "
                   "time (or ISO time); remembered in vanilla/since.txt")
    f = sub.add_parser("find", help="look a sidecar's camera up in the index")
    f.add_argument("sidecar", type=Path)
    args = ap.parse_args()
    if args.cmd == "index":
        if args.since:
            f = index_path().parent / SINCE_FILE
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_text(resolve_since(args.since) + "\n", encoding="utf-8")
        idx = build()
        p = write_index(idx)
        s = stats(idx, args.top)
        print(f"{s['unique']} unique vanilla frames ({s['pngs']} files); "
              f"{s['refired']} cameras shot more than once ({s['duplicate_captures']} wasted shoots), "
              f"{s['duplicate_pngs']} extra files; index {p}")
        for r, n in s["top_offenders"]:
            print(f"  {n:4d}  {r}")
        return 0
    meta = _read(args.sidecar)
    if not _is_sidecar(meta):
        ap.error(f"{args.sidecar}: not a capture sidecar")
    e = lookup(meta)
    if e is None:
        print("no vanilla frame for this camera")
        return 1
    print(json.dumps({k: e[k] for k in ("png", "exr", "json", "bin", "resolution")}, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
