"""Parallax depth sheets: 1:1 crops of each surface at today's depth and at 0.75 and 0.6 of it,
self-shadow off/on rows with the darkest-5% luminance quoted (``sheet.png``), and the shading
layers stacked one by one with a balanced candidate (``stacking.png``), and the normal-map
strength rows at parallax 3.75 cm (``normals.png``), all beside it.

Reads the frames of ``sandover/shots/parallax-depth.yaml``; each crop window is the one where
the row's first and last compared frames differ most, the same window across a row.

Usage: ``uv run --no-sync python scripts/parallax_depth_sheet.py <frames dir> <sheet.png>``
"""
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

CW, CH, PAD, LAB = 1000, 700, 12, 44
BG, FG = (32, 32, 32), (235, 235, 235)
DEPTH_M = 0.05
AMP = [
    ("stone, masonry: vil1-sages-stonewall (village1, noon)", "stonewall-d"),
    ("stone, carved: vil1-sculptstone (village1, noon)", "sculptstone-d"),
    ("stone, rockwall: jng-rockwall-01 (jungle cliff, play snapshot 0002, 20:30)", "jungle-cliff-d"),
    ("precursor: cmn-precursor-stonehengetop (jungle stonehenge floor, noon, in shade)", "stonehenge-floor-d"),
]
TEMPLE = ("precursor: jungle temple top (noon, sunlit)",
          ["stack-temple-top-full", "stack-temple-top-balanced", "stack-temple-top-d60"])
# rows where moving water would win the automatic window: fixed on the surface
CROPS = {"jungle-cliff-d100": (1700, 150), "jungle-cliff-d100-selfshadow-off": (1700, 150),
         "stack-jungle-cliff-baked": (1700, 150), "stack-temple-top-full": (2000, 1400),
         "stack-temple-top-baked": (2000, 1400), "stack-beach-baked": (1700, 1400)}
FACTORS = [1.0, 0.75, 0.6]
SHADOW = [("stonewall", "self-shadow, stone wall at noon"), ("jungle-cliff", "self-shadow, jungle cliff at 20:30")]
LAYERS = ["baked only (materials off)", "+ normal map", "+ parallax 0.05 m", "+ self-shadow (today)",
          "balanced: parallax 0.75, self-shadow on"]
STACK = [
    ("village1 stone wall (stone, masonry), noon",
     ["stack-stonewall-baked", "stack-stonewall-normal", "stonewall-d100-selfshadow-off", "stonewall-d100",
      "stonewall-d75"]),
    ("jungle cliff rock (stone, rockwall), noon",
     [f"stack-jungle-cliff-{a}" for a in ("baked", "normal", "parallax", "full", "balanced")]),
    ("jungle temple top (precursor), noon",
     [f"stack-temple-top-{a}" for a in ("baked", "normal", "parallax", "full", "balanced")]),
    ("beach sand (ground; no parallax texture, so only the normal map adds)",
     ["stack-beach-baked", "stack-beach-normal", None, "stack-beach-full", None]),
]


NORMALS = [
    ("village1 stone wall (stone), noon", "stonewall", (1500, 850)),
    ("village1 carved stone (stone), noon", "sculptstone", (2700, 1450)),
    ("jungle cliff rock (stone), noon", "jungle-cliff-noon", (1700, 150)),
    ("jungle cliff rock (stone), 20:30", "jungle-cliff-night", (1700, 150)),
    ("jungle stonehenge floor (precursor), noon, in shade", "stonehenge-floor", (950, 1450)),
    ("jungle temple top (precursor), noon", "temple-top", (2000, 1400)),
    ("beach sand (ground), noon", "beach", (1700, 1400)),
]
STRENGTHS = [100, 80, 60, 40]


def font(size):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:
        return ImageFont.load_default()


def load(frames, name):
    return np.asarray(Image.open(frames / f"{name}.png").convert("RGB"))


def window(a, b, darker=False):
    """Top-left of the CW x CH window with the most pixels differing by more than 8/255
    (with ``darker``, only pixels where ``b`` is darker than ``a``)."""
    d = a.astype(int) - b.astype(int)
    m = ((d.max(2) if darker else np.abs(d).max(2)) > 8).astype(np.int64)
    s = np.pad(m.cumsum(0).cumsum(1), ((1, 0), (1, 0)))
    best, at = -1, (0, 0)
    for y in range(0, m.shape[0] - CH + 1, 50):
        for x in range(0, m.shape[1] - CW + 1, 50):
            n = s[y + CH, x + CW] - s[y, x + CW] - s[y + CH, x] + s[y, x]
            if n > best:
                best, at = n, (x, y)
    return at, best / (CW * CH)


def lum(img):
    f = img.astype(float) / 255
    return f @ np.array([0.2126, 0.7152, 0.0722])


def dark5(img):
    l = lum(img).ravel()
    return float(l[l <= np.percentile(l, 5)].mean() * 255), float(np.median(l) * 255)


def place(key, a, b, darker=False):
    if key in CROPS:
        return CROPS[key], None
    return window(a, b, darker)


def compose(rows, ncols, out):
    width = PAD + ncols * (CW + PAD)
    height = PAD + len(rows) * (LAB + CH + LAB + PAD)
    sheet = Image.new("RGB", (width, height), BG)
    dr, f1, f2 = ImageDraw.Draw(sheet), font(30), font(26)
    y = PAD
    for label, crops, tags in rows:
        dr.text((PAD, y + 6), label, fill=FG, font=f1)
        y += LAB
        for i, (c, t) in enumerate(zip(crops, tags)):
            x = PAD + i * (CW + PAD)
            if c is not None:
                sheet.paste(Image.fromarray(c), (x, y))
            dr.text((x + 4, y + CH + 8), t, fill=FG, font=f2)
        y += CH + LAB + PAD
    out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out)


def main():
    frames, out = Path(sys.argv[1]), Path(sys.argv[2])
    rows, stats = [], {}
    amp = [(lab, [f"{pre}{a}" for a in ("100", "75", "60")]) for lab, pre in AMP] + [TEMPLE]
    for label, names in amp:
        imgs = [load(frames, n) for n in names]
        (x, y), share = place(names[0], imgs[0], imgs[-1])
        tags = [f"{f:g} x  depth {DEPTH_M * f * 100:.2f} cm" + ("  (today)" if f == 1 else "") for f in FACTORS]
        rows.append((label, [im[y:y + CH, x:x + CW] for im in imgs], tags))
        stats[names[0]] = {"crop_xy": [x, y], "share_changed_today_vs_0.6": share and round(share, 4)}
    for spot, label in SHADOW:
        off, on = load(frames, f"{spot}-d100-selfshadow-off"), load(frames, f"{spot}-d100")
        (x, y), share = place(f"{spot}-d100-selfshadow-off", off, on, darker=True)
        co, cn = off[y:y + CH, x:x + CW], on[y:y + CH, x:x + CW]
        (do, mo), (dn, mn) = dark5(co), dark5(cn)
        s = {"crop_xy": [x, y], "share_changed": share and round(share, 4), "dark5_off": round(do, 1), "dark5_on": round(dn, 1),
             "median_off": round(mo, 1), "median_on": round(mn, 1), "dark5_on_over_off": round(dn / max(do, 1e-6), 3)}
        stats[f"{spot}-selfshadow"] = s
        rows.append((f"{label}: darkest 5% luminance {do:.1f} -> {dn:.1f} /255 (x{s['dark5_on_over_off']:.2f})",
                     [co, cn], [f"off  dark5 {do:.1f}, median {mo:.1f}", f"on (today)  dark5 {dn:.1f}, median {mn:.1f}"]))
    compose(rows, 3, out)
    srows = []
    for label, names in STACK:
        imgs = [load(frames, n) if n else None for n in names]
        (x, y), share = place(names[0], imgs[1], imgs[3])
        crops = [im[y:y + CH, x:x + CW] if im is not None else None for im in imgs]
        tags = [t if n else f"{t}: n/a" for t, n in zip(LAYERS, names)]
        meds = [round(float(np.median(lum(c)) * 255), 1) if c is not None else None for c in crops]
        d5 = [round(dark5(c)[0], 1) if c is not None else None for c in crops]
        stats[f"stack {label}"] = {"crop_xy": [x, y], "median": meds, "dark5": d5}
        srows.append((label, crops, tags))
    compose(srows, 5, out.with_name("stacking.png"))
    nrows = []
    if (frames / "normals-stonewall-n100.png").exists():
        for label, key, (x, y) in NORMALS:
            crops = [load(frames, f"normals-{key}-n{p}")[y:y + CH, x:x + CW] for p in STRENGTHS]
            d = [dark5(c) for c in crops]
            stats[f"normals {key}"] = {"crop_xy": [x, y], "dark5": [round(a, 1) for a, _ in d],
                                       "median": [round(m, 1) for _, m in d]}
            tags = [f"normal x{p / 100:g}{' (today)' if p == 100 else ''}: dark5 {a:.1f}, median {m:.1f}"
                    for p, (a, m) in zip(STRENGTHS, d)]
            nrows.append((f"{label}; parallax 3.75 cm, self-shadow on", crops, tags))
        compose(nrows, 4, out.with_name("normals.png"))
    out.with_suffix(".json").write_text(json.dumps(stats, indent=2))
    print(json.dumps(stats, indent=2))


if __name__ == "__main__":
    main()
