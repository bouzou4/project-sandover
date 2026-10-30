"""SDR colour against vanilla: Oklab drift per matched region (shots/sdr-colour.yaml).

python -m sandover.sdr_colour measure VANILLA_DIR CANDIDATE_DIR [--json out.json]
python -m sandover.sdr_colour stages CANDIDATE_DIR      # display-transform stages from the EXR
python -m sandover.sdr_colour sheet OUT.png VANILLA_DIR BEFORE_DIR AFTER_DIR

Regions are rects (x, y, w, h at 2560x1440) on content both images share; medians per region,
so shadow edges and specks inside a region do not steer it. Hue shift in degrees (Oklab ab),
chroma and L as candidate / vanilla.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np

from sandover import hdr

REGIONS: dict[str, dict[str, tuple[int, int, int, int]]] = {
    "village1-hut-noon": {"sky": (0, 0, 1700, 120), "sea": (0, 560, 400, 170),
                          "far hills": (0, 330, 2560, 270), "grass L": (100, 1050, 1000, 300),
                          "grass R": (1500, 1050, 800, 300), "skin": (1318, 758, 12, 34),
                          "shirt": (1274, 740, 36, 26)},
    "beach-start-dusk": {"sky": (20, 20, 560, 240), "sea": (20, 560, 560, 140),
                         "sand": (1440, 980, 1000, 300), "sand L": (100, 1050, 1000, 250),
                         "rock": (1500, 120, 900, 450)},
    "jungle-start-noon": {"river": (0, 600, 640, 140), "boulders": (1440, 700, 950, 550),
                          "bridge": (600, 950, 800, 400), "canopy": (600, 0, 1000, 300)},
    "sunken-start-night": {"wall": (0, 0, 520, 1000), "floor ring": (1300, 1050, 1200, 300),
                           "window": (2000, 80, 560, 640), "door": (840, 420, 300, 500)},
    "misty-start-noon": {"cliffs": (1200, 100, 1300, 500), "ground": (100, 1100, 2400, 300)},
    "rolling-start-noon": {"grass": (100, 850, 2400, 550), "cliff": (0, 100, 480, 700),
                           "building": (900, 300, 1000, 400)},
    "blue-vent-noon": {"grass": (40, 160, 700, 500), "leaves": (1200, 800, 1200, 500),
                       "vent stone": (900, 700, 700, 200), "eco": (1000, 80, 350, 350)},
    "green-vent-night": {"grass": (40, 200, 450, 800), "vent stone": (700, 600, 1000, 350),
                         "eco": (900, 40, 700, 300)},
}

M1 = np.array([[0.4122214708, 0.5363325363, 0.0514459929],
               [0.2119034982, 0.6806995451, 0.1073969566],
               [0.0883024619, 0.2817188376, 0.6299787005]])
M2 = np.array([[0.2104542553, 0.7936177850, -0.0040720468],
               [1.9779984951, -2.4285922050, 0.4505937099],
               [0.0259040371, 0.7827717662, -0.8086757660]])


def decode(e: np.ndarray) -> np.ndarray:
    """Display encoding to linear light: the gamma 2.2 the HDR path and an SDR panel use."""
    return np.power(np.clip(e, 0.0, None), 2.2)


def oklab(lin: np.ndarray) -> np.ndarray:
    return np.cbrt(np.clip(lin, 0.0, None) @ M1.T) @ M2.T


def load_png(p: Path, size: tuple[int, int] = (2560, 1440)) -> np.ndarray:
    from PIL import Image

    im = Image.open(p).convert("RGB")
    if im.size != size:
        im = im.resize(size, Image.Resampling.BOX)
    return np.asarray(im, dtype=np.float32) / 255.0


def region_lab(enc: np.ndarray, rect: tuple[int, int, int, int]) -> np.ndarray | None:
    x, y, w, h = rect
    if w <= 0 or h <= 0:
        return None
    lab = oklab(decode(enc[y:y + h, x:x + w].reshape(-1, 3)))
    return np.median(lab, axis=0)


def delta(v: np.ndarray, c: np.ndarray) -> dict[str, float]:
    cv, cc = math.hypot(v[1], v[2]), math.hypot(c[1], c[2])
    dh = math.degrees(math.atan2(c[2], c[1]) - math.atan2(v[2], v[1]))
    dh = (dh + 180.0) % 360.0 - 180.0
    return {"hue_deg": round(dh if min(cv, cc) > 0.01 else 0.0, 1),
            "chroma": round(cc / max(cv, 1e-6), 3), "L": round(c[0] / max(v[0], 1e-6), 3),
            "dE": round(float(np.linalg.norm(c - v)) * 100, 2)}


def measure(van: Path, cand: Path, images: dict[str, np.ndarray] | None = None) -> dict:
    out = {}
    for shot, regs in REGIONS.items():
        vp, cp = van / f"{shot}.png", cand / f"{shot}.png"
        if not (vp.exists() and (cp.exists() or (images and shot in images))):
            continue
        v = load_png(vp)
        c = images[shot] if images and shot in images else load_png(cp)
        rows = {}
        for name, rect in regs.items():
            lv, lc = region_lab(v, rect), region_lab(c, rect)
            if lv is not None:
                rows[name] = delta(lv, lc)
        out[shot] = rows
    return out


def summary(res: dict) -> dict[str, float]:
    rows = [r for s in res.values() for r in s.values()]
    if not rows:
        return {}
    return {"hue_abs_deg": round(float(np.mean([abs(r["hue_deg"]) for r in rows])), 1),
            "chroma": round(float(np.exp(np.mean([math.log(max(r["chroma"], 1e-3)) for r in rows]))), 3),
            "L": round(float(np.exp(np.mean([math.log(max(r["L"], 1e-3)) for r in rows]))), 3),
            "dE": round(float(np.mean([r["dE"] for r in rows])), 2)}


def hdr_map(e: np.ndarray, paper: float = 200.0, peak: float = 1000.0, black: float = 0.0,
            knee: float = 0.0, sat: float = 1.0, toon: bool = True) -> np.ndarray:
    """``hdr_map`` of post_processing.frag, line for line; returns nits."""
    w = hdr.LUMA
    e = np.maximum(e, 0.0)
    lin = np.power(e, 2.2)
    y = lin @ w
    hot = np.power(np.minimum(e, 1.0), 2.2)
    if toon:
        mx = e.max(axis=-1)
        hw = np.maximum(hdr._smoothstep(2.0, 3.5, mx), hdr._smoothstep(1.2, 1.8, mx))[..., None]
        lin = lin + (hot * (y / np.maximum(hot @ w, 1e-6))[..., None] - lin) * hw
    lin = np.maximum(y[..., None] + (lin - y[..., None]) * sat, 0.0)
    y = lin @ w
    x = y * paper
    kn = min(paper + (peak - paper) * min(max(knee, 0.0), 1.0), 0.999 * peak)
    r = peak - kn
    o = np.where(x < kn, x, kn + r * (1.0 - np.exp(-(x - kn) / r)))
    if black > 0:
        o = o + black * (1.0 - np.exp(-o / black))
    nits = lin * (o / np.maximum(y, 1e-12))[..., None]
    m = nits.max(axis=-1)
    nits = nits * np.where(m > peak, peak / np.maximum(m, 1e-12), 1.0)[..., None]
    return np.where((y > 0)[..., None], nits, 0.0)


def stages(cand: Path) -> dict:
    """Per shot from the EXR (the tonemap input): the share of pixels each display-transform
    stage changes, and HDR decoded at paper white against the SDR output below game white."""
    out = {}
    for shot in REGIONS:
        exr = cand / f"{shot}.exr"
        if not exr.exists():
            continue
        e, attrs = hdr.read_exr(exr)
        e = np.nan_to_num(e.astype(np.float32), nan=0.0, posinf=65504.0)
        png = load_png(cand / f"{shot}.png", (e.shape[1], e.shape[0]))
        sdr = hdr.tonemap(e, "toon")
        clampd = np.clip(e, 0.0, 1.0)
        lum = e @ hdr.LUMA
        mx = e.max(axis=-1)
        below = (lum < 1.0) & (mx <= 1.0)
        rel = hdr_map(e) / 200.0
        hdr_enc = np.power(np.clip(rel, 0.0, None), 1 / 2.2)
        d_hdr = np.abs(hdr_enc - sdr).max(axis=-1)[below] * 255
        out[shot] = {
            "model_vs_png_levels": round(float(np.abs(sdr - png).mean() * 255), 3),
            "over1_max_channel_pct": round(float((mx > 1.0).mean() * 100), 2),
            "toon_shoulder_pct": round(float((lum > 0.95).mean() * 100), 2),
            "toon_vs_clamp_levels": round(float(np.abs(sdr - clampd).mean() * 255), 3),
            "below_white_pct": round(float(below.mean() * 100), 2),
            "hdr_vs_sdr_below_white_mean_levels": round(float(d_hdr.mean()), 3),
            "hdr_vs_sdr_below_white_p99_levels": round(float(np.percentile(d_hdr, 99)), 3),
            "hdr_vs_sdr_below_white_max_levels": round(float(d_hdr.max()), 3),
            "tonemap": attrs.get("sandoverTonemapMode"),
        }
    return out


def exr_scaled(cand: Path, shot: str) -> np.ndarray | None:
    """The shot's EXR (tonemap input) at the library size, box-filtered like the PNG."""
    exr = cand / f"{shot}.exr"
    if not exr.exists():
        return None
    e, _ = hdr.read_exr(exr)
    e = np.nan_to_num(e.astype(np.float32), nan=0.0, posinf=65504.0)
    if e.shape[:2] != (1440, 2560):
        from PIL import Image

        e = np.stack([np.asarray(Image.fromarray(np.ascontiguousarray(e[..., i]), "F")
                                 .resize((2560, 1440), Image.Resampling.BOX)) for i in range(3)], -1)
    return e


def attribute(van: Path, cand: Path, curves: dict[str, object]) -> dict:
    """Per region: the drift of each display transform applied to the candidate's EXR. ``clamp``
    is vanilla's per-channel clip, so (curve - clamp) is what the display transform adds and
    (clamp vs vanilla) is the scene (lighting, materials) drift."""
    out = {}
    for shot, regs in REGIONS.items():
        e = exr_scaled(cand, shot)
        vp = van / f"{shot}.png"
        if e is None or not vp.exists():
            continue
        v = load_png(vp)
        mapped = {name: f(e) for name, f in curves.items()}
        rows = {}
        for rn, rect in regs.items():
            lv = region_lab(v, rect)
            if lv is None:
                continue
            rows[rn] = {name: delta(lv, region_lab(np.clip(img, 0, 1), rect))
                        for name, img in mapped.items()}
        out[shot] = rows
    return out


def sheet(out: Path, cols: list[tuple[str, Path]], results: dict[str, dict]) -> None:
    from PIL import Image, ImageDraw, ImageFont

    w, h, pad, lab = 768, 432, 8, 26
    shots = [s for s in REGIONS if all((d / f"{s}.png").exists() for _, d in cols)]
    font = ImageFont.load_default(size=18)
    small = ImageFont.load_default(size=15)
    nrow = max(len(REGIONS[s]) for s in shots)
    block = h + lab + 18 * nrow + pad
    img = Image.new("RGB", (len(cols) * (w + pad) + pad, len(shots) * block + 40), (18, 18, 18))
    d = ImageDraw.Draw(img)
    d.text((pad, 10), "SDR colour vs vanilla (Oklab medians per region: hue shift deg, chroma "
           "ratio, L ratio, dE x100)", fill=(230, 230, 230), font=font)
    for r, s in enumerate(shots):
        y0 = 40 + r * block
        for c, (label, folder) in enumerate(cols):
            x0 = pad + c * (w + pad)
            im = Image.open(folder / f"{s}.png").convert("RGB").resize((w, h))
            dr = ImageDraw.Draw(im)
            for name, (x, y, rw, rh) in REGIONS[s].items():
                if rw > 0:
                    k = w / 2560
                    dr.rectangle([x * k, y * k, (x + rw) * k, (y + rh) * k], outline=(255, 0, 255))
                    dr.text((x * k + 3, y * k + 2), name, fill=(255, 0, 255), font=small)
            img.paste(im, (x0, y0 + lab))
            d.text((x0, y0 + 3), f"{s} - {label}", fill=(230, 230, 230), font=font)
            rows = results.get(label, {}).get(s)
            if rows:
                for i, (name, m) in enumerate(rows.items()):
                    d.text((x0, y0 + lab + h + 2 + 18 * i),
                           f"{name}: hue {m['hue_deg']:+.1f}  C x{m['chroma']:.2f}  "
                           f"L x{m['L']:.2f}  dE {m['dE']:.1f}", fill=(200, 200, 200), font=small)
    img.save(out)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="sandover.sdr_colour")
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("measure")
    m.add_argument("vanilla", type=Path)
    m.add_argument("candidate", type=Path)
    m.add_argument("--json", type=Path)
    s = sub.add_parser("stages")
    s.add_argument("candidate", type=Path)
    at = sub.add_parser("attribute")
    at.add_argument("vanilla", type=Path)
    at.add_argument("candidate", type=Path)
    sh = sub.add_parser("sheet")
    sh.add_argument("out", type=Path)
    sh.add_argument("vanilla", type=Path)
    sh.add_argument("before", type=Path)
    sh.add_argument("after", type=Path)
    a = ap.parse_args(argv)
    if a.cmd == "measure":
        res = measure(a.vanilla, a.candidate)
        doc = {"shots": res, "summary": summary(res)}
        print(json.dumps(doc, indent=1))
        if a.json:
            a.json.write_text(json.dumps(doc, indent=1))
    elif a.cmd == "attribute":
        res = attribute(a.vanilla, a.candidate, {"clamp": lambda e: hdr.tonemap(e, "clamp"),
                                                 "toon": lambda e: hdr.tonemap(e, "toon")})
        for shot, rows in res.items():
            for rn, m in rows.items():
                print(f"{shot:20s} {rn:12s} " + "  ".join(
                    f"{k}: h{v['hue_deg']:+6.1f} C{v['chroma']:5.2f} L{v['L']:5.2f}" for k, v in m.items()))
    elif a.cmd == "stages":
        print(json.dumps(stages(a.candidate), indent=1))
    else:
        res = {"before": measure(a.vanilla, a.before), "after": measure(a.vanilla, a.after)}
        sheet(a.out, [("vanilla", a.vanilla), ("before", a.before), ("after", a.after)], res)
        print(json.dumps({k: summary(v) for k, v in res.items()}, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
