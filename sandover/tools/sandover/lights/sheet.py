"""Top-down light sheets: every light of a level over its tfrag footprint, coloured by kind."""

from __future__ import annotations

from pathlib import Path

import numpy as np

KIND_STYLE = {  # colour, marker
    "static": ("#c050d0", "s"),
    "particle": ("#20a0e0", "^"),
    "entity": ("#f08020", "o"),
    "vanilla-light": ("#30c040", "*"),
    "prop": ("#80c8ff", "D"),
}


def footprint(glb: Path, n: int = 60000) -> np.ndarray | None:
    """(N, 3) subsample of the level's tfrag vertex positions (metres)."""
    import pygltflib

    from sandover.unbake.glb import SKIP_TREES, _read

    if not glb.is_file():
        return None
    g = pygltflib.GLTF2().load(str(glb))
    blob = g.binary_blob()
    seen, pts = set(), []
    for root_idx in g.scenes[g.scene or 0].nodes:
        root = g.nodes[root_idx]
        if "-tfrag-" not in (root.name or "") or any(s in root.name for s in SKIP_TREES):
            continue
        for c in root.children:
            node = g.nodes[c]
            if node.mesh is None:
                continue
            for p in g.meshes[node.mesh].primitives:
                if p.attributes.POSITION in seen:
                    continue
                seen.add(p.attributes.POSITION)
                pts.append(_read(g, blob, p.attributes.POSITION))
    if not pts:
        return None
    a = np.concatenate(pts)
    rng = np.random.default_rng(0)
    return a[rng.choice(len(a), min(n, len(a)), replace=False)]


def draw(ax, recs: list[dict], fp: np.ndarray | None, title: str, legend: bool = True) -> None:
    if fp is not None:
        ax.scatter(fp[:, 0], fp[:, 2], s=0.2, c="#8a8a8a", alpha=0.5, linewidths=0, rasterized=True)
    for kind, (col, mk) in KIND_STYLE.items():
        rs = [r for r in recs if r["kind"] == kind]
        if not rs:
            continue
        xy = np.array([[r["pos"][0], r["pos"][2]] for r in rs])
        ax.scatter(xy[:, 0], xy[:, 1], s=14 if kind != "static" else 8, c=col, marker=mk,
                   edgecolors="black", linewidths=0.3, label=f"{kind} ({len(rs)})", zorder=3)
    ax.set_aspect("equal", adjustable="datalim")
    ax.set_title(title, fontsize=10)
    ax.set_xlabel("x (m)", fontsize=8)
    ax.set_ylabel("z (m)", fontsize=8)
    ax.tick_params(labelsize=7)
    if legend:
        ax.legend(fontsize=7, loc="best", framealpha=0.8)


def write_sheets(out: Path, per_level: dict[str, list[dict]], fps: dict[str, np.ndarray | None]) -> list[Path]:
    """``<out>/<level>.png`` per level and ``00-overview.png`` (all levels, small)."""
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    out.mkdir(parents=True, exist_ok=True)
    written = []
    for lv, recs in per_level.items():
        fig, ax = plt.subplots(figsize=(9, 9), dpi=110)
        draw(ax, recs, fps.get(lv), f"{lv}: {len(recs)} lights (top-down, grey = tfrag)")
        fig.tight_layout()
        p = out / f"{lv}.png"
        fig.savefig(p)
        plt.close(fig)
        written.append(p)
    n = len(per_level)
    cols = min(5, n)
    rows = (n + cols - 1) // cols
    fig, axes = plt.subplots(rows, cols, figsize=(4 * cols, 4 * rows), dpi=90, squeeze=False)
    for ax, (lv, recs) in zip(axes.ravel(), per_level.items()):
        draw(ax, recs, fps.get(lv), f"{lv} ({len(recs)})", legend=False)
        ax.set_xlabel("")
        ax.set_ylabel("")
    for ax in axes.ravel()[n:]:
        ax.axis("off")
    handles = [plt.Line2D([], [], color=c, marker=m, ls="", label=k) for k, (c, m) in KIND_STYLE.items()]
    fig.legend(handles=handles, loc="lower right", fontsize=9)
    fig.tight_layout()
    p = out / "00-overview.png"
    fig.savefig(p)
    plt.close(fig)
    return [p] + written
