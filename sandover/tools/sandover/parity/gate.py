"""Parity gate logic: capture with a pinned build, compare capture dirs, calibrate bands.

Data layout under ``$SANDOVER_DATA``::

    parity/bin/<engine sha>-<staged name>/   pinned binaries; the rig prunes only ``bin/``
    parity/calibration.json                  F0 and m per (set, resolution, MSAA, engine sha)
    parity/calib/<key>/                       calibration runs, error maps, perturbation table
    parity/runs/<stamp>-<kind>/               check and determinism runs
    goldens/<set>/<engine sha>/               golden PNGs, sidecars, manifest.json
    goldens/<set>/noise.json                  per-shot noise floor from the last calibration
    ci/<stamp>-<sha>/                         runs started by ``sandover-parity watch``
"""

from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
import sys
import time
from dataclasses import asdict
from datetime import UTC, datetime
from pathlib import Path

import numpy as np

from sandover import engine
from sandover.parity import metrics
from sandover.paths import data_dir, root
from sandover.rig import capture

SCHEMA = 1
SHOTS_DIR = Path(__file__).resolve().parents[2].parent / "shots"
CAMERA_TOL = 1e-3


def now() -> str:
    return datetime.now(UTC).isoformat(timespec="seconds")


def stamp() -> str:
    return datetime.now().strftime("%Y%m%d-%H%M%S")


def rel(p: Path) -> str:
    """Path relative to ``$SANDOVER_DATA`` for files that record locations."""
    try:
        return Path(p).resolve().relative_to(data_dir().resolve()).as_posix()
    except ValueError:
        return Path(p).name


def manifest_path(set_or_path: str) -> Path:
    p = Path(set_or_path)
    if p.suffix in (".yaml", ".yml") and p.exists():
        return p
    p = SHOTS_DIR / f"{set_or_path}.yaml"
    if not p.exists():
        raise SystemExit(f"no shot manifest {p.name} in {SHOTS_DIR}")
    return p


def load_set(set_or_path: str) -> tuple[str, list[capture.Shot], str]:
    """(set name, shots, digest of the expanded shot list)."""
    name, shots = capture.load_manifest(manifest_path(set_or_path))
    blob = json.dumps([asdict(s) for s in shots], sort_keys=True).encode()
    return name, shots, hashlib.sha256(blob).hexdigest()[:16]


def _git(*args: str) -> str:
    return subprocess.run(["git", "-C", str(root()), *args], capture_output=True, text=True,
                          check=True).stdout.strip()


def goal_src_state() -> dict:
    """Checkout commit and whether ``goal_src`` has uncommitted changes: goalc compiles GOAL
    code from the checkout at capture time, not from the pinned binaries."""
    try:
        head = _git("rev-parse", "--short", "HEAD")
        dirty = _git("status", "--porcelain", "--untracked-files=no", "--", "goal_src")
        return {"checkout_sha": head, "goal_src_dirty": bool(dirty)}
    except (OSError, subprocess.CalledProcessError):
        return {"checkout_sha": None, "goal_src_dirty": None}


def pinned_root() -> Path:
    return data_dir() / "parity" / "bin"


def sha_of_bin(bin_dir: Path) -> tuple[str | None, str]:
    """(engine sha, reason if None) from ``pin.json`` or the staged copy's ``build.json``."""
    pin = bin_dir / "pin.json"
    if pin.exists():
        return json.loads(pin.read_text(encoding="utf-8"))["engine_sha"], ""
    info = engine.build_info(bin_dir)
    if info is None:
        return None, f"{bin_dir.name} has no {engine.BUILD_INFO} (staged before it existed)"
    return info.get("engine_sha"), info.get("reason") or ""


def require_sha(bin_dir: Path, override: str | None = None) -> str:
    """Engine sha of ``bin_dir``; exits when it is unknown and no ``--engine-sha`` was given."""
    sha, why = sha_of_bin(bin_dir)
    if override:
        if sha and sha != override:
            raise SystemExit(f"--engine-sha {override} contradicts the recorded {sha} of {bin_dir.name}")
        return override
    if not sha:
        raise SystemExit(f"engine sha of {bin_dir.name} is unknown: {why}. "
                         "Rebuild and re-stage from a clean checkout, or pass --engine-sha.")
    return sha


def resolve_build(build: str, pin: bool, sha: str | None = None) -> Path:
    """``current`` stages the current build; otherwise a directory, or a name under
    ``parity/bin`` or ``bin``. With ``pin``, the copy is copied to ``parity/bin`` under its
    recorded engine sha (or ``sha``) so later rebuilds and pruning cannot touch it."""
    if build == "current":
        src = engine.staged_bin()
    else:
        cands = [Path(build), pinned_root() / build, data_dir() / "bin" / build]
        src = next((c for c in cands if (c / f"gk{engine.EXE}").exists()), None)
        if src is None:
            raise SystemExit(f"no binaries at {build}")
    if not pin or src.parent == pinned_root():
        return src
    sha = require_sha(src, sha)
    dest = pinned_root() / f"{sha}-{src.name}"
    if not (dest / "pin.json").exists():
        tmp = dest.with_name(dest.name + ".tmp")
        shutil.rmtree(tmp, ignore_errors=True)
        shutil.copytree(src, tmp)
        info = engine.build_info(src) or {}
        (tmp / "pin.json").write_text(json.dumps(
            {"engine_sha": sha, "sha_source": "build.json" if info.get("engine_sha") == sha else "--engine-sha",
             "head": info.get("head"), "staged": src.name, "pinned": now()}, indent=2) + "\n", encoding="utf-8")
        tmp.rename(dest)
    return dest


def run_capture(set_name: str, shots: list[capture.Shot], digest: str, bin_dir: Path, out_dir: Path,
                parallel: int, build_kind: str) -> tuple[int, float, dict]:
    """Capture ``shots`` with ``bin_dir`` into ``out_dir`` and stamp the sidecars with the
    set, digest, engine sha and GOAL source state. Returns (exit code, wall seconds, source)."""
    src = goal_src_state()
    t0 = time.monotonic()
    rc = capture.run_shots(set_name, shots, out_dir, parallel, None, bin_dir)
    secs = time.monotonic() - t0
    if goal_src_state() != src:
        print("WARNING: the checkout changed during the capture; GOAL code may differ between shots",
              file=sys.stderr)
        src["changed_during_capture"] = True
    if src["goal_src_dirty"]:
        print("WARNING: goal_src has uncommitted changes; this capture is not reproducible from a sha",
              file=sys.stderr)
    sha = sha_of_bin(bin_dir)[0] or "unknown"
    for sh in shots:
        side = out_dir / f"{sh.name}.json"
        if side.exists():
            meta = json.loads(side.read_text(encoding="utf-8"))
            meta.update(schema=SCHEMA, set=set_name, manifest_digest=digest, build=build_kind,
                        engine_sha=sha, bin=bin_dir.name, goal_src=src)
            side.write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    return rc, secs, src


def compare_dirs(ref: Path, test: Path, names: list[str], maps_dir: Path | None = None,
                 precheck: bool = True) -> list[dict]:
    """Per-shot FLIP (and pre-checks) of ``test`` against ``ref``."""
    rows = []
    for n in names:
        pa, pb = ref / f"{n}.png", test / f"{n}.png"
        row: dict = {"shot": n}
        if not pa.exists() or not pb.exists():
            row["status"] = "missing"
            row["missing"] = [str(p.parent.name) for p in (pa, pb) if not p.exists()]
            rows.append(row)
            continue
        a, b = metrics.load(pa), metrics.load(pb)
        if precheck:
            pc = metrics.precheck(a, b)
            row["precheck"] = pc.to_dict()
            if not pc.ok:
                row["status"] = "precheck"
                rows.append(row)
                continue
        mean, fmap = metrics.flip(a, b)
        row.update(flip_mean=mean, flip_max=float(fmap.max()),
                   px_differ=int(np.any(a != b, axis=-1).sum()),
                   max_abs_diff=int(np.abs(a.astype(np.int16) - b).max()))
        if maps_dir is not None:
            metrics.save_error_map(fmap, maps_dir / f"{n}.flip.png")
        row["camera"] = camera_match(pa.with_suffix(".json"), pb.with_suffix(".json"))
        rows.append(row)
    return rows


SIDECAR_KEYS = ("continue", "hour", "resolution", "msaa", "camera", "effects", "wind", "jak", "particles",
                "settle_frames")


def camera_match(sa: Path, sb: Path) -> dict:
    """Sidecar agreement: staging fields equal and camera trans/rot within :data:`CAMERA_TOL`."""
    if not sa.exists() or not sb.exists():
        return {"ok": False, "reason": "sidecar missing"}
    a, b = (json.loads(p.read_text(encoding="utf-8")) for p in (sa, sb))
    diff = [k for k in SIDECAR_KEYS if a.get(k) != b.get(k)]
    if diff:
        return {"ok": False, "reason": "fields differ: " + ", ".join(diff)}
    va = np.array(a.get("camera_trans", []) + a.get("camera_rot", []), dtype=np.float64)
    vb = np.array(b.get("camera_trans", []) + b.get("camera_rot", []), dtype=np.float64)
    if va.shape != vb.shape:
        return {"ok": False, "reason": "camera missing"}
    d = float(np.abs(va - vb).max()) if va.size else 0.0
    return {"ok": d <= CAMERA_TOL, "bit_equal": bool(np.array_equal(va, vb)), "max_delta": d,
            "reason": "" if d <= CAMERA_TOL else f"camera differs by {d:g}"}


def summarize(rows: list[dict]) -> dict:
    means = [r["flip_mean"] for r in rows if "flip_mean" in r]
    if not means:
        return {"shots": 0}
    worst = max(rows, key=lambda r: r.get("flip_mean", -1))
    return {"shots": len(means), "mean_of_means": float(np.mean(means)), "median": float(np.median(means)),
            "p99": float(np.percentile(means, 99)), "worst": worst["flip_mean"], "worst_shot": worst["shot"],
            "worst_max": max(r["flip_max"] for r in rows if "flip_max" in r),
            "zero_px_shots": sum(1 for r in rows if r.get("px_differ") == 0)}


def perturbation_table(ref: Path, names: list[str]) -> dict[str, dict]:
    """FLIP of each known-small post-process perturbation of the reference against itself."""
    out: dict[str, dict] = {}
    per: dict[str, dict[str, float]] = {k: {} for k in metrics.PERTURBATIONS}
    for n in names:
        p = ref / f"{n}.png"
        if not p.exists():
            continue
        img = metrics.load(p)
        for k, fn in metrics.PERTURBATIONS.items():
            per[k][n] = metrics.flip(img, fn(img))[0]
    for k, vals in per.items():
        v = np.array(list(vals.values()))
        low = min(vals, key=vals.get)
        out[k] = {"min": float(v.min()), "median": float(np.median(v)), "max": float(v.max()),
                  "min_shot": low, "per_shot": vals}
    return out


def bands(floor_means: list[float], perturb: dict[str, dict]) -> dict:
    """F0 = p99 of the per-shot noise floor; ``m`` puts ``F0 + m`` halfway between F0 and the
    smallest FLIP any known-small perturbation gives on any shot."""
    f0 = float(np.percentile(floor_means, 99))
    kind = min(perturb, key=lambda k: perturb[k]["min"])
    p_min = perturb[kind]["min"]
    m = max(0.0, (p_min - f0) / 2)
    return {"F0": f0, "m": m, "threshold": f0 + m, "registers_at": p_min, "registers_kind": kind,
            "separable": p_min > f0}


def cal_key(set_name: str, resolution: tuple[int, int], msaa: int, sha: str) -> str:
    return f"{set_name}|{resolution[0]}x{resolution[1]}|msaa{msaa}|{sha}"


def cal_path() -> Path:
    return data_dir() / "parity" / "calibration.json"


def load_calibration() -> dict:
    p = cal_path()
    if p.exists():
        return json.loads(p.read_text(encoding="utf-8"))
    return {"schema": SCHEMA, "entries": {}}


def find_calibration(set_name: str, resolution, msaa: int, sha: str) -> tuple[str, dict] | None:
    """Exact key, else the newest entry for the same set, resolution and MSAA."""
    entries = load_calibration()["entries"]
    key = cal_key(set_name, tuple(resolution), msaa, sha)
    if key in entries:
        return key, entries[key]
    prefix = key.rsplit("|", 1)[0] + "|"
    same = [(k, e) for k, e in entries.items() if k.startswith(prefix)]
    return max(same, key=lambda kv: kv[1]["created"]) if same else None


def goldens_dir(set_name: str) -> Path:
    return data_dir() / "goldens" / set_name


def golden_generations(set_name: str) -> list[tuple[Path, dict]]:
    """Published generations, newest first."""
    gens = []
    for d in goldens_dir(set_name).glob("*"):
        mf = d / "manifest.json"
        if d.is_dir() and mf.exists():
            gens.append((d, json.loads(mf.read_text(encoding="utf-8"))))
    return sorted(gens, key=lambda g: g[1]["created"], reverse=True)


def file_sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()
