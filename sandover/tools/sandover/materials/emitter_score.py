"""``sandover-materials emitter-score``: separate true emitters from lit receivers (issue #161).

Bake-derived E marks the lamp and also the wall the lamp lights, so most emissive maps cover
their whole texture. This scores every texture of ``emissive-maps/index.json`` with a
probability of being a real emitter, from six components:

- ``correlation`` (a): Pearson correlation, over opaque texels, between the texture's E map and
  its albedo luma. Emitters glow where they are bright; a lit receiver has even E over any texel.
  0 when either is flat.
- ``coverage`` (b): fraction of texels with E >= ``covered`` (the maps index value).
- ``gradient`` (c): mean over the lit triangles drawing the texture (any corner E >= ``covered``)
  of ``(max - min) / max`` of its corner E. Light falls off across a receiver; an emitter panel
  is lit evenly.
- ``consistency`` (d): fraction of the texture's triangles, over every level, that are lit.
  A lamp is lit wherever it is drawn; a wall texture only near the lamps.
- ``slot`` (e): prior of the dominant E source (the maps index ``role``: effect slot roles,
  ``night``, ``static``; ``luminance`` for luminance-sourced maps).
- ``brightness`` (f): E-weighted mean albedo luma.

``score = sigmoid(bias + sum(weight * component))``, weights, priors and the threshold in
``sandover/presets/emissive.toml`` ``[emitter_score]``. (c) and (d) come from a scan of the
level glbs and sidecars cached in ``emitter-score/draws.json`` (``--rescan`` to redo it).

Output ``$SANDOVER_DATA/materials/emitter-score.json``: ``{"params": ..., "threshold": t,
"textures": {name: {"score", "emitter", "components": {...}, "tpage", "role", "class",
"human", "anchor", "ladder_default"}}}``, keyed by texture name like the emissive table. ``human`` is
the reviewer's verdict where one exists (``emitter``/``receiver``, from an override or a human
ladder strength) and ``anchor`` the ladder anchor role (torch, lamp glass...). Neither changes
``score``; an anchor or a human ``emitter`` makes ``emitter`` true
(:func:`sandover.materials.emissive_strengths.is_emitter`). ``ladder_default`` is the queue
default: the bake default for emitters, ``receiver_default`` for receivers. See
:func:`sandover.materials.emissive_strengths.scored_queue` for the queue helper.

``--validate`` prints precision/recall against the maintainer verdicts and a name-based weak label
set, the threshold that keeps every human-confirmed emitter and the material-class breakdown;
``--sheets`` writes the top-24 emitter/receiver contact sheets under ``emitter-score/sheets/``.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
import pygltflib
from PIL import Image, ImageDraw

from sandover import paths
from sandover.materials.emissive_maps import (
    index_texture_dir,
    load_index,
    maps_dir,
    override_names,
    texture_luma,
)
from sandover.materials.emissive_strengths import LADDER_MIN, is_emitter, latest_strengths, read_strengths
from sandover.materials.emissive_table import _material_texture_name
from sandover.materials.overrides import overrides_path, read_overrides
from sandover.materials.sheets import materials_dir, read_csv, textures_dir
from sandover.unbake import sidecar
from sandover.unbake.glb import SKIP_TREES, _read
from sandover.unbake.levels import LEVELS

COMPONENTS = ("correlation", "coverage", "gradient", "consistency", "slot", "brightness")
EMITTER_NAME = re.compile(r"torch|flame|fire(?!place)|lamp(?!-wood)|lantern|light(?!ning)|lite|bulb|glow|"
                          r"magma|crystal|eco-valve-inside|blueeco|screen|neon|candle|ember|coal|glass|"
                          r"^lava-(flow|floor-deadly|ground-hot|green)|vil3-lava", re.I)
RECEIVER_NAME = re.compile(r"rock|stone|wall|wood|log|plank|ground|dirt|grass|sand|cliff|brick|roof|"
                           r"floor|bark|tree|trunk|path|bridge|dock|mud|pole|post|beam|board|grate|"
                           r"rim|hinge|pipe|plate|metal", re.I)


def preset_path() -> Path:
    return paths.root() / "sandover" / "presets" / "emissive.toml"


def score_path() -> Path:
    return materials_dir() / "emitter-score.json"


def work_dir() -> Path:
    return materials_dir() / "emitter-score"


# ---- parameters -------------------------------------------------------------------------

@dataclass
class Params:
    bias: float = -1.0
    covered: float = 0.1
    alpha_min: float = 0.05
    threshold: float = 0.5
    weights: dict[str, float] = field(default_factory=dict)
    slot_prior: dict[str, float] = field(default_factory=dict)
    receiver_default: float = LADDER_MIN
    receiver_scale: float = 0.0


def load_params(path: Path | None = None) -> Params:
    """``[emitter_score]`` of ``emissive.toml``; missing weights count as 0."""
    raw = tomllib.loads((path or preset_path()).read_text(encoding="utf-8")).get("emitter_score", {})
    w = {k: float(raw.get("weights", {}).get(k, 0.0)) for k in COMPONENTS}
    return Params(bias=float(raw.get("bias", -1.0)), covered=float(raw.get("covered", 0.1)),
                  alpha_min=float(raw.get("alpha_min", 0.05)), threshold=float(raw.get("threshold", 0.5)),
                  weights=w, slot_prior={k: float(v) for k, v in raw.get("slot_prior", {}).items()},
                  receiver_default=float(raw.get("ladder", {}).get("receiver_default", LADDER_MIN)),
                  receiver_scale=float(raw.get("export", {}).get("receiver_scale", 0.0)))


# ---- (a), (b), (f): texture space ---------------------------------------------------------

def map_components(e_map: np.ndarray, rgba: np.ndarray, covered: float = 0.1,
                   alpha_min: float = 0.05) -> dict[str, float]:
    """correlation, coverage and brightness of one E map (H, W, 0..1) against its albedo
    (H, W, 4, 0..1). The map is resampled (nearest) to the albedo size when they differ."""
    h, w = rgba.shape[:2]
    if e_map.shape != (h, w):
        ys = (np.arange(h) * e_map.shape[0] // h)[:, None]
        xs = (np.arange(w) * e_map.shape[1] // w)[None, :]
        e_map = e_map[ys, xs]
    luma = rgba[..., :3] @ np.array([0.299, 0.587, 0.114])
    opaque = rgba[..., 3] > alpha_min
    if not opaque.any():
        opaque = np.ones((h, w), bool)
    e, l = e_map[opaque].astype(np.float64), luma[opaque]
    corr = 0.0
    if e.std() > 1e-3 and l.std() > 1e-3:
        corr = float(np.corrcoef(e, l)[0, 1])
    bright = float((e * l).sum() / e.sum()) if e.sum() > 0 else 0.0
    return {"correlation": round(corr, 4), "coverage": round(float((e_map >= covered).mean()), 4),
            "brightness": round(bright, 4)}


# ---- (c), (d): geometry scan --------------------------------------------------------------

def draw_stats(e_tri: np.ndarray, covered: float = 0.1) -> tuple[int, int, float]:
    """(triangles, lit triangles, summed corner falloff over the lit ones) for (T, 3) corner E."""
    if not len(e_tri):
        return 0, 0, 0.0
    mx = e_tri.max(1)
    lit = mx >= covered
    fall = (mx[lit] - e_tri[lit].min(1)) / mx[lit]
    return int(len(e_tri)), int(lit.sum()), float(fall.sum())


def scan_level(level: str, levels_dir: Path, lighting_dir: Path, acc: dict[str, dict],
               covered: float = 0.1) -> None:
    """Add this level's per-texture triangle counts to ``acc`` (name -> {"levels": {level:
    [tris, lit]}, "falloff": sum}); tfrag/tie only, like the emissive table."""
    glb_path = levels_dir / level / f"{level}-background.glb"
    sc_path = lighting_dir / f"{level}.unbake"
    if not glb_path.is_file() or not sc_path.is_file():
        return
    g = pygltflib.GLTF2().load(str(glb_path))
    blob = g.binary_blob()
    tree_rec = {(t["kind"], t["fr3_tree"]): t["records"] for t in sidecar.read(sc_path)["trees"]}
    n_tfrag = 0
    for root_idx in g.scenes[g.scene or 0].nodes:
        root = g.nodes[root_idx]
        name = root.name or ""
        if any(s in name for s in SKIP_TREES):
            continue
        if "-tfrag-" in name:
            key = (0, n_tfrag)
            n_tfrag += 1
        elif "-tie-" in name:
            key = (1, int(name.rsplit("-", 1)[1]))
        else:
            continue
        rec = tree_rec.get(key)
        if rec is None:
            continue
        e_all = ((rec[:, 3] >> 16) & 0xFF) / 255.0
        for c in root.children:
            node = g.nodes[c]
            if node.mesh is None:
                continue
            for p in g.meshes[node.mesh].primitives:
                tex = _material_texture_name(g, p.material)
                if p.indices is None or not tex:
                    continue
                idx = _read(g, blob, p.indices).astype(np.int64).ravel()
                idx = idx[: len(idx) // 3 * 3].reshape(-1, 3)
                idx = idx[(idx < len(e_all)).all(1)]
                n, lit, fall = draw_stats(e_all[idx], covered)
                a = acc.setdefault(tex, {"levels": {}, "falloff": 0.0})
                lv = a["levels"].setdefault(level, [0, 0])
                lv[0] += n
                lv[1] += lit
                a["falloff"] += fall


def scan(levels_dir: Path, lighting_dir: Path, levels: list[str] | None = None, covered: float = 0.1,
         log=print) -> dict[str, dict]:
    acc: dict[str, dict] = {}
    for lvl in levels or list(LEVELS):
        scan_level(lvl, levels_dir, lighting_dir, acc, covered)
        log(f"{lvl}: {len(acc)} textures so far")
    return acc


def geometry_components(d: dict | None) -> dict[str, float]:
    """gradient and consistency from one texture's scan record; neutral 0.5 when unseen."""
    if not d:
        return {"gradient": 0.5, "consistency": 0.5}
    tris = sum(v[0] for v in d["levels"].values())
    lit = sum(v[1] for v in d["levels"].values())
    return {"gradient": round(d["falloff"] / lit, 4) if lit else 0.5,
            "consistency": round(lit / tris, 4) if tris else 0.5}


# ---- score ---------------------------------------------------------------------------------

def slot_component(rec: dict, params: Params) -> float:
    key = "luminance" if rec.get("source") == "luminance" else rec.get("role", "")
    return params.slot_prior.get(key, 0.5)


def combine(comp: dict[str, float], params: Params) -> float:
    """sigmoid(bias + sum(weight * component))."""
    z = params.bias + sum(params.weights.get(k, 0.0) * float(comp.get(k, 0.0)) for k in COMPONENTS)
    return 1.0 / (1.0 + math.exp(-z))


def human_labels(verdicts: dict[str, str], latest: dict[str, dict]) -> dict[str, str]:
    """Texture name -> ``emitter``/``receiver`` from human overrides and human ladder strengths
    (a strength within two quarter-octaves of the rail bottom counts as a receiver)."""
    out: dict[str, str] = {}
    for name, r in latest.items():
        if r.get("source") != "human":
            continue
        try:
            s = float(r.get("strength", ""))
        except ValueError:
            continue
        out[name] = "receiver" if s <= LADDER_MIN * 2 ** 0.5 else "emitter"
    for name, v in verdicts.items():
        out[name] = "emitter" if v == "emissive" else "receiver"
    return out


def weak_label(name: str) -> str | None:
    """Name-based guess: ``emitter`` (torch, lamp, lava...), ``receiver`` (rock, wood, wall...)
    or None. Only used to report precision/recall, never to score."""
    if EMITTER_NAME.search(name):
        return "emitter"
    if RECEIVER_NAME.search(name):
        return "receiver"
    return None


def material_classes(rows: list[dict]) -> tuple[dict[tuple[str, str], str], dict[str, str]]:
    by_tp = {(r.get("tpage", ""), r.get("name", "")): r.get("class", "") for r in rows}
    by_name: dict[str, str] = {}
    for r in rows:
        by_name.setdefault(r.get("name", ""), r.get("class", ""))
    return by_tp, by_name


def load_map(maps: Path, tpage: str, name: str) -> np.ndarray | None:
    p = maps / tpage / f"{name}.png"
    if not p.is_file():
        return None
    return np.asarray(Image.open(p).convert("L"), np.float64) / 255.0


def load_albedo(tex_root: Path, tex_index: dict[str, list[str]], tpage: str, name: str) -> np.ndarray | None:
    tps = tex_index.get(name, [])
    tp = tpage if tpage in tps else (tps[0] if tps else None)
    if tp is None:
        return None
    return np.asarray(Image.open(tex_root / tp / f"{name}.png").convert("RGBA"), np.float64) / 255.0


def score_all(index: dict, draws: dict[str, dict], params: Params, maps: Path, tex_root: Path,
              human: dict[str, str] | None = None, classes: tuple[dict, dict] | None = None) -> dict[str, dict]:
    """name -> record (see module docstring) for every texture of the maps index."""
    tex_index = index_texture_dir(tex_root)
    human = human or {}
    by_tp, by_name = classes or ({}, {})
    anchors = {a["name"]: role for role, a in index.get("anchors", {}).items()}
    out: dict[str, dict] = {}
    for name, rec in sorted(index.get("textures", {}).items()):
        tpage = rec.get("tpage", "")
        m = load_map(maps, tpage, name)
        rgba = load_albedo(tex_root, tex_index, tpage, name)
        if m is not None and rgba is not None:
            comp = map_components(m, rgba, params.covered, params.alpha_min)
        else:
            comp = {"correlation": 0.0, "coverage": float(rec.get("coverage", 0.0)),
                    "brightness": float(rec.get("luma_p95", 0.0))}
        comp.update(geometry_components(draws.get(name)))
        comp["slot"] = slot_component(rec, params)
        r = {"score": round(combine(comp, params), 4), "anchor": anchors.get(name, ""),
             "human": human.get(name, "")}
        emitter = is_emitter(r, params.threshold)
        out[name] = {
            **r, "emitter": emitter,
            "components": {k: comp[k] for k in COMPONENTS},
            "tpage": tpage, "role": rec.get("role", "") or rec.get("source", ""),
            "class": by_tp.get((tpage, name)) or by_name.get(name, ""),
            "ladder_default": float(rec.get("default_strength", 0.0)) if emitter else params.receiver_default,
        }
    return out


def rethreshold(scores: dict[str, dict], threshold: float, receiver_default: float, index: dict) -> None:
    """Refresh ``emitter`` and ``ladder_default`` in place for a new threshold."""
    tex = index.get("textures", {})
    for name, r in scores.items():
        r["emitter"] = is_emitter(r, threshold)
        r["ladder_default"] = (float(tex.get(name, {}).get("default_strength", 0.0))
                               if r["emitter"] else receiver_default)


# ---- validation ----------------------------------------------------------------------------

def precision_recall(scores: dict[str, dict], labels: dict[str, str], threshold: float) -> dict:
    """Emitter-class precision/recall of ``score >= threshold`` over the labelled textures."""
    tp = fp = fn = tn = 0
    for name, lab in labels.items():
        if name not in scores:
            continue
        pred = scores[name]["score"] >= threshold
        if lab == "emitter":
            tp, fn = tp + pred, fn + (not pred)
        else:
            fp, tn = fp + pred, tn + (not pred)
    return {"n": tp + fp + fn + tn, "tp": tp, "fp": fp, "fn": fn, "tn": tn,
            "precision": round(tp / (tp + fp), 3) if tp + fp else None,
            "recall": round(tp / (tp + fn), 3) if tp + fn else None}


def keep_threshold(scores: dict[str, dict], human: dict[str, str], default: float) -> float:
    """The highest threshold (2 decimals, at most ``default``) that keeps every human-confirmed
    emitter at or above it."""
    kept = [scores[n]["score"] for n, v in human.items() if v == "emitter" and n in scores]
    if not kept:
        return default
    return min(default, math.floor(min(kept) * 100) / 100)


def class_breakdown(scores: dict[str, dict], threshold: float) -> dict[str, list[int]]:
    """class -> [receivers, total]."""
    out: dict[str, list[int]] = {}
    for r in scores.values():
        c = out.setdefault(r["class"] or "?", [0, 0])
        c[0] += not is_emitter(r, threshold)
        c[1] += 1
    return dict(sorted(out.items(), key=lambda kv: -kv[1][1]))


def validation_report(scores: dict[str, dict], human: dict[str, str], threshold: float) -> str:
    weak = {n: w for n in scores if (w := weak_label(n))}
    both = {**weak, **human}
    lines = [f"threshold {threshold:.2f}: {sum(not is_emitter(r, threshold) for r in scores.values())} of "
             f"{len(scores)} textures are receivers (score below, not an anchor or a human emitter)"]
    for label, labs in (("human", human), ("name (weak)", weak), ("human + name", both)):
        pr = precision_recall(scores, labs, threshold)
        lines.append(f"  {label:13s} n={pr['n']:3d} precision {pr['precision']} recall {pr['recall']} "
                     f"(tp {pr['tp']} fp {pr['fp']} fn {pr['fn']} tn {pr['tn']})")
    for n, v in sorted(human.items()):
        if n in scores:
            lines.append(f"  human {v:8s} {n}: score {scores[n]['score']:.3f}")
    lines.append("  class: receivers / total")
    for c, (rcv, tot) in class_breakdown(scores, threshold).items():
        lines.append(f"    {c:10s} {rcv:4d} / {tot}")
    return "\n".join(lines)


# ---- contact sheets ------------------------------------------------------------------------

CELL = 128


def _cell_image(arr: np.ndarray, rgba: bool) -> Image.Image:
    if rgba:
        a = arr.copy()
        a[..., 3] = np.clip(a[..., 3] * 2, 0, 1)  # PS2 alpha 128 = opaque
        im = Image.fromarray(np.clip(np.rint(a * 255), 0, 255).astype(np.uint8), "RGBA")
        bg = Image.new("RGBA", im.size, (40, 40, 40, 255))
        im = Image.alpha_composite(bg, im).convert("RGB")
    else:
        im = Image.fromarray(np.clip(np.rint(arr * 255), 0, 255).astype(np.uint8), "L").convert("RGB")
    return im.resize((CELL, CELL), Image.NEAREST)


def contact_sheet(rows: list[tuple[str, dict]], maps: Path, tex_root: Path, title: str) -> Image.Image:
    """One row per texture: albedo | E map | name, score and components."""
    tex_index = index_texture_dir(tex_root)
    text_w, pad = 360, 6
    cols = 2
    per_col = math.ceil(len(rows) / cols) if rows else 1
    cw = 2 * CELL + text_w + 3 * pad
    sheet = Image.new("RGB", (cols * cw, 28 + per_col * (CELL + pad)), (20, 24, 22))
    d = ImageDraw.Draw(sheet)
    d.text((pad, 8), title, fill=(230, 235, 231))
    for i, (name, r) in enumerate(rows):
        x0 = (i // per_col) * cw + pad
        y0 = 28 + (i % per_col) * (CELL + pad)
        rgba = load_albedo(tex_root, tex_index, r["tpage"], name)
        m = load_map(maps, r["tpage"], name)
        if rgba is not None:
            sheet.paste(_cell_image(rgba, True), (x0, y0))
        if m is not None:
            sheet.paste(_cell_image(m, False), (x0 + CELL + pad, y0))
        c = r["components"]
        txt = [f"{name}", f"{r['tpage']}  [{r['class'] or '?'}] {r['role']}",
               f"score {r['score']:.3f}" + (f"  human: {r['human']}" if r["human"] else ""),
               f"corr {c['correlation']:+.2f}  cover {c['coverage']:.2f}",
               f"grad {c['gradient']:.2f}  consist {c['consistency']:.2f}",
               f"slot {c['slot']:.2f}  bright {c['brightness']:.2f}"]
        for j, t in enumerate(txt):
            d.text((x0 + 2 * CELL + 2 * pad, y0 + 4 + 18 * j), t, fill=(230, 235, 231) if j else (79, 183, 177))
    return sheet


def write_sheets(scores: dict[str, dict], maps: Path, tex_root: Path, out_dir: Path, n: int = 24) -> list[Path]:
    ranked = sorted(scores.items(), key=lambda kv: (-kv[1]["score"], kv[0]))
    out_dir.mkdir(parents=True, exist_ok=True)
    out = []
    for fname, rows, title in (("top-emitters.png", ranked[:n], f"Top {n} emitters by score"),
                               ("top-receivers.png", ranked[::-1][:n], f"Top {n} receivers by score")):
        p = out_dir / fname
        contact_sheet(rows, maps, tex_root, title).save(p)
        out.append(p)
    return out


# ---- CLI -----------------------------------------------------------------------------------

def load_scores(path: Path | None = None) -> dict:
    p = path or score_path()
    if not p.is_file():
        return {"threshold": 0.5, "textures": {}}
    return json.loads(p.read_text(encoding="utf-8"))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="sandover-materials emitter-score", description=__doc__.splitlines()[0])
    data = paths.data_dir()
    ap.add_argument("--levels-dir", type=Path, default=data / "unbake/riproot/decompiler_out/jak1/levels")
    ap.add_argument("--lighting-dir", type=Path, default=sidecar.scan_dir(data / "lighting"),
                    help="default $SANDOVER_DATA/lighting/raw when the export wrote it, else lighting")
    ap.add_argument("--textures", type=Path, default=None, help="default $SANDOVER_ROOT/decompiler_out/jak1/textures")
    ap.add_argument("--preset", type=Path, default=None, help="default sandover/presets/emissive.toml")
    ap.add_argument("--out", type=Path, default=None, help="default $SANDOVER_DATA/materials/emitter-score.json")
    ap.add_argument("--rescan", action="store_true", help="redo the geometry scan (draws.json)")
    ap.add_argument("--validate", action="store_true",
                    help="report precision/recall and use the threshold that keeps every human emitter")
    ap.add_argument("--sheets", action="store_true", help="write the top-24 contact sheets")
    args = ap.parse_args(argv)

    params = load_params(args.preset)
    tex_root = args.textures or textures_dir()
    index = load_index()
    work = work_dir()
    draws_p = work / "draws.json"
    if args.rescan or not draws_p.is_file():
        draws = scan(args.levels_dir, args.lighting_dir, covered=params.covered)
        work.mkdir(parents=True, exist_ok=True)
        draws_p.write_text(json.dumps(draws, sort_keys=True), encoding="utf-8")
    else:
        draws = json.loads(draws_p.read_text(encoding="utf-8"))
    human = human_labels(override_names(read_overrides(overrides_path())), latest_strengths(read_strengths()))
    mats = materials_dir() / "materials.csv"
    classes = material_classes(read_csv(mats) if mats.is_file() else [])
    scores = score_all(index, draws, params, maps_dir(), tex_root, human, classes)
    threshold = params.threshold
    if args.validate:
        threshold = keep_threshold(scores, human, params.threshold)
        rethreshold(scores, threshold, params.receiver_default, index)
        print(validation_report(scores, human, threshold))
    out = args.out or score_path()
    out.parent.mkdir(parents=True, exist_ok=True)
    doc = {"params": {"bias": params.bias, "weights": params.weights, "slot_prior": params.slot_prior,
                      "covered": params.covered, "receiver_default": params.receiver_default},
           "threshold": threshold, "textures": scores}
    out.write_text(json.dumps(doc, indent=1, sort_keys=True), encoding="utf-8")
    n_rcv = sum(not r["emitter"] for r in scores.values())
    print(f"{len(scores)} textures scored, {n_rcv} receivers below {threshold:.2f} -> {out}")
    if args.sheets:
        for p in write_sheets(scores, maps_dir(), tex_root, work / "sheets"):
            print(f"  sheet {p}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
