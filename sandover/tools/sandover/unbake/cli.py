"""Run the unbake fit on ripped levels: sandover-unbake <level>... [--levels-dir D] [--out D].

`sandover-unbake export-sidecar <level>...` writes the per-vertex sidecar (see sidecar.py).
`sandover-unbake canopy [<level>...]` writes the canopy shade maps (see canopy.py).
`sandover-unbake canopy-vol [<level>...]` writes the leaf volume maps for backlit leaves
(see canopy_vol.py).
"""

from __future__ import annotations

import argparse
import json
import os
import time
from pathlib import Path

import numpy as np

from . import fit as F
from . import moodtables as mt
from . import points as P
from .glb import load_background
from .levels import LEVELS, POINT_LIGHT_KINDS

VILLAGE3_RUNTIME = {0: (-0.067, 0.25, 0.966), 1: (0.259, 0.966, 0.0), 2: (-0.067, 0.25, -0.966), 3: (0.866, 0.5, 0.0)}


def gather(trees):
    xs, ns, cs, comps, kinds, tris = [], [], [], [], [], []
    off_v = off_c = 0
    for t in trees:
        used = np.unique(t.tris)
        remap = np.full(len(t.pos), -1)
        remap[used] = np.arange(len(used)) + off_v
        xs.append(t.pos[used])
        ns.append(t.normal[used])
        cs.append(t.colors[:, used].transpose(1, 0, 2))
        comps.append(t.component[used] + off_c)
        kinds += [t.kind] * len(used)
        tris.append(remap[t.tris])
        off_v += len(used)
        off_c += t.component.max() + 1
    comp = np.unique(np.concatenate(comps), return_inverse=True)[1].ravel()
    return np.concatenate(xs), np.concatenate(ns), np.concatenate(cs), comp, np.array(kinds), np.concatenate(tris)


def stats(e: np.ndarray) -> dict:
    return {"median": float(np.median(e)), "p90": float(np.percentile(e, 90)), "max": float(e.max())}


def run_level(level: str, levels_dir: Path, root: Path, out: Path, sample: int, blind: bool, viz_on: bool) -> dict:
    t0 = time.time()
    roles = LEVELS[level]
    lights, _, ctxs = mt.parse(root)
    table = lights[ctxs[mt.level_moods(root)[level]]["mood-lights-table"]]
    x, n, C, comp, kind, tris = gather(load_background(levels_dir / level / f"{level}-background.glb"))
    fitm = np.isin(kind, ["tfrag", "tie"])
    S = roles.sun
    Cs = C[:, S]
    tdir = np.array([table[s].direction for s in S])
    g0 = F.Globals(tdir.copy(), np.array([table[s].lgt_color for s in S]) * F.U8 + 1.0,
                   np.array([table[s].amb_color for s in S]) * F.U8 + 1.0)
    fi = np.flatnonzero(fitm)
    comp_f = np.unique(comp[fi], return_inverse=True)[1].ravel()
    r = F.fit_level(Cs[fi], n[fi], comp_f, g0, sample=sample)
    fits = {"table": r}
    if blind and len(S) >= 2:
        gb = F.Globals(np.tile([0.0, 1.0, 0.0], (len(S), 1)), np.full((len(S), 3), 200.0), np.full((len(S), 3), 80.0))
        fits["blind"] = F.fit_level(Cs[fi], n[fi], comp_f, gb, sample=sample, blind=True)
    for f in fits.values():  # flipping all normals and all suns together is unobservable; keep suns up
        if (f.g.d[:, 1] * np.linalg.norm(f.g.lgt, axis=1)).sum() < 0:
            f.g.d *= -1
    g = r.g
    sign = F.pick_signs(Cs, n, comp, g)
    ns = n * sign[comp][:, None]
    nd = F.lambert(ns, g.d)
    a0, k0 = F.solve_ak(Cs, nd, g)
    pred0 = F.predict(a0, k0, nd, g)
    a, k, v = F.fit_visibility(Cs, ns, a0, k0, g)
    pred = F.predict(a, k, v * nd, g)

    res = {"level": level, "category": roles.category, "note": roles.note, "sun_slots": S,
           "n_vertices": len(x), "n_fit_vertices": int(fitm.sum()), "kinds": {}}
    per_slot = []
    for j, s in enumerate(S):
        lit = nd[fitm, j] > 0.2
        row = {"slot": s, "angle_table_deg": float(F.angle_deg(g.d[j], tdir[j])),
               "dir_fit": g.d[j].round(4).tolist(), "dir_table": tdir[j].round(4).tolist(),
               "lgt_fit_u8": g.lgt[j].round(1).tolist(), "amb_fit_u8": g.amb[j].round(1).tolist(),
               "lgt_table_u8": (table[s].lgt_color * F.U8).round(1).tolist(),
               "err_novis": stats(np.abs(pred0[fitm, j] - Cs[fitm, j])),
               "err_vis": stats(np.abs(pred[fitm, j] - Cs[fitm, j])),
               "shadowed_frac": float(((v[fitm, j] < 0.5) & lit).sum() / max(lit.sum(), 1))}
        if "blind" in fits:
            row["angle_blind_vs_table_init_deg"] = float(F.angle_deg(fits["blind"].g.d[j], g.d[j]))
            row["angle_blind_vs_table_deg"] = float(F.angle_deg(fits["blind"].g.d[j], tdir[j]))
        if level == "village3" and s in VILLAGE3_RUNTIME:
            row["angle_runtime_override_deg"] = float(F.angle_deg(g.d[j], np.array(VILLAGE3_RUNTIME[s])))
        per_slot.append(row)
    res["slots"] = per_slot
    res["overall_novis"] = F.errors(Cs[fitm], pred0[fitm])
    res["overall_vis"] = F.errors(Cs[fitm], pred[fitm])
    res["baseline_const"] = F.errors(Cs[fitm], np.repeat(Cs[fitm].mean(1, keepdims=True), len(S), 1))
    for kd in np.unique(kind):
        m = kind == kd
        res["kinds"][kd] = {"n": int(m.sum()), "novis": F.errors(Cs[m], pred0[m]), "vis": F.errors(Cs[m], pred[m])}
    res["vertices_needing_occlusion_frac"] = float((((v < 0.5) & (nd > 0.2)).any(1))[fitm].mean())

    pts = {}
    res["effects"] = []
    for s, role in roles.effects.items():
        row = {"slot": s, "role": role}
        if role in POINT_LIGHT_KINDS:
            Ce = C[fi, s]
            Y = Ce.mean(1)
            seeds = P.seed_clusters(x[fi], Y)
            base = np.minimum(a[fi, None] * np.median(Ce / np.maximum(a[fi, None], 0.05), 0)[None], 255)
            row["baseline_ambient_only"] = stats(np.abs(base - Ce))
            row["baseline_ambient_only_lit"] = stats(np.abs(base - Ce)[Y > 12]) if (Y > 12).any() else None
            row["n_lights"] = len(seeds)
            if len(seeds):
                lit = Y > 12
                row["lit_frac"] = float(lit.mean())
                for mode, win in (("invsq", False), ("windowed", True)):
                    pf = P.fit_points(x[fi], ns[fi], Ce, a[fi], seeds, windowed=win)
                    row[mode] = {"err_all": stats(np.abs(pf.pred - Ce)),
                                 "err_lit": stats(np.abs(pf.pred[lit] - Ce[lit])) if lit.any() else None,
                                 "lights_pos": pf.pos.round(2).tolist(), "lights_rgb": pf.rgb.round(0).tolist(),
                                 "radius": pf.radius.round(2).tolist(), "amb": pf.amb.round(1).tolist()}
                full_b = np.zeros((len(x), 3))
                full_p = np.zeros((len(x), 3))
                full_b[fi] = Ce
                full_p[fi] = pf.pred
                pts[s] = (pf.pos, pf.rgb, full_b, full_p)
        else:
            row["modeled"] = False
        res["effects"].append(row)

    out.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(out / f"{level}-fit.npz", x=x.astype(np.float32), tris=tris.astype(np.uint32), slots=S,
                        baked=Cs.astype(np.uint8), fitted=pred.astype(np.float32),
                        fitted_novis=pred0.astype(np.float32), v=v.astype(np.float16), a=a, k=k,
                        normal=ns.astype(np.float32), kind=kind)
    if viz_on:
        from . import viz

        viz.record(out / f"{level}.rrd", level, x, tris, S, Cs, pred, v, g.d, tdir, pts)
        j = int(np.argmax([table[s].direction[1] for s in S]))  # highest sun
        viz.residual_glb(out / f"{level}-residual.glb", x, tris, {
            "residual_noon": viz.heat(np.abs(pred[:, j] - Cs[:, j]).mean(1), 32.0),
            "residual_noon_novis": viz.heat(np.abs(pred0[:, j] - Cs[:, j]).mean(1), 32.0),
            "visibility_noon": viz.heat(v[:, j], 1.0),
            "baked_noon": viz.to_rgb8(Cs[:, j]),
            "fitted_noon": viz.to_rgb8(pred[:, j])})
    res["seconds"] = round(time.time() - t0, 1)
    (out / f"{level}-summary.json").write_text(json.dumps(res, indent=1))
    return res


def main(argv=None):
    import sys

    argv = sys.argv[1:] if argv is None else argv
    if argv[:1] == ["export-sidecar"]:
        from . import sidecar

        return sidecar.main(argv[1:])
    if argv[:1] == ["canopy"]:
        from . import canopy

        return canopy.main(argv[1:])
    if argv[:1] == ["canopy-vol"]:
        from . import canopy_vol

        return canopy_vol.main(argv[1:])
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("levels", nargs="*", default=list(LEVELS))
    root = Path(os.environ.get("SANDOVER_ROOT", "."))
    data = Path(os.environ.get("SANDOVER_DATA", root / "sandover/data"))
    ap.add_argument("--levels-dir", type=Path, default=root / "decompiler_out/jak1/levels")
    ap.add_argument("--root", type=Path, default=root)
    ap.add_argument("--out", type=Path, default=data / "unbake")
    ap.add_argument("--sample", type=int, default=30000)
    ap.add_argument("--no-blind", action="store_true")
    ap.add_argument("--no-viz", action="store_true")
    a = ap.parse_args(argv)
    for lvl in a.levels:
        r = run_level(lvl, a.levels_dir, a.root, a.out, a.sample, not a.no_blind, not a.no_viz)
        print(lvl, r["category"], [round(s["angle_table_deg"], 1) for s in r["slots"]],
              round(r["overall_vis"]["median"], 2), round(r["overall_novis"]["median"], 2), f'{r["seconds"]}s',
              flush=True)


if __name__ == "__main__":
    main()
