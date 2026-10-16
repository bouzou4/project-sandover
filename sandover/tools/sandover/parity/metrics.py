"""Image metrics for the parity gate (ADR-0008).

SSIM and PSNR (scikit-image) are cheap pre-checks for gross corruption; FLIP LDR mean
(``flip-evaluator``) is the only pass/fail metric. Images are sRGB ``uint8`` HxWx3 arrays.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass
from pathlib import Path

import numpy as np
from PIL import Image

# pre-check limits: anything below is a harness error or corruption, not a subtle change
SSIM_MIN = 0.5
BLACK_MEAN = 2.0


def load(path: Path) -> np.ndarray:
    return np.asarray(Image.open(path).convert("RGB"))


def flip(ref: np.ndarray, test: np.ndarray) -> tuple[float, np.ndarray]:
    """FLIP LDR mean and the per-pixel error map (HxW float32 in [0, 1])."""
    import flip_evaluator

    a = ref.astype(np.float32) / 255.0
    b = test.astype(np.float32) / 255.0
    fmap, mean, _ = flip_evaluator.evaluate(a, b, "LDR", applyMagma=False)
    return float(mean), np.asarray(fmap, dtype=np.float32).reshape(ref.shape[:2])


@dataclass
class PreCheck:
    ok: bool
    reason: str
    ssim: float | None = None
    psnr: float | None = None
    test_mean: float | None = None

    def to_dict(self) -> dict:
        return asdict(self)


def precheck(ref: np.ndarray, test: np.ndarray) -> PreCheck:
    """Reject wrong size, black frames and channel swaps before paying for FLIP."""
    from skimage.metrics import peak_signal_noise_ratio, structural_similarity

    if ref.shape != test.shape:
        return PreCheck(False, f"size {test.shape[1]}x{test.shape[0]} != {ref.shape[1]}x{ref.shape[0]}")
    tm = float(test.mean())
    if tm < BLACK_MEAN <= float(ref.mean()):
        return PreCheck(False, "black frame", test_mean=tm)
    gr, gt = _luma(ref), _luma(test)
    ssim = float(structural_similarity(gr, gt, data_range=255.0))
    psnr = float(peak_signal_noise_ratio(ref, test, data_range=255)) if not np.array_equal(ref, test) else float("inf")
    pc = PreCheck(True, "", ssim, psnr, tm)
    if ssim < SSIM_MIN:
        pc.ok, pc.reason = False, f"SSIM {ssim:.3f} < {SSIM_MIN}"
    elif _swapped(ref, test):
        pc.ok, pc.reason = False, "colour channels swapped"
    return pc


def _luma(img: np.ndarray) -> np.ndarray:
    return img.astype(np.float32) @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def _swapped(ref: np.ndarray, test: np.ndarray) -> bool:
    """True if some channel permutation of ``test`` matches ``ref`` far better than as is."""
    r = ref.reshape(-1, 3)[::97].astype(np.float32)
    t = test.reshape(-1, 3)[::97].astype(np.float32)
    err = np.abs(r - t).mean()
    best = min(np.abs(r - t[:, p]).mean() for p in ([2, 1, 0], [1, 0, 2], [0, 2, 1], [1, 2, 0], [2, 0, 1]))
    return err > 4.0 and best < 0.5 * err


def shift(img: np.ndarray, dx: int = 1, dy: int = 0) -> np.ndarray:
    """Translate by whole pixels, repeating the edge."""
    out = np.roll(img, (dy, dx), axis=(0, 1))
    if dx > 0:
        out[:, :dx] = img[:, :1]
    elif dx < 0:
        out[:, dx:] = img[:, -1:]
    if dy > 0:
        out[:dy] = img[:1]
    elif dy < 0:
        out[dy:] = img[-1:]
    return out


def gamma_nudge(img: np.ndarray, levels: float = 1.0) -> np.ndarray:
    """Gamma change that moves mid-grey (128) by ``levels`` 8-bit steps (darker if positive)."""
    g = np.log((128.0 - levels) / 255.0) / np.log(128.0 / 255.0)
    lut = np.clip(np.round(255.0 * (np.arange(256) / 255.0) ** g), 0, 255).astype(np.uint8)
    return lut[img]


def offset(img: np.ndarray, levels: int = 1) -> np.ndarray:
    return np.clip(img.astype(np.int16) + levels, 0, 255).astype(np.uint8)


PERTURBATIONS = {
    "shift-1px-x": lambda im: shift(im, 1, 0),
    "shift-1px-y": lambda im: shift(im, 0, 1),
    "gamma-1-level": lambda im: gamma_nudge(im, 1.0),
    "offset+1": lambda im: offset(im, 1),
}


def save_error_map(fmap: np.ndarray, path: Path) -> None:
    """Error map as a magma-coloured PNG (FLIP's convention; grey without matplotlib)."""
    try:
        from matplotlib import colormaps

        rgb = (colormaps["magma"](np.clip(fmap, 0, 1))[..., :3] * 255).astype(np.uint8)
    except ImportError:
        rgb = np.repeat((np.clip(fmap, 0, 1) * 255).astype(np.uint8)[..., None], 3, -1)
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(rgb).save(path, compress_level=3)
