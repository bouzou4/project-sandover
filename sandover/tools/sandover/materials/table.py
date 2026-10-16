"""materials.csv: merge the two judge passes, FiftyOne glue, spot-check and corrections."""

from __future__ import annotations

import json
import random
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path

from sandover.materials import CLASSES, TAGS
from sandover.materials import judge as judge_mod
from sandover.materials.judge import PASSES, batches, load_pass
from sandover.materials.sheets import (MANIFEST_FIELDS, PER_SHEET, hex_id, manifest_path,
                                       materials_dir, read_csv, render_all, write_csv)

FIELDS = ["combo_id", "tpage", "name", "class", "confidence", "judge_agreement", "source",
          "envmap_swatch", "has_alpha", "levels", "reviewed", "tags", "note", "parallax"]
REVIEW_BELOW = 0.6
AMBIGUOUS_BELOW = 0.5
BANDS = ((0.0, 0.6), (0.6, 0.85), (0.85, 1.01))
SPOT_FIELDS = ["slot", "index", "combo_id", "tpage", "name", "judged_class", "confidence",
               "judge_agreement", "human_class", "tags", "human_note"]
SEED = 5


def csv_path() -> Path:
    return materials_dir() / "materials.csv"


def _levels_map() -> dict[tuple[str, str], str]:
    """(tpage, name) -> "lvl1;lvl2" from texture-levels.json, "" if none/not generated."""
    from sandover.census import load_texture_levels, texture_levels_path
    levels = load_texture_levels(texture_levels_path())
    return {k: ";".join(v) for k, v in levels.items()}


def _fmt(j: dict | None) -> str:
    return "-" if j is None else f"{j['class']} {j['confidence']:.2f} ({j['note']})"


def combine(ja: dict | None, jb: dict | None) -> tuple[str, float, str, str]:
    """(class, confidence, judge_agreement, note) from the two passes.

    Agreement averages the confidences; a disagreement keeps the more confident class at
    half its confidence, so it always lands below the ambiguous threshold.
    """
    if ja and jb:
        best = max((ja, jb), key=lambda j: j["confidence"])
        if ja["class"] == jb["class"]:
            return ja["class"], (ja["confidence"] + jb["confidence"]) / 2, "agree", best["note"]
        return best["class"], min(best["confidence"] / 2, AMBIGUOUS_BELOW - 0.01), "disagree", f"a: {_fmt(ja)} | b: {_fmt(jb)}"
    one = ja or jb
    if one:
        return one["class"], min(one["confidence"], AMBIGUOUS_BELOW - 0.01), "single", one["note"]
    return "special", 0.0, "none", "not judged"


def merge() -> None:
    """Write materials.csv from both passes; rows a human already reviewed are kept."""
    manifest = read_csv(manifest_path())
    ja, jb = load_pass("a"), load_pass("b")
    levels = _levels_map()
    kept, parallax = {}, {}
    if csv_path().exists():
        old = read_csv(csv_path())
        kept = {r["combo_id"]: r for r in old if r["reviewed"] == "True"}
        parallax = {r["combo_id"]: r.get("parallax", "") for r in old}
    rows = []
    for m in manifest:
        i = int(m["index"])
        cls, conf, agree, note = combine(ja.get(i), jb.get(i))
        row = {"combo_id": m["combo_id"], "tpage": m["tpage"], "name": m["name"],
               "class": cls, "confidence": f"{conf:.3f}", "judge_agreement": agree,
               "source": "judge" if agree != "none" else "default",
               "envmap_swatch": m["envmap_swatch"], "has_alpha": m["has_alpha"],
               "levels": levels.get((m["tpage"], m["name"]), ""), "reviewed": False,
               "tags": "", "note": note, "parallax": parallax.get(m["combo_id"], "")}
        if m["combo_id"] in kept:
            h = kept[m["combo_id"]]
            # .get: an older materials.csv (pre-tags) is missing the new column.
            row.update({k: h.get(k, "") for k in ("class", "confidence", "source", "reviewed", "tags", "note")})
        rows.append(row)
    write_csv(csv_path(), rows, FIELDS)
    print(f"wrote {csv_path()} ({len(rows)} rows, {len(kept)} human rows kept)")


def refresh_levels() -> None:
    """Update the levels column of an existing materials.csv, without re-judging anything."""
    levels = _levels_map()
    rows = read_csv(csv_path())
    n = sum(1 for r in rows if (r["tpage"], r["name"]) in levels)
    for r in rows:
        r["levels"] = levels.get((r["tpage"], r["name"]), r.get("levels", ""))
    write_csv(csv_path(), rows, FIELDS)
    print(f"refreshed levels for {n}/{len(rows)} rows")


def print_status() -> None:
    n = len(read_csv(manifest_path()))
    n_sheets = -(-n // PER_SHEET)
    for p in PASSES:
        got = load_pass(p)
        missing = []
        for b, sheets in enumerate(batches(p, n_sheets)):
            want = {i for s in sheets for i in range((s - 1) * PER_SHEET, min(s * PER_SHEET, n))}
            if want - got.keys():
                missing.append(f"{b:02d}({len(want - got.keys())})")
        print(f"pass {p}: {len(got)}/{n} judged; batches with gaps: {' '.join(missing) or 'none'}")


def _dataset():
    from sandover.census import DATASET_NAME, _configure_fiftyone_env
    _configure_fiftyone_env()
    import fiftyone as fo
    return fo, fo.load_dataset(DATASET_NAME)


def load_fiftyone() -> None:
    """Push class, confidence and agreement into the dataset; save the needs-review view."""
    fo, ds = _dataset()
    from fiftyone import ViewField as F

    by_key = {(r["tpage"], r["name"]): r for r in read_csv(csv_path())}
    idx = {(m["tpage"], m["name"]): int(m["index"]) for m in read_csv(manifest_path())}
    ja, jb = load_pass("a"), load_pass("b")
    keys = list(zip(*ds.values(["tpage_name", "name"])))
    rows = [by_key[k] for k in keys]
    ds.set_values("material", [fo.Classification(label=r["class"],
                                                 confidence=float(r["confidence"]))
                               for r in rows])
    ds.set_values("material_class", [r["class"] for r in rows])
    ds.set_values("material_confidence", [float(r["confidence"]) for r in rows])
    ds.set_values("judge_agreement", [r["judge_agreement"] for r in rows])
    ds.set_values("judge_a", [ja.get(idx[k], {}).get("class") for k in keys])
    ds.set_values("judge_b", [jb.get(idx[k], {}).get("class") for k in keys])
    ds.set_values("envmap_swatch", [r["envmap_swatch"] == "True" for r in rows])
    ds.set_values("material_reviewed", [r["reviewed"] == "True" for r in rows])
    # Named "material_tags" (not "tags"): FiftyOne's own sample.tags already carries
    # apply_corrections' class:<name>/ok convention.
    ds.set_values("material_tags", [[t for t in r.get("tags", "").split(";") if t] for r in rows])
    ds.set_values("material_note", [r["note"] for r in rows])
    ds.set_values("sheet_index", [idx[k] for k in keys])
    view = ds.match((F("material_reviewed") == False) & (  # noqa: E712
        (F("judge_agreement") != "agree") | (F("material_confidence") < REVIEW_BELOW)
    )).sort_by("material_confidence")
    ds.save_view("needs-review", view, overwrite=True)
    print(f"loaded {len(rows)} labels; needs-review: {len(view)} samples")


def spotcheck(n: int = 100) -> None:
    """Stratified sample (class x confidence band) with sheets and an annotatable CSV."""
    rows = read_csv(csv_path())
    manifest = read_csv(manifest_path())
    rng = random.Random(SEED)
    pools: dict[str, list[list[int]]] = {c: [[] for _ in BANDS] for c in CLASSES}
    for i, r in enumerate(rows):
        if r["reviewed"] == "True":
            continue
        c = float(r["confidence"])
        band = next(b for b, (lo, hi) in enumerate(BANDS) if lo <= c < hi)
        pools[r["class"]][band].append(i)
    for bands in pools.values():
        for p in bands:
            rng.shuffle(p)
    # Round-robin over classes and bands so every stratum is drawn before any repeats.
    picked: list[int] = []
    band_turn = 0
    while len(picked) < n and any(p for b in pools.values() for p in b):
        for cls in CLASSES:
            for k in range(len(BANDS)):
                band = pools[cls][(band_turn + k) % len(BANDS)]
                if band and len(picked) < n:
                    picked.append(band.pop())
                    break
        band_turn += 1
    picked.sort(key=lambda i: (rows[i]["class"], float(rows[i]["confidence"])))
    out_dir = materials_dir() / "spotcheck"
    spot, cells = [], []
    for slot, i in enumerate(picked):
        r = rows[i]
        spot.append({"slot": slot, "index": i, "combo_id": r["combo_id"], "tpage": r["tpage"],
                     "name": r["name"], "judged_class": r["class"],
                     "confidence": r["confidence"], "judge_agreement": r["judge_agreement"],
                     "human_class": "", "tags": "", "human_note": ""})
        cells.append({**manifest[i], "label": f"{r['class']} {float(r['confidence']):.2f}"})
    write_csv(out_dir / "spotcheck.csv", spot, SPOT_FIELDS)
    render_all(cells, out_dir, prefix="spot-check")
    print(f"wrote {len(spot)} rows and {-(-len(spot) // PER_SHEET)} sheets under {out_dir}")


def _set_human(row: dict, cls: str, note: str, tags: str = "") -> None:
    before = row["class"]
    row.update({"class": cls, "confidence": "1.000", "source": "human", "reviewed": True})
    if tags:
        row["tags"] = tags
    tail = f"human: {before} -> {cls}" if before != cls else "human: confirmed"
    row["note"] = "; ".join(x for x in (tail, note) if x)


def apply_corrections(path: Path | None, from_fiftyone: bool = False) -> None:
    """Human labels win: from a CSV (combo_id, human_class[, tags, human_note]) or class:<name> tags."""
    rows = read_csv(csv_path())
    by_id = {r["combo_id"]: r for r in rows}
    fixes: list[tuple[str, str, str, str]] = []
    if path:
        fixes = [(r["combo_id"], r["human_class"].strip().lower(), r.get("human_note", ""),
                  r.get("tags", "").strip())
                 for r in read_csv(path) if r.get("human_class", "").strip()]
    if from_fiftyone:
        fo, ds = _dataset()
        for combo, sample_tags, cls in zip(*ds.values(["combo_id", "tags", "material_class"])):
            for t in sample_tags:
                if t.startswith("class:"):
                    fixes.append((hex_id(combo), t.split(":", 1)[1].strip().lower(), "", ""))
                elif t == "ok":
                    fixes.append((hex_id(combo), cls, "", ""))
    good = [f for f in fixes if f[1] in CLASSES and f[0] in by_id]
    for combo, cls, note, tags in good:
        _set_human(by_id[combo], cls, note, tags)
    write_csv(csv_path(), rows, FIELDS)
    bad = [f for f in fixes if f not in good]
    print(f"applied {len(good)} corrections; skipped {len(bad)}: {bad[:5]}")


# ---- rejudge: re-run the vision judge on the needs-review set, with human notes as guidance ---

def rejudge_candidates(limit: int | None = None) -> list[dict]:
    """Needs-review rows not yet human-classified, least confident first, disagreements first."""
    rows = read_csv(csv_path())
    manifest = {(m["tpage"], m["name"]): m for m in read_csv(manifest_path())}
    cands = [r for r in rows if r["reviewed"] != "True" and (
        r["judge_agreement"] != "agree" or float(r["confidence"]) < REVIEW_BELOW)]
    cands.sort(key=lambda r: (float(r["confidence"]), r["judge_agreement"] != "disagree"))
    if limit:
        cands = cands[:limit]
    return [{**manifest[(r["tpage"], r["name"])], "class": r["class"], "confidence": r["confidence"]}
            for r in cands if (r["tpage"], r["name"]) in manifest]


def guidance_lines(rows: list[dict]) -> list[str]:
    """One line per human note, keyed by texture so the judge can place it in context."""
    return [f"{r['tpage']}/{r['name']}: {r['human_note'].strip()}"
            for r in rows if r.get("human_note", "").strip()]


def guidance_block(rows: list[dict]) -> str:
    lines = guidance_lines(rows)
    if not lines:
        return ""
    return "Human guidance from the spot-check:\n" + "\n".join(f"- {line}" for line in lines)


def latest_rejudge_dir() -> Path | None:
    d = materials_dir() / "judgements" / "rejudge"
    if not d.is_dir():
        return None
    labels = sorted(p.name for p in d.iterdir() if p.is_dir())
    return d / labels[-1] if labels else None


def apply_rejudge(out_dir: Path) -> dict[str, tuple[str, str]]:
    """Merge ``out_dir/judgement.json`` into materials.csv; return {combo_id: (old, new)} changes.

    Skips rows a human already reviewed (excluded from the candidate set in the first place,
    but re-checked here in case a correction landed between preparing and judging).
    """
    manifest = {int(r["index"]): r for r in read_csv(out_dir / "manifest.csv")}
    judged = json.loads((out_dir / "judgement.json").read_text(encoding="utf-8"))
    rows = read_csv(csv_path())
    by_id = {r["combo_id"]: r for r in rows}
    changed: dict[str, tuple[str, str]] = {}
    for j in judged:
        cls = str(j.get("class", "")).strip().lower()
        if cls not in CLASSES:
            continue
        m = manifest.get(int(j["index"]))
        if not m:
            continue
        row = by_id.get(m["combo_id"])
        if not row or row["reviewed"] == "True":
            continue
        if row["class"] != cls:
            changed[m["combo_id"]] = (row["class"], cls)
        conf = min(max(float(j.get("confidence", 0.0)), 0.0), 1.0)
        row.update({"class": cls, "confidence": f"{conf:.3f}",
                     "note": f"rejudge: {j.get('note', '')}".strip()})
    write_csv(csv_path(), rows, FIELDS)
    (out_dir / "merged.json").write_text(json.dumps({"changed": len(changed)}), encoding="utf-8")
    return changed


def rejudge(with_notes: bool = False, limit: int | None = None) -> str:
    """Two-phase: prepare a batch (manifest+sheets+prompt), or merge a batch already judged.

    Re-run to move from "prepared" to "merged" once the vision judge has written
    ``judgement.json``. See ``kb/howto/materials.md``.
    """
    pending = latest_rejudge_dir()
    if pending and (pending / "judgement.json").is_file() and not (pending / "merged.json").is_file():
        changed = apply_rejudge(pending)
        return f"applied rejudge {pending.name}: {len(changed)} class(es) changed"
    if pending and (pending / "manifest.csv").is_file() and not (pending / "judgement.json").is_file():
        return (f"rejudge {pending.name} awaiting judgement: give "
                f"{pending / 'prompt.txt'} to the vision judge, then run rejudge again")

    rows = rejudge_candidates(limit)
    if not rows:
        return "no needs-review rows to rejudge"
    label = f"{datetime.now(timezone.utc):%Y%m%d-%H%M%S}"
    out_dir = materials_dir() / "judgements" / "rejudge" / label
    cells = [{**r, "index": i, "label": f"{r['class']} {float(r['confidence']):.2f}"}
             for i, r in enumerate(rows)]
    write_csv(out_dir / "manifest.csv", cells, MANIFEST_FIELDS)
    render_all(cells, out_dir, prefix="rejudge")
    guidance = ""
    if with_notes:
        spot_csv = materials_dir() / "spotcheck" / "spotcheck.csv"
        if spot_csv.is_file():
            guidance = guidance_block(read_csv(spot_csv))
    (out_dir / "prompt.txt").write_text(
        judge_mod.rejudge_prompt(out_dir, len(rows), guidance), encoding="utf-8")
    return f"prepared {len(rows)} row(s) under {out_dir}; give prompt.txt to the vision judge, then run rejudge again"


def summary() -> str:
    rows = read_csv(csv_path())
    manifest = read_csv(manifest_path())
    ja, jb = load_pass("a"), load_pass("b")
    n = len(rows)
    dist = Counter(r["class"] for r in rows)
    agree = Counter(r["judge_agreement"] for r in rows)
    both = agree["agree"] + agree["disagree"]
    ambiguous = sum(1 for r in rows if float(r["confidence"]) < AMBIGUOUS_BELOW)
    review = sum(1 for r in rows if r["reviewed"] != "True" and (
        r["judge_agreement"] != "agree" or float(r["confidence"]) < REVIEW_BELOW))
    conf = Counter()
    for m in manifest:
        i = int(m["index"])
        if i in ja and i in jb and ja[i]["class"] != jb[i]["class"]:
            conf[tuple(sorted((ja[i]["class"], jb[i]["class"])))] += 1
    lines = ["| class | textures | share | mean confidence |", "|---|---|---|---|"]
    for c in CLASSES:
        cs = [float(r["confidence"]) for r in rows if r["class"] == c]
        mean = sum(cs) / len(cs) if cs else 0.0
        lines.append(f"| {c} | {dist[c]} | {100 * dist[c] / n:.1f}% | {mean:.2f} |")
    lines += ["", f"Judged twice: {both}/{n}; agreement {agree['agree']}/{both} "
              f"({100 * agree['agree'] / max(both, 1):.1f}%); single: {agree['single']}; "
              f"none: {agree['none']}.",
              f"Ambiguous (confidence < {AMBIGUOUS_BELOW}): {ambiguous}. "
              f"Needs review (disagreement or confidence < {REVIEW_BELOW}): {review}.",
              "", "| confusion (a vs b) | textures |", "|---|---|"]
    lines += [f"| {a} / {b} | {k} |" for (a, b), k in conf.most_common(8)]
    return "\n".join(lines) + "\n"
