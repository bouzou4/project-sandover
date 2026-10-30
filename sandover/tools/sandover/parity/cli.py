"""sandover-parity: the vanilla-parity gate.

Usage::

    sandover-parity calibrate --set auto-v1              # F0 and m -> parity/calibration.json
    sandover-parity golden --set auto-v1 --reason "..."   # goldens/<set>/<engine sha>/
    sandover-parity check --set auto-v1 --build current   # candidate vs golden; exit 1 on failure
    sandover-parity determinism --set auto-v0             # two runs of one build, FLIP per shot
    sandover-parity watch                                 # poll origin/master, check the latest staged build

``--build`` is ``current`` (stage the current Release build), a directory, or a staged
copy's name under ``$SANDOVER_DATA/parity/bin`` or ``$SANDOVER_DATA/bin``. Builds are manual.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

from sandover import engine
from sandover.parity import gate
from sandover.paths import data_dir, root


def _write(path: Path, obj) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, indent=2) + "\n", encoding="utf-8")


def _fmt(v: float) -> str:
    return f"{v:.6f}"


def _two_runs(args, set_name, shots, digest, bin_dir, base: Path,
              runs: int) -> tuple[list[Path], list[float], list[dict]]:
    dirs, secs, srcs = [], [], []
    for k in range(runs):
        d = base / f"run{k}"
        print(f"== run {k + 1}/{runs} -> {gate.rel(d)}", flush=True)
        rc, s, src = gate.run_capture(set_name, shots, digest, bin_dir, d, args.parallel, "vanilla")
        dirs.append(d)
        secs.append(s)
        srcs.append(src)
        if rc:
            print(f"run {k} had failed shots; comparing what exists", file=sys.stderr)
    return dirs, secs, srcs


def cmd_calibrate(args) -> int:
    set_name, shots, digest = gate.load_set(args.set)
    names = [s.name for s in shots]
    if args.runs:
        dirs, secs, srcs = [Path(p) for p in args.runs], [], []
        bin_name = sha = None
        for d in dirs:
            side = next(d.glob("*.json"), None)
            meta = json.loads(side.read_text(encoding="utf-8")) if side else {}
            if sha and meta.get("engine_sha") != sha:
                raise SystemExit(f"--runs were captured with different engine shas ({sha}, {meta.get('engine_sha')})")
            bin_name, sha = meta.get("bin"), meta.get("engine_sha")
            srcs.append(meta.get("goal_src"))
        if sha in (None, "unknown"):
            if not args.engine_sha:
                raise SystemExit("the --runs sidecars carry no engine sha; pass --engine-sha")
            sha = args.engine_sha
    else:
        bin_dir = gate.resolve_build(args.build, pin=True, sha=args.engine_sha)
        sha, bin_name = gate.require_sha(bin_dir), bin_dir.name
    res, msaa = shots[0].resolution, shots[0].msaa
    key = gate.cal_key(set_name, res, msaa, sha)
    base = data_dir() / "parity" / "calib" / key.replace("|", "_")
    if not args.runs:
        dirs, secs, srcs = _two_runs(args, set_name, shots, digest, bin_dir, base, 2)
    print("== noise floor (run0 vs run1)", flush=True)
    floor = gate.compare_dirs(dirs[0], dirs[1], names, base / "maps" / "noise", precheck=False)
    ok = [r for r in floor if "flip_mean" in r]
    if len(ok) < len(names):
        print(f"{len(names) - len(ok)} shots missing from a run", file=sys.stderr)
    print("== perturbations of run0", flush=True)
    pert = gate.perturbation_table(dirs[0], [r["shot"] for r in ok])
    b = gate.bands([r["flip_mean"] for r in ok], pert)
    entry = {
        "set": set_name, "manifest_digest": digest, "resolution": list(res), "msaa": msaa,
        "engine_sha": sha, "bin": bin_name, "created": gate.now(), **b,
        "noise": gate.summarize(floor), "capture_seconds": secs, "goal_src": srcs,
        "perturbations": {k: {kk: vv for kk, vv in v.items() if kk != "per_shot"} for k, v in pert.items()},
        "runs": [gate.rel(d) for d in dirs],
    }
    _write(base / "floor.json", floor)
    _write(base / "perturbations.json", pert)
    cal = gate.load_calibration()
    cal["entries"][key] = entry
    _write(gate.cal_path(), cal)
    _write(gate.goldens_dir(set_name) / "noise.json", {
        "schema": gate.SCHEMA, "set": set_name, "calibration": key, "created": entry["created"],
        "shots": {r["shot"]: {k: r.get(k) for k in ("flip_mean", "flip_max", "px_differ", "max_abs_diff")}
                  for r in ok}})
    print(f"\n{key}: F0 {_fmt(b['F0'])}  m {_fmt(b['m'])}  threshold {_fmt(b['threshold'])}  "
          f"(smallest perturbation {b['registers_kind']} {_fmt(b['registers_at'])})")
    print("| perturbation | min | median | max |\n|---|---|---|---|")
    for k, v in pert.items():
        print(f"| {k} | {_fmt(v['min'])} | {_fmt(v['median'])} | {_fmt(v['max'])} |")
    if not b["separable"]:
        print("WARNING: the smallest perturbation is inside the noise floor; m = 0", file=sys.stderr)
    return 0


def cmd_golden(args) -> int:
    set_name, shots, digest = gate.load_set(args.set)
    bin_dir = gate.resolve_build(args.build, pin=True, sha=args.engine_sha)
    sha = gate.require_sha(bin_dir)
    dest = gate.goldens_dir(set_name) / sha
    if dest.exists():
        if not args.replace:
            print(f"{gate.rel(dest)} exists; pass --replace to move it aside and re-capture", file=sys.stderr)
            return 2
        aside = dest.with_name(f"{sha}.replaced-{gate.stamp()}")
        dest.rename(aside)
        print(f"moved the old generation to {gate.rel(aside)}")
    tmp = dest.with_name(dest.name + ".tmp")
    shutil.rmtree(tmp, ignore_errors=True)
    rc, secs, src = gate.run_capture(set_name, shots, digest, bin_dir, tmp, args.parallel, "vanilla")
    missing = [s.name for s in shots if not (tmp / f"{s.name}.png").exists()]
    if rc or missing:
        print(f"golden not published; failed shots: {missing}", file=sys.stderr)
        return 1
    _write(tmp / "manifest.json", {
        "schema": gate.SCHEMA, "set": set_name, "manifest": gate.manifest_path(args.set).name,
        "manifest_digest": digest, "engine_sha": sha, "bin": bin_dir.name, "build": "vanilla",
        "goal_src": src, "created": gate.now(), "reason": args.reason,
        "resolution": list(shots[0].resolution),
        "msaa": shots[0].msaa, "capture_seconds": round(secs, 1), "parallel": args.parallel,
        "shots": [{"name": s.name, "png_sha256": gate.file_sha(tmp / f"{s.name}.png")} for s in shots]})
    tmp.rename(dest)
    gens = gate.golden_generations(set_name)
    print(f"golden {set_name}/{sha}: {len(shots)} shots in {secs:.0f}s -> {gate.rel(dest)}")
    if len(gens) > 2:
        old = ", ".join(d.name for d, _ in gens[2:])
        print(f"policy: keep the last two generations; older ones ({old}) are deleted only with approval")
    return 0


def _golden(set_name: str, which: str | None) -> tuple[Path, dict]:
    gens = gate.golden_generations(set_name)
    if which:
        gens = [g for g in gens if g[0].name == which or g[1]["engine_sha"] == which]
    if not gens:
        raise SystemExit(f"no golden for {set_name}{' ' + which if which else ''}; run sandover-parity golden")
    return gens[0]


def cmd_check(args) -> int:
    set_name, shots, digest = gate.load_set(args.set)
    gdir, gman = _golden(set_name, args.golden)
    if gman["manifest_digest"] != digest:
        print(f"WARNING: shot manifest changed since the golden ({gman['manifest_digest']} != {digest})",
              file=sys.stderr)
    found = gate.find_calibration(set_name, gman["resolution"], gman["msaa"], gman["engine_sha"])
    if found is None:
        raise SystemExit(f"no calibration for {set_name}; run sandover-parity calibrate")
    cal_key, cal = found
    if cal["engine_sha"] != gman["engine_sha"]:
        print(f"WARNING: no calibration for engine {gman['engine_sha']}; using {cal_key}", file=sys.stderr)
    threshold = cal["threshold"]
    out = Path(args.out) if args.out else data_dir() / "parity" / "runs" / f"{gate.stamp()}-check-{set_name}"
    if args.captures:
        cand, bin_name, secs = Path(args.captures), "(existing captures)", None
        side = next(cand.glob("*.json"), None)
        meta = json.loads(side.read_text(encoding="utf-8")) if side else {}
        cand_sha, src = meta.get("engine_sha") or "unknown", meta.get("goal_src")
    else:
        bin_dir = gate.resolve_build(args.build, pin=False)
        cand, bin_name = out / "captures", bin_dir.name
        cand_sha, why = gate.sha_of_bin(bin_dir)
        if args.engine_sha:
            cand_sha = gate.require_sha(bin_dir, args.engine_sha)
        elif not cand_sha:
            print(f"WARNING: candidate engine sha unknown: {why}", file=sys.stderr)
            cand_sha = "unknown"
        _, secs, src = gate.run_capture(set_name, shots, digest, bin_dir, cand, args.parallel, "feature")
    rows = gate.compare_dirs(gdir, cand, [s.name for s in shots], out / "maps")
    for r in rows:
        if "status" in r:
            continue
        if not r["camera"]["ok"]:
            r["status"] = "harness"
        else:
            r["status"] = "pass" if r["flip_mean"] <= threshold else "fail"
    bad = [r for r in rows if r["status"] != "pass"]
    report = {
        "schema": gate.SCHEMA, "set": set_name, "created": gate.now(), "golden": gate.rel(gdir),
        "golden_engine_sha": gman["engine_sha"], "golden_goal_src": gman.get("goal_src"),
        "candidate_bin": bin_name, "candidate_engine_sha": cand_sha, "candidate_goal_src": src,
        "calibration": cal_key, "calibration_exact": cal["engine_sha"] == gman["engine_sha"],
        "F0": cal["F0"], "m": cal["m"], "threshold": threshold, "capture_seconds": secs,
        "passed": not bad, "summary": gate.summarize(rows), "shots": rows}
    _write(out / "report.json", report)
    lines = [f"# Parity check: {set_name}", "",
             f"- Result: **{'PASS' if not bad else 'FAIL'}** ({len(rows) - len(bad)}/{len(rows)} shots pass)",
             f"- Golden: `{gate.rel(gdir)}` (engine {gman['engine_sha']}); candidate: `{bin_name}`"
             f" (engine {cand_sha}, checkout {(src or {}).get('checkout_sha')}"
             f"{', goal_src dirty' if (src or {}).get('goal_src_dirty') else ''})",
             f"- Threshold: FLIP mean <= F0 + m = {_fmt(cal['F0'])} + {_fmt(cal['m'])} = {_fmt(threshold)}"
             f" (`{cal_key}`)", "",
             "| shot | status | FLIP mean | FLIP max | px differ | SSIM | PSNR |", "|---|---|---|---|---|---|---|"]
    for r in rows:
        pc = r.get("precheck") or {}
        ssim = f"{pc['ssim']:.4f}" if pc.get("ssim") is not None else "-"
        psnr = f"{pc['psnr']:.1f}" if pc.get("psnr") is not None else "-"
        note = r.get("camera", {}).get("reason") or pc.get("reason") or ""
        lines.append(f"| {r['shot']} | {r['status']}{' (' + note + ')' if note else ''} | "
                     f"{_fmt(r['flip_mean']) if 'flip_mean' in r else '-'} | "
                     f"{r['flip_max']:.4f} | {r['px_differ']} | {ssim} | {psnr} |" if "flip_mean" in r else
                     f"| {r['shot']} | {r['status']}{' (' + note + ')' if note else ''} | - | - | - | {ssim} | {psnr} |")
    (out / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines[:5]))
    for r in bad:
        print(f"  {r['status'].upper()} {r['shot']} {_fmt(r['flip_mean']) if 'flip_mean' in r else ''}")
    print(f"report: {gate.rel(out / 'report.json')}  summary: {gate.rel(out / 'summary.md')}")
    return 0 if not bad else 1


def cmd_determinism(args) -> int:
    set_name, shots, digest = gate.load_set(args.set)
    bin_dir = gate.resolve_build(args.build, pin=False)
    out = data_dir() / "parity" / "runs" / f"{gate.stamp()}-determinism-{set_name}"
    dirs, secs, srcs = _two_runs(args, set_name, shots, digest, bin_dir, out, args.runs)
    names = [s.name for s in shots]
    pairs = {}
    for i in range(len(dirs)):
        for j in range(i + 1, len(dirs)):
            rows = gate.compare_dirs(dirs[i], dirs[j], names, out / "maps" / f"{i}-{j}", precheck=False)
            pairs[f"{i}-{j}"] = {"summary": gate.summarize(rows), "shots": rows}
    _write(out / "report.json", {"schema": gate.SCHEMA, "set": set_name, "bin": bin_dir.name,
                                 "engine_sha": gate.sha_of_bin(bin_dir)[0] or "unknown", "goal_src": srcs,
                                 "created": gate.now(), "capture_seconds": secs, "pairs": pairs})
    for k, p in pairs.items():
        s = p["summary"]
        cams = sum(1 for r in p["shots"] if r.get("camera", {}).get("bit_equal"))
        print(f"runs {k}: {s['shots']} shots, mean FLIP {s['mean_of_means']:.5f}, worst {s['worst']:.5f} "
              f"({s['worst_shot']}), max px FLIP {s['worst_max']:.3f}, {s['zero_px_shots']} shots pixel-exact, "
              f"{cams} cameras bit-equal")
    print(f"report: {gate.rel(out / 'report.json')}")
    return 0


def _git(*a: str) -> str:
    return subprocess.run(["git", "-C", str(root()), *a], capture_output=True, text=True, check=True).stdout.strip()


def cmd_watch(args) -> int:
    state_path = data_dir() / "parity" / "watch.json"
    state = json.loads(state_path.read_text(encoding="utf-8")) if state_path.exists() else {}
    while True:
        try:
            _git("fetch", "-q", "origin", "master")
            head = _git("rev-parse", "--short", "origin/master")
        except subprocess.CalledProcessError as exc:
            print(f"fetch failed: {exc.stderr.strip()}", file=sys.stderr)
            head = None
        latest = engine.latest_staged()
        if head and head != state.get("last_sha") and latest is not None:
            out = data_dir() / "ci" / f"{gate.stamp()}-{head}"
            print(f"origin/master {head}: parity check of {latest.name} -> {gate.rel(out)}", flush=True)
            ns = argparse.Namespace(set=args.set, golden=None, build=str(latest), captures=None,
                                    out=str(out), parallel=args.parallel, engine_sha=None)
            try:
                rc = cmd_check(ns)
            except SystemExit as exc:
                print(exc, file=sys.stderr)
                rc = 2
            state = {"last_sha": head, "bin": latest.name, "rc": rc, "run": gate.rel(out), "at": gate.now()}
            _write(state_path, state)
        if args.once:
            return state.get("rc", 0) or 0
        time.sleep(args.interval)


def main() -> int:
    ap = argparse.ArgumentParser(prog="sandover-parity", description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p, set_default: str, build: bool = True):
        p.add_argument("--set", default=set_default, help=f"shot set name or manifest path (default {set_default})")
        p.add_argument("--parallel", type=int, default=4, help="game instances (default 4)")
        if build:
            p.add_argument("--build", default="current", help="current, a directory, or a staged copy's name")

    c = sub.add_parser("calibrate", help="noise floor F0 and margin m for a shot set")
    common(c, "auto-v1")
    c.add_argument("--engine-sha", help="engine sha of the build when its staged build.json has none")
    c.add_argument("--runs", nargs=2, metavar="DIR", help="use two existing capture dirs instead of capturing")
    c.set_defaults(fn=cmd_calibrate)
    g = sub.add_parser("golden", help="capture a golden generation from a (vanilla) build")
    common(g, "auto-v1")
    g.add_argument("--engine-sha", help="engine sha of the build when its staged build.json has none")
    g.add_argument("--reason", required=True, help="why this generation exists (refresh policy)")
    g.add_argument("--replace", action="store_true", help="move an existing generation of this sha aside")
    g.set_defaults(fn=cmd_golden)
    k = sub.add_parser("check", help="capture with a candidate build and gate it against the golden")
    common(k, "auto-v1")
    k.add_argument("--golden", help="golden generation (engine sha); default newest")
    k.add_argument("--captures", help="compare an existing capture dir instead of capturing")
    k.add_argument("--engine-sha", help="candidate engine sha when its staged build.json has none")
    k.add_argument("--out", help="run directory (default $SANDOVER_DATA/parity/runs/<stamp>-check-<set>)")
    k.set_defaults(fn=cmd_check)
    d = sub.add_parser("determinism", help="repeat runs of one build; FLIP per shot pair")
    common(d, "auto-v0")
    d.add_argument("--runs", type=int, default=2)
    d.set_defaults(fn=cmd_determinism)
    w = sub.add_parser("watch", help="poll origin/master; check the latest staged build on each new commit")
    common(w, "auto-v0", build=False)
    w.add_argument("--interval", type=int, default=300, help="seconds between polls")
    w.add_argument("--once", action="store_true")
    w.set_defaults(fn=cmd_watch)
    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
