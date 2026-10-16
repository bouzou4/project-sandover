"""Emissive verdicts folded into the sidecar's E.

``$SANDOVER_DATA/materials/emissive-overrides.csv`` is the shared, append-only verdict file
written by the in-game inspect tool (``kb/howto/inspect.md``) and the viewer's inspect panel
(``kb/howto/viewer.md``); its columns and key rules live in :mod:`sandover.materials.overrides`
(``combo_id, tpage, name, level, draw_id, verdict, note, timestamp``; the last row per
``combo_id``, or ``name`` when ``combo_id`` is blank, wins; an empty verdict clears).

A verdict is per texture: it applies to every vertex drawn with a texture of that name in every
level (the glb names materials by texture name, so a ``combo_id`` key resolves to the name on its
latest row). ``draw_id`` is context only and never narrows a verdict.

``emissive`` sets E to 1 and keeps the vertex's E source if it had one (a lamp baked into the
night slots stays night-weighted); a vertex with no E gets the static source (weight 1 at every
hour). ``not-emissive`` sets E and its source to 0. Nothing else in the record changes.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np

from sandover.materials.overrides import override_key, read_overrides

from .emissive import SRC_STATIC

E_MASK = np.uint32(0x0FFF0000)  # E (bits 16-23) and its source (24-27)


def load(path: Path) -> dict[str, str]:
    """Texture name -> verdict in force (``emissive`` or ``not-emissive``)."""
    last: dict[str, dict] = {}
    for row in read_overrides(path):
        key = override_key(row)
        if not key:
            continue
        if row.get("verdict"):
            last[key] = row
        else:
            last.pop(key, None)
    return {row["name"]: row["verdict"] for row in last.values() if row.get("name")}


def _force(w: np.ndarray, verts: np.ndarray, verdict: str) -> None:
    verts = verts[(verts >= 0) & (verts < len(w))]
    if verdict == "not-emissive":
        w[verts] &= ~E_MASK
        return
    src = (w[verts] >> np.uint32(24)) & np.uint32(0xF)
    had = ((w[verts] >> np.uint32(16)) & np.uint32(0xFF)) > 0
    src = np.where(had, src, np.uint32(SRC_STATIC)).astype(np.uint32)
    w[verts] = (w[verts] & ~E_MASK) | (np.uint32(255) << np.uint32(16)) | (src << np.uint32(24))


def texture_vertices(prims: list[tuple[str, np.ndarray]]) -> dict[str, np.ndarray]:
    """Texture name -> vertex ids, from a tree's (texture name, vertex ids) primitives."""
    out: dict[str, list[np.ndarray]] = {}
    for tex, idx in prims:
        out.setdefault(tex, []).append(idx)
    return {k: np.unique(np.concatenate(v)) for k, v in out.items()}


def apply(verdicts: dict[str, str], w: np.ndarray, tex_verts: dict[str, np.ndarray]) -> int:
    """Force E in the record words ``w`` (V,) of one tree in place; returns vertices touched.

    ``tex_verts`` maps each texture name the tree draws to its vertex ids.
    """
    touched = np.zeros(len(w), bool)
    for name, verts in tex_verts.items():
        verdict = verdicts.get(name)
        if verdict in ("emissive", "not-emissive"):
            _force(w, verts, verdict)
            touched[verts[(verts >= 0) & (verts < len(w))]] = True
    return int(touched.sum())
