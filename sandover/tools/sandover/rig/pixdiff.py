"""Per-shot pixel difference of two capture directories: the "no visible change" check.

For every ``<name>.png`` in A that B also has: the largest channel difference (0-255), how many
pixels differ at all and by more than the threshold (count and share); with both EXRs present
also the largest absolute and relative difference of the HDR buffer. Exit 1 when a PNG differs by
more than the threshold (default 2).

    python -m sandover.rig.pixdiff runA/ runB/ [--threshold 2] [--json out.json]
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

from sandover.hdr import read_exr


def png_diff(a: Path, b: Path, threshold: int) -> dict:
    x = np.asarray(Image.open(a).convert("RGB"), dtype=np.int16)
    y = np.asarray(Image.open(b).convert("RGB"), dtype=np.int16)
    if x.shape != y.shape:
        return {"error": f"size {x.shape[1]}x{x.shape[0]} vs {y.shape[1]}x{y.shape[0]}"}
    d = np.abs(x - y).max(axis=2)
    over = int((d > threshold).sum())
    return {"max": int(d.max()), "px_diff": int((d > 0).sum()), "px_over": over,
            "share_over": over / d.size, "px": int(d.size)}


def exr_diff(a: Path, b: Path) -> dict:
    x, _ = read_exr(a)
    y, _ = read_exr(b)
    if x.shape != y.shape:
        return {"error": "size"}
    d = np.abs(x - y)
    rel = d / np.maximum(np.maximum(np.abs(x), np.abs(y)), 1e-3)
    return {"max_abs": float(np.nanmax(d)), "max_rel": float(np.nanmax(rel))}


def compare(a_dir: Path, b_dir: Path, threshold: int = 2) -> list[dict]:
    rows = []
    for a in sorted(a_dir.glob("*.png")):
        b = b_dir / a.name
        if not b.exists():
            continue
        row = {"name": a.stem, **png_diff(a, b, threshold)}
        ea, eb = a.with_suffix(".exr"), b.with_suffix(".exr")
        if ea.exists() and eb.exists():
            row["exr"] = exr_diff(ea, eb)
        rows.append(row)
    return rows


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="pixdiff", description=__doc__.splitlines()[0])
    ap.add_argument("a", type=Path)
    ap.add_argument("b", type=Path)
    ap.add_argument("--threshold", type=int, default=2, help="largest allowed PNG channel difference")
    ap.add_argument("--json", type=Path, help="also write the rows as JSON here")
    args = ap.parse_args(argv)
    rows = compare(args.a, args.b, args.threshold)
    if args.json:
        doc = {"threshold": args.threshold, "shots": rows}
        args.json.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    if not rows:
        print("no PNG in both directories", file=sys.stderr)
        return 2
    bad = 0
    for r in rows:
        if "error" in r:
            print(f"{r['name']:24s} {r['error']}")
            bad += 1
            continue
        e = r.get("exr")
        hdr = f"  exr max abs {e['max_abs']:.3g} rel {e['max_rel']:.3g}" if e and "error" not in e else ""
        ok = r["max"] <= args.threshold
        bad += not ok
        print(f"{r['name']:24s} {'ok  ' if ok else 'FAIL'} max {r['max']:3d}  differing px "
              f"{r['px_diff']} ({100 * r['px_diff'] / r['px']:.3f}%), over {args.threshold}: {r['px_over']} "
              f"({100 * r['share_over']:.4f}%){hdr}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
