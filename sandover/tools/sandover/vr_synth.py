"""Synthetic VR eye images: runtime profile, A/B sheets, renderer inventory and geometry tests.

The engine's fake XR mode (``pc-set-xr-fake``, ``--xr-fake``) draws both eyes with the
per-eye field of view and view poses the runtime reports every frame and a scripted head pose
(``profile`` only records them for the measurements), and writes
``<shot>-left.png`` / ``<shot>-right.png`` next to the side-by-side shot. See
``kb/howto/vr-synthetic.md``.

Usage::

    uv run sandover-vr-synth profile gk.out.log $SANDOVER_DATA/vr/psvr2-profile.json
    uv run sandover-vr-synth sheet <shots dir> --pair <mono shot>=<xr shot> ...
    uv run sandover-vr-synth inventory <gk log>
    uv run sandover-vr-synth measure <shots dir> --profile <json>
    uv run sandover-vr-synth sky <shots dir> --log <gk log> --shot <cam>=<xr shot> --out <json>
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

VIEW_RE = re.compile(
    r"\[xr\] view (VIEW|LOCAL) (\d) flags (\S+) pos (\S+) (\S+) (\S+) quat (\S+) (\S+) (\S+) (\S+) "
    r"fov_rad (\S+) (\S+) (\S+) (\S+) rec (\d+) (\d+)")
PROBE_RE = re.compile(r"bgprobe: frame (\d+) shot (\w+) eye (-?\d) fb (\d+) (\d+)x(\d+) \| (.*?) \| "
                      r"off-target buckets:(.*)$")


def _font(size: int = 18):
    try:
        return ImageFont.load_default(size=size)
    except TypeError:
        return ImageFont.load_default()


def parse_profile(log_text: str) -> dict:
    """The first full set of ``[xr] view`` lines: per-eye fov (radians) and view poses in VIEW
    space (relative to the head) and LOCAL, plus the recommended image size."""
    views: dict[str, dict[int, dict]] = {"VIEW": {}, "LOCAL": {}}
    rec = None
    for m in VIEW_RE.finditer(log_text):
        space, i = m.group(1), int(m.group(2))
        if i in views[space]:
            continue
        g = [float(x) for x in m.groups()[3:14]]
        views[space][i] = {"flags": m.group(3), "pos": g[0:3], "quat": g[3:7], "fov_rad": g[7:11]}
        rec = [int(m.group(15)), int(m.group(16))]
    if len(views["VIEW"]) != 2:
        raise ValueError("no '[xr] view VIEW 0/1' lines in the log")
    v = [views["VIEW"][0], views["VIEW"][1]]
    ipd = math.dist(v[0]["pos"], v[1]["pos"])
    return {"recommended": rec, "views": v, "local": [views["LOCAL"].get(0), views["LOCAL"].get(1)],
            "ipd_m": ipd}


def quat_to_mat(q) -> np.ndarray:
    x, y, z, w = q
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
        [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
        [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]])


def _fit(img: Image.Image, h: int) -> Image.Image:
    return img.resize((max(1, round(img.width * h / img.height)), h), Image.LANCZOS)


def anaglyph(left: Image.Image, right: Image.Image) -> Image.Image:
    """Red = left eye, cyan = right eye (both in luminance)."""
    lg = np.asarray(left.convert("L"), dtype=np.uint8)
    rg = np.asarray(right.convert("L").resize(left.size), dtype=np.uint8)
    return Image.fromarray(np.dstack([lg, rg, rg]))


def labelled_row(cells: list[tuple[str, Image.Image]], h: int = 360) -> Image.Image:
    font = _font(20)
    fitted = [(t, _fit(im, h)) for t, im in cells]
    pad, lab = 8, 30
    w = sum(im.width for _, im in fitted) + pad * (len(fitted) + 1)
    out = Image.new("RGB", (w, h + lab + 2 * pad), (36, 36, 36))
    d = ImageDraw.Draw(out)
    x = pad
    for t, im in fitted:
        d.text((x, pad), t, fill=(235, 235, 235), font=font)
        out.paste(im.convert("RGB"), (x, pad + lab))
        x += im.width + pad
    return out


def cmd_profile(a) -> int:
    prof = parse_profile(Path(a.log).read_text(errors="replace"))
    Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    Path(a.out).write_text(json.dumps(prof, indent=2))
    deg = [[round(math.degrees(f), 2) for f in v["fov_rad"]] for v in prof["views"]]
    print(f"{a.out}: ipd {prof['ipd_m'] * 1000:.2f} mm, fov deg {deg}, rec {prof['recommended']}")
    return 0


def cmd_sheet(a) -> int:
    d = Path(a.dir)
    rows = []
    for pair in a.pair:
        mono, xr = pair.split("=")
        cells = [(f"{mono}: mono", Image.open(d / f"{mono}.png"))]
        left, right = Image.open(d / f"{xr}-left.png"), Image.open(d / f"{xr}-right.png")
        sbs = Image.open(d / f"{xr}.png")
        half = sbs.width // 2
        # the side-by-side halves share one frustum, so they overlay without the eyes' offsets
        ana = anaglyph(sbs.crop((0, 0, half, sbs.height)), sbs.crop((half, 0, 2 * half, sbs.height)))
        cells += [(f"{xr}: left swapchain", left), (f"{xr}: right swapchain", right),
                  ("anaglyph, shared frustum (red L, cyan R)", ana)]
        rows.append(labelled_row(cells, a.height))
    w = max(r.width for r in rows)
    out = Image.new("RGB", (w, sum(r.height for r in rows)), (36, 36, 36))
    y = 0
    for r in rows:
        out.paste(r, (0, y))
        y += r.height
    out.save(a.out)
    print(a.out)
    return 0


def parse_inventory(log_text: str) -> list[dict]:
    rows = []
    for line in log_text.splitlines():
        m = PROBE_RE.search(line)
        if not m:
            continue
        counts = dict(re.findall(r"(\w+) (\d+)", m.group(7)))
        rows.append({"frame": int(m.group(1)), "shot": m.group(2) in ("1", "true"), "eye": int(m.group(3)),
                     "fb": int(m.group(4)), "size": f"{m.group(5)}x{m.group(6)}",
                     **{k: int(v) for k, v in counts.items()}, "off_target": m.group(8).strip()})
    return rows


def cmd_inventory(a) -> int:
    rows = [r for r in parse_inventory(Path(a.log).read_text(errors="replace")) if r["shot"] or a.all]
    cols = ["frame", "eye", "size", "sky", "tfrag", "tie", "shrub", "merc", "generic", "sprite",
            "ocean", "tex", "other", "off_target"]
    print(" | ".join(cols))
    for r in rows:
        print(" | ".join(str(r.get(c, "")) for c in cols))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="sandover-vr-synth", description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("profile", help="runtime profile JSON from a gk log's [xr] view lines")
    p.add_argument("log")
    p.add_argument("out")
    s = sub.add_parser("sheet", help="mono | left | right | anaglyph rows")
    s.add_argument("dir")
    s.add_argument("--pair", action="append", required=True, metavar="MONO=XR")
    s.add_argument("--height", type=int, default=360)
    s.add_argument("--out", required=True)
    i = sub.add_parser("inventory", help="per-eye draws per renderer category (SANDOVER_BG_PROBE)")
    i.add_argument("log")
    i.add_argument("--all", action="store_true", help="also frames that took no screenshot")
    m = sub.add_parser("measure", help="mono-to-eye registration per head pose; disparity rows")
    m.add_argument("dir")
    m.add_argument("--log", required=True, help="gk log with the [xr] fake lines")
    m.add_argument("--cam", action="append", required=True)
    m.add_argument("--downscale", type=int, default=4)
    m.add_argument("--out", required=True)
    v = sub.add_parser("mvstep", help="per-eye motion vectors of a head step against the geometry")
    v.add_argument("dump", help="pc-motion-vectors-dump directory of the XR frames")
    v.add_argument("--log", required=True, help="gk log with the [xr] fake lines")
    v.add_argument("--before", default="0,0,0", help="head pose before the step: yaw,pitch,roll (deg)")
    v.add_argument("--after", required=True, help="head pose after the step: yaw,pitch,roll (deg)")
    v.add_argument("--centre", type=int, default=32, help="half size of the centre box (px)")
    v.add_argument("--out")
    k = sub.add_parser("sky", help="identity-head eyes vs the mono shot above the horizon")
    k.add_argument("dir")
    k.add_argument("--log", required=True, help="gk log with the [xr] fake lines")
    k.add_argument("--shot", action="append", required=True, metavar="CAM=XR_SHOT")
    k.add_argument("--downscale", type=int, default=4)
    k.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    if a.cmd == "mvstep":
        from sandover.vr_mv import cmd_mvstep
        return cmd_mvstep(a)
    if a.cmd == "measure":
        from sandover.vr_measure import cmd_measure
        return cmd_measure(a)
    if a.cmd == "sky":
        from sandover.vr_measure import sky_metrics
        text = Path(a.log).read_text(errors="replace")
        res = []
        for pair in a.shot:
            cam, shot = pair.split("=")
            res += sky_metrics(Path(a.dir), text, cam, shot, a.downscale)
        Path(a.out).write_text(json.dumps(res, indent=1))
        return 0
    return {"profile": cmd_profile, "sheet": cmd_sheet, "inventory": cmd_inventory}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
