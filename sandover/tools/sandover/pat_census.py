"""Pat census: which collision surface material (Jak's footstep sound) lies under each texture.

For every level, every textured background triangle (tfrag, tie, shrub; the level's
``<level>-background.glb`` from the rip) is matched to the nearest collision triangle of the
level's collide mesh (``out/jak1/fr3/<level>.fr3``) within ``--max-dist`` metres whose plane
faces the same way (``|cos| >= --min-cos``). The collision triangle's pat material (``pat-h.gc``
``pat-material``: stone, pcmetal, wood, ...) is what Jak's feet hear there. Per texture and level
the matched area per pat gives the dominant pat and its share; the texture is joined to
``materials.csv`` by name and the level's DGO and its class compared with the pat.

By default only walkable surfaces count: floor-facing visible triangles (``|n.y| >= 0.7``) on
ground-mode collision, since footsteps only read the ground and walls mostly carry the default
stone pat (``--all-surfaces`` keeps everything). Only background collision is in the .fr3: actor surfaces (platforms, doors, elevators) have
their own collision and are not covered, so their textures show up as unmatched.

Usage::

    uv run sandover-pat-census [--levels jungleb,snow] [--out-dir DIR]

Writes ``$SANDOVER_DATA/materials/candidates/pat-census.csv`` and ``pat-census-report.md``.
"""

from __future__ import annotations

import argparse
import csv
import sys
from collections import Counter, defaultdict
from pathlib import Path

import numpy as np

from sandover import paths
from sandover.sand_textures import PAT_MATERIALS, load_collide_pat

LEVEL_DGO = {
    "beach": "BEA", "citadel": "CIT", "darkcave": "DAR", "demo": "DEM", "finalboss": "FIN",
    "firecanyon": "FIC", "intro": "INT", "jungle": "JUN", "jungleb": "JUB", "lavatube": "LAV",
    "maincave": "MAI", "misty": "MIS", "ogre": "OGR", "robocave": "ROB", "rolling": "ROL",
    "snow": "SNO", "sunken": "SUN", "sunkenb": "SUB", "swamp": "SWA", "title": "TIT",
    "training": "TRA", "village1": "VI1", "village2": "VI2", "village3": "VI3",
}

# Table classes a pat is consistent with; None = the taxonomy has no class for it.
PAT_CLASSES: dict[str, frozenset[str] | None] = {
    "stone": frozenset({"stone"}),
    "ice": frozenset({"ice"}),
    "quicksand": None,
    "waterbottom": None,
    "tar": None,
    "sand": frozenset({"stone"}),
    "wood": frozenset({"wood"}),
    "grass": frozenset({"foliage"}),
    "pcmetal": frozenset({"metal"}),
    "snow": frozenset({"snow"}),
    "deepsnow": frozenset({"snow"}),
    "hotcoals": frozenset({"stone", "lava"}),
    "lava": frozenset({"lava"}),
    "crwood": frozenset({"wood"}),
    "gravel": frozenset({"stone"}),
    "dirt": frozenset({"stone"}),
    "metal": frozenset({"metal"}),
    "straw": frozenset({"foliage", "wood"}),
    "tube": None,
    "swamp": None,
    "stopproj": None,
    "rotate": None,
    "neutral": None,
}
# Pats folded into another class (no class of their own in materials.toml).
PAT_FOLDED = {"sand": "stone", "gravel": "stone", "dirt": "stone", "hotcoals": "stone/lava",
              "deepsnow": "snow", "crwood": "wood", "grass": "foliage", "straw": "foliage/wood",
              "pcmetal": "metal (+precursor tag)"}

MAX_DIST = 0.5
MIN_COS = 0.7
MIN_UP = 0.7
K_NEAREST = 16
ENVMAP_SUFFIX = "-envmap"
FIELDS = ["texture", "combo_id", "tpage", "level", "table_class", "tags", "pat_material", "share",
          "second_pat", "second_share", "samples", "visible_tris", "matched_area_m2", "agreement",
          "join"]


# --- geometry ---------------------------------------------------------------------------------

def tri_normals(t: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Unit normals and areas of triangles (n, 3, 3)."""
    cr = np.cross(t[:, 1] - t[:, 0], t[:, 2] - t[:, 0])
    ln = np.linalg.norm(cr, axis=1)
    return cr / np.maximum(ln, 1e-12)[:, None], 0.5 * ln


def _seg_dist(p, a, b):
    ab = b - a
    t = np.clip(((p - a) * ab).sum(-1) / np.maximum((ab * ab).sum(-1), 1e-12), 0.0, 1.0)
    return np.linalg.norm(p - (a + t[..., None] * ab), axis=-1)


def point_tri_dist(p: np.ndarray, a: np.ndarray, b: np.ndarray, c: np.ndarray) -> np.ndarray:
    """Distance from points p to triangles abc (broadcasting over leading axes)."""
    n = np.cross(b - a, c - a)
    nn = np.maximum((n * n).sum(-1), 1e-18)
    d = ((p - a) * n).sum(-1) / nn
    q = p - d[..., None] * n
    inside = np.ones(q.shape[:-1], bool)
    for u, v in ((a, b), (b, c), (c, a)):
        inside &= (np.cross(v - u, q - u) * n).sum(-1) >= 0
    plane = np.abs(d) * np.sqrt(nn)
    edge = np.minimum(np.minimum(_seg_dist(p, a, b), _seg_dist(p, b, c)), _seg_dist(p, c, a))
    return np.where(inside, plane, edge)


def match_tris(vis: np.ndarray, col: np.ndarray, max_dist: float = MAX_DIST,
               min_cos: float = MIN_COS, k: int = K_NEAREST, chunk: int = 100_000) -> np.ndarray:
    """Index of the collision triangle under each visible triangle (-1 if none in range)."""
    from scipy.spatial import cKDTree

    out = np.full(len(vis), -1, np.int64)
    if not len(vis) or not len(col):
        return out
    cn, _ = tri_normals(col)
    vn, _ = tri_normals(vis)
    tree = cKDTree(col.mean(1))
    k = min(k, len(col))
    for s in range(0, len(vis), chunk):
        v = vis[s:s + chunk]
        cen = v.mean(1)
        _, idx = tree.query(cen, k=k)
        idx = idx.reshape(len(v), k)
        ct = col[idx]
        dist = point_tri_dist(cen[:, None], ct[..., 0, :], ct[..., 1, :], ct[..., 2, :])
        cos = np.abs((cn[idx] * vn[s:s + chunk, None]).sum(-1))
        dist = np.where(cos >= min_cos, dist, np.inf)
        j = np.argmin(dist, axis=1)
        best = dist[np.arange(len(v)), j]
        out[s:s + chunk] = np.where(best <= max_dist, idx[np.arange(len(v)), j], -1)
    return out


# --- level data -------------------------------------------------------------------------------

def glb_path(level: str) -> Path:
    return (paths.data_dir() / "unbake/riproot/decompiler_out/jak1/levels" / level
            / f"{level}-background.glb")


def fr3_path(level: str) -> Path:
    return paths.root() / "out/jak1/fr3" / f"{level}.fr3"


def visible_tris(path: Path):
    """(material name, triangles (n, 3, 3) in world metres) per primitive, node transforms
    applied (wind tie instances carry a matrix)."""
    from pygltflib import GLTF2

    g = GLTF2().load(str(path))
    blob = g.binary_blob()

    def acc(i):
        a = g.accessors[i]
        bv = g.bufferViews[a.bufferView]
        n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[a.type]
        dt = {5126: np.float32, 5125: np.uint32, 5123: np.uint16, 5121: np.uint8}[a.componentType]
        off = (bv.byteOffset or 0) + (a.byteOffset or 0)
        isz = np.dtype(dt).itemsize
        if bv.byteStride and bv.byteStride != n * isz:
            raw = np.frombuffer(blob, np.uint8, count=bv.byteStride * a.count, offset=off)
            return raw.reshape(a.count, bv.byteStride)[:, :n * isz].copy().view(dt).reshape(a.count, n)
        x = np.frombuffer(blob, dt, count=a.count * n, offset=off)
        return x.reshape(a.count, n) if n > 1 else x

    def local(node) -> np.ndarray:
        if node.matrix:
            return np.array(node.matrix, np.float64).reshape(4, 4).T
        m = np.eye(4)
        if node.scale:
            m = np.diag([*node.scale, 1.0]) @ m
        if node.rotation:
            x, y, z, w = node.rotation
            r = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                          [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                          [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
            rm = np.eye(4)
            rm[:3, :3] = r
            m = rm @ m
        if node.translation:
            m[:3, 3] += node.translation
        return m

    roots = set(range(len(g.nodes))) - {c for n in g.nodes for c in (n.children or [])}
    stack = [(i, np.eye(4)) for i in sorted(roots)]
    while stack:
        i, parent = stack.pop()
        node = g.nodes[i]
        world = parent @ local(node)
        stack.extend((c, world) for c in (node.children or []))
        if node.mesh is None:
            continue
        for pr in g.meshes[node.mesh].primitives:
            if pr.material is None or pr.mode not in (None, 4) or pr.indices is None:
                continue
            name = g.materials[pr.material].name
            if not name:
                continue
            P = acc(pr.attributes.POSITION).astype(np.float64)
            P = P @ world[:3, :3].T + world[:3, 3]
            idx = acc(pr.indices).astype(np.int64).reshape(-1, 3)
            yield name, P[idx]


def ground_mask(normals: np.ndarray, min_up: float = MIN_UP) -> np.ndarray:
    """Floor-like triangles (either winding). Walls and trunks carry the default stone pat
    because footsteps only read the ground, so they are left out."""
    return np.abs(normals[:, 1]) >= min_up


def census_level(level: str, max_dist: float = MAX_DIST, min_cos: float = MIN_COS,
                 ground_only: bool = True) -> dict:
    """Material name -> {"tris", "matched", "pat_area": Counter, "pat_tris": Counter}.
    ``ground_only``: walkable surfaces only (see ``ground_mask``)."""
    col, pat = load_collide_pat(fr3_path(level))
    if ground_only:
        col, pat = col[(pat >> 3) & 7 == 0], pat[(pat >> 3) & 7 == 0]
    mats = (pat >> 6) & 0x3F
    names, chunks = [], []
    for name, t in visible_tris(glb_path(level)):
        names.append(name)
        chunks.append(t)
    out: dict[str, dict] = {}
    if not chunks:
        return out
    vis = np.concatenate(chunks)
    owner = np.repeat(np.arange(len(names)), [len(c) for c in chunks])
    nrm, area = tri_normals(vis)
    keep = area > 1e-6
    if ground_only:
        keep &= ground_mask(nrm)
    vis, owner, area = vis[keep], owner[keep], area[keep]
    hit = match_tris(vis, col, max_dist, min_cos)
    for o, h, a in zip(owner, hit, area):
        rec = out.setdefault(names[o], {"tris": 0, "matched": 0, "pat_area": Counter(),
                                        "pat_tris": Counter()})
        rec["tris"] += 1
        if h >= 0:
            m = int(mats[h])
            p = PAT_MATERIALS[m] if m < len(PAT_MATERIALS) else f"pat{m}"
            rec["matched"] += 1
            rec["pat_area"][p] += float(a)
            rec["pat_tris"][p] += 1
    return out


# --- join and verdict -------------------------------------------------------------------------

def table_index(rows: list[dict]) -> dict[str, list[dict]]:
    by_name: dict[str, list[dict]] = defaultdict(list)
    for r in rows:
        by_name[r["name"]].append(r)
    return by_name


def join_row(by_name: dict[str, list[dict]], name: str, level: str) -> tuple[dict | None, str, str]:
    """(table row, texture name used, join method) for a glb material name in a level."""
    cands = [name]
    if name.endswith(ENVMAP_SUFFIX):
        cands.append(name[: -len(ENVMAP_SUFFIX)])
    dgo = LEVEL_DGO.get(level, level.upper()[:3]) + ".DGO"
    for n in cands:
        rows = by_name.get(n, [])
        if not rows:
            continue
        here = [r for r in rows if dgo in (r.get("levels") or "").split(";")]
        if len(here) == 1:
            return here[0], n, "name+level"
        if len(rows) == 1:
            return rows[0], n, "name"
        pool = here or rows
        # same name on several tpages (tfrag/shrub/pris): fine unless their classes differ
        how = "same-class" if len({r["class"] for r in pool}) == 1 else "conflict"
        tfrag = [r for r in pool if r["tpage"].endswith("-tfrag")]
        return (tfrag or pool)[0], n, how
    return None, cands[-1], "none"


def verdict(table_class: str | None, pat: str | None) -> str:
    if pat is None:
        return "no-match"
    if not table_class:
        return "unjoined"
    ok = PAT_CLASSES.get(pat)
    if ok is None:
        return "no-class"
    if table_class in ok:
        return "agree"
    # stone is pat-material 0, the default: weak evidence against another class
    return "disagree-stone" if pat == "stone" else "disagree"


def dominant(pat_area: Counter) -> tuple[str | None, float, str, float]:
    total = sum(pat_area.values())
    if total <= 0:
        return None, 0.0, "", 0.0
    top = pat_area.most_common(2)
    second = top[1] if len(top) > 1 else ("", 0.0)
    return top[0][0], top[0][1] / total, second[0], second[1] / total


def build_rows(per_level: dict[str, dict], table: list[dict]) -> list[dict]:
    by_name = table_index(table)
    merged: dict[tuple[str, str], tuple[dict | None, str, dict]] = {}
    for level, recs in per_level.items():
        for name, rec in recs.items():
            row, tex, how = join_row(by_name, name, level)
            if (level, tex) in merged:
                acc = merged[(level, tex)][2]
                for k in ("tris", "matched", "pat_area", "pat_tris"):
                    acc[k] += rec[k]
            else:
                merged[(level, tex)] = (row, how, {k: (v.copy() if isinstance(v, Counter) else v)
                                                   for k, v in rec.items()})
    rows = []
    for (level, tex), (row, how, rec) in merged.items():
        pat, share, pat2, share2 = dominant(rec["pat_area"])
        cls = row["class"] if row else ""
        rows.append({
            "texture": tex, "combo_id": row["combo_id"] if row else "",
            "tpage": row["tpage"] if row else "", "level": level, "table_class": cls,
            "tags": row.get("tags", "") if row else "", "pat_material": pat or "",
            "share": f"{share:.3f}" if pat else "", "second_pat": pat2,
            "second_share": f"{share2:.3f}" if pat2 else "",
            "samples": rec["pat_tris"][pat] if pat else 0, "visible_tris": rec["tris"],
            "matched_area_m2": f"{sum(rec['pat_area'].values()):.1f}",
            "agreement": verdict(cls, pat), "join": how,
        })
    rows.sort(key=lambda r: (r["level"], r["texture"]))
    return rows


# --- report -----------------------------------------------------------------------------------

def _tbl(rows: list[dict], cols: list[str]) -> list[str]:
    out = ["| " + " | ".join(cols) + " |", "|" + "---|" * len(cols)]
    out += ["| " + " | ".join(str(r.get(c, "")) for c in cols) + " |" for r in rows]
    return out


def render_report(rows: list[dict], per_level: dict[str, dict], params: dict) -> str:
    cols = ["texture", "level", "table_class", "pat_material", "share", "second_pat", "samples"]
    L = ["# Pat census: collision footstep material under each texture", "",
         f"Generated by `sandover-pat-census` (max distance {params['max_dist']} m, "
         f"|cos| >= {params['min_cos']}, "
         f"{'walkable floors only' if params.get('ground_only', True) else 'all surfaces'}). One row per texture and level in `pat-census.csv`; "
         "`share` is the dominant pat's fraction of the matched area, `samples` its triangle "
         "count. Background collision only: actor surfaces (platforms, doors) are not covered. "
         "`disagree-stone` = the pat is stone, enum 0 and the default, so weak evidence; "
         "`disagree` = a deliberate non-stone pat contradicts the table class.", ""]

    L += ["## Reliability", ""]
    tot = sum(r["tris"] for recs in per_level.values() for r in recs.values())
    hit = sum(r["matched"] for recs in per_level.values() for r in recs.values())
    L.append(f"- Visible textured {'floor ' if params.get('ground_only', True) else ''}triangles: {tot}; matched to collision: {hit} "
             f"({100 * hit / max(tot, 1):.1f}%); no collision within range: {tot - hit} "
             f"({100 * (tot - hit) / max(tot, 1):.1f}%). Unmatched floors are mostly out-of-bounds "
             "terrain, vista, decals over gaps and actor-collided surfaces (platforms).")
    joins = Counter(r["join"] for r in rows)
    L.append(f"- Join to materials.csv (texture rows): {dict(joins)}. `name+level` = unique name "
             "in the level's DGO; `name` = globally unique name; `same-class` = the name is on "
             "several tpages of the level, all with one class; `conflict` = those rows disagree "
             "on class (the tfrag row is used): "
             + (", ".join(sorted({f"{r['texture']} ({r['level']})" for r in rows
                                 if r["join"] == "conflict"})) or "none") + ".")
    L.append("- Per level, matched triangles:")
    for lv, recs in sorted(per_level.items()):
        t = sum(r["tris"] for r in recs.values())
        h = sum(r["matched"] for r in recs.values())
        L.append(f"  - {lv}: {h}/{t} ({100 * h / max(t, 1):.0f}%)")
    L.append("")

    area = Counter()
    for recs in per_level.values():
        for r in recs.values():
            area.update(r["pat_area"])
    L += ["## Pat materials found", "",
          "| pat | matched area m2 | table class |", "|---|---|---|"]
    for p, a in area.most_common():
        cls = PAT_CLASSES.get(p)
        L.append(f"| {p} | {a:.0f} | {'/'.join(sorted(cls)) if cls else '**none**'} |")
    none = [p for p in area if PAT_CLASSES.get(p) is None]
    folded = [f"{p} -> {PAT_FOLDED[p]}" for p in area if p in PAT_FOLDED]
    L += ["", f"- No table class: {', '.join(none) or '-'}.",
          f"- Folded into another class: {', '.join(folded) or '-'}.", ""]

    real = [r for r in rows if r["pat_material"]]
    cnt = Counter(r["agreement"] for r in rows)
    L += ["## Disagreements (by sample count)", "", f"Verdicts: {dict(cnt)}.", ""]
    for kind, title in (("disagree", "Non-stone pat contradicts the table"),
                        ("disagree-stone", "Stone pat under a non-stone class (weak)")):
        dis = sorted((r for r in real if r["agreement"] == kind), key=lambda r: -int(r["samples"]))
        L += [f"### {title} ({len(dis)})", ""]
        L += _tbl(dis[:60], cols)
        if len(dis) > 60:
            L.append(f"\n{len(dis) - 60} more in the CSV.")
        L.append("")

    pc = sorted((r for r in real if "precursor" in r["texture"] or "precursor" in r["tags"]),
                key=lambda r: -int(r["samples"]))
    pcm = Counter(r["pat_material"] for r in pc)
    L += ["## Precursor textures (expected pcmetal)", "",
          f"Dominant pat over {len(pc)} precursor texture rows with a match: {dict(pcm)}.", ""]
    L += _tbl(pc, cols + ["tags"])
    L.append("")

    jb = [r for r in rows if r["level"] == "jungleb"
          and ("stonehenge" in r["texture"] or "darker-metal" in r["texture"])]
    L += ["## jungleb stonehenge and darker-metal", ""]
    L += _tbl(jb, cols + ["second_share", "visible_tris"])
    L.append("")

    ice = sorted((r for r in real if r["table_class"] in ("ice", "snow")
                  or "ice" in r["texture"].split("-") or "snow" in r["texture"]),
                 key=lambda r: -int(r["samples"]))
    L += ["## Ice and snow textures", ""]
    L += _tbl(ice, cols)
    L.append("")

    sm = sorted((r for r in real if r["table_class"] == "stone"
                 and r["pat_material"] in ("pcmetal", "metal")), key=lambda r: -int(r["samples"]))
    L += ["## Table says stone, pat says metal or pcmetal", ""]
    L += _tbl(sm, cols)
    L.append("")
    return "\n".join(L)


def levels_available() -> list[str]:
    return [lv for lv in sorted(LEVEL_DGO) if fr3_path(lv).exists() and glb_path(lv).exists()]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="sandover-pat-census", description=__doc__.split("\n")[0])
    ap.add_argument("--levels", help="comma-separated levels (default: every level with data)")
    ap.add_argument("--max-dist", type=float, default=MAX_DIST)
    ap.add_argument("--min-cos", type=float, default=MIN_COS)
    ap.add_argument("--all-surfaces", action="store_true",
                    help="match walls and ceilings too, not only walkable floors")
    ap.add_argument("--table", type=Path, help="materials.csv (default: the live table)")
    ap.add_argument("--out-dir", type=Path, help="default $SANDOVER_DATA/materials/candidates")
    a = ap.parse_args(argv)

    levels = a.levels.split(",") if a.levels else levels_available()
    table_path = a.table or paths.data_dir() / "materials/materials.csv"
    with open(table_path, newline="", encoding="utf-8") as f:
        table = list(csv.DictReader(f))
    per_level = {}
    for lv in levels:
        try:
            per_level[lv] = census_level(lv, a.max_dist, a.min_cos, not a.all_surfaces)
        except (OSError, RuntimeError) as e:
            print(f"{lv}: skipped ({e})", file=sys.stderr)
            continue
        recs = per_level[lv].values()
        print(f"{lv}: {sum(r['matched'] for r in recs)}/{sum(r['tris'] for r in recs)} matched")
    rows = build_rows(per_level, table)
    out = a.out_dir or paths.data_dir() / "materials/candidates"
    out.mkdir(parents=True, exist_ok=True)
    with open(out / "pat-census.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, FIELDS, lineterminator="\n")
        w.writeheader()
        w.writerows(rows)
    params = {"max_dist": a.max_dist, "min_cos": a.min_cos, "ground_only": not a.all_surfaces}
    (out / "pat-census-report.md").write_text(render_report(rows, per_level, params),
                                              encoding="utf-8", newline="\n")
    print(f"wrote {len(rows)} rows to {out / 'pat-census.csv'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
