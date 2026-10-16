"""HDR captures: read the engine's EXR, print its range, and tonemap it offline.

With the FP16 target on, every screenshot (rig shot, F9 snapshot, F2) also writes
``<name>.exr``: the buffer the final pass tonemaps, as half-float RGB, 1.0 = SDR white
(``kb/howto/hdr.md``). Values are display-encoded, not linear; generic EXR viewers show
the image lighter than the game. ``stats`` prints peak, mean and the share of pixels above 1.0 and 2.0
per channel; ``--preview`` writes a PNG through one of the tonemap curves, so tonemap variants
come from one capture without relaunching the game. ``core`` prints the mean RGB of the
brightest 1% of pixels per window under a curve (and in any PNG given with ``--vs``), the
measure used for emitter cores against vanilla (#263).

Usage::

    uv run sandover-hdr stats shot.exr
    uv run sandover-hdr stats shot.exr --preview shot-toon.png --curve toon --knee 0.8
    uv run sandover-hdr core shot.exr --curve toon --rect 2150,1130,320,280 --vs shot.png
"""

from __future__ import annotations

import argparse
import functools
import json
import struct
import sys
from pathlib import Path

import numpy as np

MAGIC = 20000630
_HALF, _FLOAT = 1, 2
CURVES = ("clamp", "filmic", "toon")
# Rec. 709 luma weights, applied to the framebuffer values as they are
LUMA = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def _cstr(buf: bytes, at: int) -> tuple[str, int]:
    end = buf.index(b"\0", at)
    return buf[at:end].decode("latin-1"), end + 1


def read_exr(path: Path) -> tuple[np.ndarray, dict[str, str]]:
    """Read an uncompressed scanline EXR with R, G, B channels (half or float).

    Returns (H x W x 3 float32, top row first; the string header attributes)."""
    buf = Path(path).read_bytes()
    magic, version = struct.unpack_from("<II", buf, 0)
    if magic != MAGIC or version & 0xFF != 2 or version & 0xFFFFFF00:
        raise ValueError(f"{path}: not a single-part scanline EXR")
    at = 8
    channels: list[tuple[str, int]] = []
    attrs: dict[str, str] = {}
    window = None
    while True:
        name, at = _cstr(buf, at)
        if not name:
            break
        typ, at = _cstr(buf, at)
        (size,) = struct.unpack_from("<i", buf, at)
        at += 4
        val = buf[at:at + size]
        if name == "channels":
            c = 0
            while val[c] != 0:
                cn, c = _cstr(val, c)
                (pt,) = struct.unpack_from("<i", val, c)
                channels.append((cn, pt))
                c += 16
        elif name == "compression" and val[0] != 0:
            raise ValueError(f"{path}: compressed EXR (only uncompressed is supported)")
        elif name == "dataWindow":
            window = struct.unpack_from("<4i", val)
        elif typ == "string":
            attrs[name] = val.decode("utf-8", "replace")
        at += size
    if window is None or any(pt not in (_HALF, _FLOAT) for _, pt in channels):
        raise ValueError(f"{path}: unsupported channel types or no dataWindow")
    names = [n for n, _ in channels]
    if not {"R", "G", "B"} <= set(names):
        raise ValueError(f"{path}: needs R, G, B channels, has {names}")
    x0, y0, x1, y1 = window
    w, h = x1 - x0 + 1, y1 - y0 + 1
    offsets = np.frombuffer(buf, "<u8", h, at)
    out = np.zeros((h, w, 3), np.float32)
    rgb_index = {"R": 0, "G": 1, "B": 2}
    for off in offsets:
        y, _n = struct.unpack_from("<iI", buf, int(off))
        p = int(off) + 8
        for cn, pt in channels:
            dt, bpp = ("<f2", 2) if pt == _HALF else ("<f4", 4)
            row = np.frombuffer(buf, dt, w, p)
            p += w * bpp
            if cn in rgb_index:
                out[y - y0, :, rgb_index[cn]] = row
    return out, attrs


def write_exr(path: Path, rgb: np.ndarray, attrs: dict[str, str] | None = None) -> None:
    """Write H x W x 3 as the engine does: half B, G, R, uncompressed, one line per chunk."""
    h, w, _ = rgb.shape

    def attr(name: str, typ: str, val: bytes) -> bytes:
        return name.encode() + b"\0" + typ.encode() + b"\0" + struct.pack("<i", len(val)) + val

    chlist = b"".join(c.encode() + b"\0" + struct.pack("<iIii", _HALF, 0, 1, 1) for c in "BGR") + b"\0"
    box = struct.pack("<4i", 0, 0, w - 1, h - 1)
    head = struct.pack("<II", MAGIC, 2) + b"".join([
        attr("channels", "chlist", chlist),
        attr("compression", "compression", b"\0"),
        attr("dataWindow", "box2i", box),
        attr("displayWindow", "box2i", box),
        attr("lineOrder", "lineOrder", b"\0"),
        attr("pixelAspectRatio", "float", struct.pack("<f", 1.0)),
        attr("screenWindowCenter", "v2f", struct.pack("<2f", 0.0, 0.0)),
        attr("screenWindowWidth", "float", struct.pack("<f", 1.0)),
        *(attr(k, "string", v.encode()) for k, v in sorted((attrs or {}).items())),
    ]) + b"\0"
    line = w * 6
    first = len(head) + 8 * h
    table = np.arange(h, dtype="<u8") * (8 + line) + first
    half = rgb.astype("<f2")
    chunks = [struct.pack("<iI", y, line) + half[y, :, ::-1].T.tobytes() for y in range(h)]
    Path(path).write_bytes(head + table.tobytes() + b"".join(chunks))


def stats(rgb: np.ndarray) -> dict[str, dict[str, float]]:
    """Per channel and for the per-pixel max channel: peak, mean, % above 1.0 and 2.0."""
    finite = np.where(np.isfinite(rgb), rgb, 0.0)
    cols = {"R": finite[..., 0], "G": finite[..., 1], "B": finite[..., 2], "max": finite.max(axis=2)}
    return {k: {"peak": float(v.max()), "mean": float(v.mean()),
                "over1": float((v > 1.0).mean() * 100), "over2": float((v > 2.0).mean() * 100)}
            for k, v in cols.items()}


def stats_table(s: dict[str, dict[str, float]]) -> str:
    rows = ["       peak    mean   >1.0 %  >2.0 %"]
    rows += [f"{k:<4}{v['peak']:8.3f}{v['mean']:8.3f}{v['over1']:9.2f}{v['over2']:8.2f}" for k, v in s.items()]
    return "\n".join(rows)


def stats_line(s: dict[str, dict[str, float]]) -> str:
    m = s["max"]
    return f"HDR peak {m['peak']:.2f}, mean {m['mean']:.3f}, >1.0 {m['over1']:.1f}%, >2.0 {m['over2']:.1f}% (max channel)"


@functools.lru_cache(maxsize=256)
def _cached_line(path: str, mtime: float) -> str:
    return stats_line(stats(read_exr(Path(path))[0]))


def exr_for(image: Path) -> Path | None:
    """The EXR captured with ``image`` (same stem), if there is one."""
    exr = Path(image).with_suffix(".exr")
    return exr if exr.exists() else None


def summary_line(image: Path) -> str | None:
    """Stats line for the EXR next to ``image``; None for an SDR capture."""
    exr = exr_for(image)
    return _cached_line(str(exr), exr.stat().st_mtime) if exr else None


def _shoulder(c: np.ndarray, knee: float) -> np.ndarray:
    if knee >= 0.999:
        return np.minimum(c, 1.0)
    r = 1.0 - knee
    s = knee + r * (1.0 - np.exp(-np.maximum(c - knee, 0.0) / r))
    return np.where(c >= knee, s, c)


def _smoothstep(e0: float, e1: float, x: np.ndarray) -> np.ndarray:
    t = np.clip((x - e0) / max(e1 - e0, 1e-4), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def _toon(c: np.ndarray, knee: float, rng: float, white_start: float = 2.0,
          white_full: float = 3.5, halo_start: float = 1.2, halo_full: float = 1.8) -> np.ndarray:
    """``tonemap_toon`` of post_processing.frag, line for line."""
    hot = np.minimum(c, 1.0)
    mx = c.max(axis=-1)
    hw = np.maximum(_smoothstep(white_start, max(white_full, white_start + 1e-4), mx),
                    _smoothstep(halo_start, max(halo_full, halo_start + 1e-4), mx))[..., None]
    lum = c @ LUMA
    a = max(1.0 - knee, 1e-4)
    t = np.maximum(lum - knee, 0.0) / a
    w = max(1.0 + rng - knee, 1e-4) / a
    lt = np.where(lum > knee, np.minimum(knee + a * t * (1.0 + t / (w * w)) / (1.0 + t), 1.0), lum)
    c = c * np.where(lum > 1e-6, lt / np.maximum(lum, 1e-6), 1.0)[..., None]
    m = c.max(axis=-1)
    f = np.maximum((1.0 - lt) / np.maximum(m - lt, 1e-6), 0.75)
    d = lt[..., None] + (c - lt[..., None]) * f[..., None]
    d = d / np.maximum(1.0, d.max(axis=-1))[..., None]
    c = np.where((m > 1.0)[..., None], d, c)
    return c + (hot - c) * hw


def tonemap(rgb: np.ndarray, curve: str, knee: float = 0.8, exposure: float = 1.0,
            toon_knee: float = 0.95, toon_range: float = 1.5, white_start: float = 2.0,
            white_full: float = 3.5, halo_start: float = 1.2, halo_full: float = 1.8) -> np.ndarray:
    """Map scene values to 0..1, as post_processing.frag does.

    clamp: the game's clamp mode (exposure unused).
    filmic: the game's filmic mode, per channel: identity below ``knee``, exponential shoulder.
    toon: the game's toon mode: luminance identity below ``toon_knee``, extended-Reinhard
    shoulder reaching 1.0 at 1 + ``toon_range``, RGB scaled by the luminance ratio; a channel
    still above 1 is desaturated toward the luminance (at most 25%), then divided by the max
    channel. Hot cores then blend toward the per-channel clip ``min(c, 1)`` (vanilla's result:
    white for blue eco, yellow for fire) by a smoothstep of the max channel from
    ``white_start`` to ``white_full``, and so does the halo around them from ``halo_start`` to
    ``halo_full`` (1.2 to 1.8), so a blue eco halo turns cyan and a green vent lime, as vanilla's
    clamp makes them; below ``halo_start`` the output is the hue-kept colour."""
    c = np.nan_to_num(rgb, nan=0.0, posinf=65504.0, neginf=0.0)
    if curve == "clamp":
        return np.clip(c, 0.0, 1.0)
    c = np.maximum(c * exposure, 0.0)
    if curve == "filmic":
        return _shoulder(c, knee)
    if curve == "toon":
        return _toon(c, toon_knee, toon_range, white_start, white_full, halo_start, halo_full)
    raise ValueError(f"unknown curve {curve!r}; pick one of {CURVES}")


def to_png(path: Path, ldr: np.ndarray) -> None:
    from PIL import Image

    Image.fromarray((ldr * 255.0 + 0.5).astype(np.uint8), "RGB").save(path)


def core_rgb(img: np.ndarray, rect: tuple[int, int, int, int], share: float = 0.01) -> list[int]:
    """Mean 8-bit RGB of the brightest ``share`` of pixels (Rec. 709 luma) in ``rect`` (x, y, w, h).

    ``img`` is 0..255 (a PNG) or 0..1 (a tonemapped EXR); the result is always 0..255."""
    x, y, w, h = rect
    win = img[y:y + h, x:x + w].reshape(-1, 3).astype(np.float64)
    if img.dtype != np.uint8:
        win = np.floor(win * 255.0 + 0.5)
    k = max(1, int(len(win) * share))
    return [int(v) for v in win[np.argsort(win @ LUMA)[-k:]].mean(axis=0).round()]


def _rect(text: str) -> tuple[int, int, int, int]:
    x, y, w, h = (int(v) for v in text.split(","))
    return x, y, w, h


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="sandover-hdr", description=__doc__.splitlines()[0])
    curve = argparse.ArgumentParser(add_help=False)
    curve.add_argument("--curve", choices=CURVES, default="clamp")
    curve.add_argument("--knee", type=float, default=0.8, help="filmic knee")
    curve.add_argument("--toon-knee", type=float, default=0.95)
    curve.add_argument("--toon-range", type=float, default=1.5)
    curve.add_argument("--white-start", type=float, default=2.0,
                       help="toon: max channel where the core starts to whiten")
    curve.add_argument("--white-full", type=float, default=3.5,
                       help="toon: max channel where the core is the per-channel clip")
    curve.add_argument("--halo-start", type=float, default=1.2,
                       help="toon: max channel where a halo starts the per-channel clip")
    curve.add_argument("--halo-full", type=float, default=1.8,
                       help="toon: max channel where that halo is the per-channel clip")
    curve.add_argument("--exposure", type=float, default=1.0)
    sub = ap.add_subparsers(dest="cmd", required=True)
    st = sub.add_parser("stats", parents=[curve],
                        help="range per channel; optionally a tonemapped preview PNG")
    st.add_argument("exr", type=Path)
    st.add_argument("--json", action="store_true", help="print JSON instead of the table")
    st.add_argument("--preview", type=Path, help="write a tonemapped PNG here")
    co = sub.add_parser("core", parents=[curve],
                        help="predicted core RGB (brightest 1%%) per window under a curve")
    co.add_argument("exr", type=Path)
    co.add_argument("--rect", type=_rect, action="append", required=True, metavar="X,Y,W,H")
    co.add_argument("--vs", type=Path, action="append", default=[],
                    help="also measure this PNG (e.g. the shot or a vanilla re-shoot)")
    args = ap.parse_args(argv)

    rgb, attrs = read_exr(args.exr)

    def ldr() -> np.ndarray:
        return tonemap(rgb, args.curve, args.knee, args.exposure, args.toon_knee,
                       args.toon_range, args.white_start, args.white_full, args.halo_start,
                       args.halo_full)

    if args.cmd == "core":
        from PIL import Image

        out = ldr()
        others = [(f"{p.parent.name}/{p.name}", np.asarray(Image.open(p).convert("RGB"))) for p in args.vs]
        for r in args.rect:
            row = f"{','.join(map(str, r))}: {args.curve} {core_rgb(out, r)}"
            row += "".join(f"  {n} {core_rgb(im, r)}" for n, im in others)
            print(row)
        return 0
    s = stats(rgb)
    if args.json:
        print(json.dumps({"size": [rgb.shape[1], rgb.shape[0]], "attrs": attrs, "stats": s}, indent=2))
    else:
        tm = " ".join(f"{k.removeprefix('sandover')}={v}" for k, v in attrs.items())
        print(f"{args.exr.name} {rgb.shape[1]}x{rgb.shape[0]} {tm}".rstrip())
        print(stats_table(s))
    if args.preview:
        to_png(args.preview, ldr())
        knee = (f"toon knee {args.toon_knee}, range {args.toon_range}, "
                f"white {args.white_start}..{args.white_full}, "
                f"halo {args.halo_start}..{args.halo_full}" if args.curve == "toon"
                else f"knee {args.knee}")
        print(f"wrote {args.preview} ({args.curve}, {knee}, exposure {args.exposure})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
