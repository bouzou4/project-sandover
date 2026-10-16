"""Critic-run folder: snapshot sidecars, triage writes, deletions, crops and ``triage.csv``.

A run folder (``$SANDOVER_DATA/runs/<YYYYMMDD-HHMM>-<slug>/``) holds ``NNNN.png`` plus
``NNNN.json`` per snapshot, written by the in-game snapshot key. Triage only ever changes
``status``, ``note``, ``tags`` and ``annotations`` in a sidecar; everything else the capture
side wrote is kept byte-for-byte in meaning. Nothing is unlinked: a deleted snapshot moves to
``<run>/deleted/`` and a removed annotation's crop moves to ``<run>/crops/removed/``.
See ``kb/design/critic-triage.md``.
"""

from __future__ import annotations

import csv
import io
import json
import os
import re
import shutil
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

FACETS = ("performance", "artistic-integrity", "remaster-fanatic", "game-developer")
FACET_LABELS = {
    "performance": "Performance",
    "artistic-integrity": "Artistic integrity",
    "remaster-fanatic": "Remaster fanatic",
    "game-developer": "Game developer",
}
STATUSES = ("kept", "not-for-review", "deleted")
CSV_NAME = "triage.csv"
CSV_COLUMNS = ("index", "annotation", "level", "tags", "note", "png", "crop")
CROP_MARGIN = 0.10
MAX_TEXT = 4000
_SIDECAR = re.compile(r"^(\d{4,})\.json$")


class TriageError(ValueError):
    """A request the run cannot honour (unknown snapshot, bad field value)."""


def stem(index: int) -> str:
    return f"{index:04d}"


def now_utc() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _replace(src: Path, dst: Path) -> None:
    # Windows refuses a replace while another process briefly holds the target open.
    for attempt in range(20):
        try:
            os.replace(src, dst)
            return
        except PermissionError:
            if attempt == 19:
                raise
            time.sleep(0.05)


def write_json_atomic(path: Path, data: dict) -> None:
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    _replace(tmp, path)


def write_text_atomic(path: Path, text: str) -> None:
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(text, encoding="utf-8", newline="")
    _replace(tmp, path)


def normalize(sidecar: dict) -> dict:
    """Fill the triage fields a fresh capture may lack, without touching anything else."""
    out = dict(sidecar)
    if out.get("status") not in STATUSES:
        out["status"] = "kept"
    note = out.get("note")
    out["note"] = note if isinstance(note, str) and note.strip() else None
    tags = out.get("tags")
    out["tags"] = [t for t in FACETS if isinstance(tags, list) and t in tags]
    anns = out.get("annotations")
    out["annotations"] = [a for a in anns if isinstance(a, dict)] if isinstance(anns, list) else []
    return out


def open_facets(snap: dict) -> list[str]:
    """Facets a snapshot carries, on itself or on any of its annotations, in facet order."""
    found = set(snap.get("tags") or [])
    found.update(a.get("facet") for a in snap.get("annotations") or [])
    return [f for f in FACETS if f in found]


def summarize(snaps: list[dict]) -> dict:
    counts = {s: 0 for s in STATUSES}
    facets = {f: 0 for f in FACETS}
    annotations = 0
    for s in snaps:
        counts[s["status"]] += 1
        if s["status"] == "kept":
            for f in open_facets(s):
                facets[f] += 1
            annotations += len(s["annotations"])
    return {"counts": counts, "facets": facets, "annotations": annotations}


def caption(summary: dict) -> str:
    """One line for the review dashboard card: open items by facet, then the kept count."""
    parts = " · ".join(f"{FACET_LABELS[f]} {summary['facets'][f]}" for f in FACETS)
    c = summary["counts"]
    return f"Open: {parts} ({c['kept']} kept, {c['not-for-review']} not for review)"


def crop_box(rect: list[float], size: tuple[int, int], margin: float = CROP_MARGIN) -> tuple[int, int, int, int]:
    """``rect`` [x, y, w, h] in native pixels grown by ``margin`` per side, clamped: (l, t, r, b)."""
    x, y, w, h = rect
    mx, my = w * margin, h * margin
    left = max(0, int(round(x - mx)))
    top = max(0, int(round(y - my)))
    right = min(size[0], int(round(x + w + mx)))
    bottom = min(size[1], int(round(y + h + my)))
    if right <= left or bottom <= top:
        raise TriageError("crop is outside the image")
    return left, top, right, bottom


def _clean_rect(rect, size: tuple[int, int] | None) -> list[int]:
    if not (isinstance(rect, (list, tuple)) and len(rect) == 4
            and all(isinstance(v, (int, float)) for v in rect)):
        raise TriageError("rect must be [x, y, w, h]")
    x, y, w, h = (float(v) for v in rect)
    if w < 0:
        x, w = x + w, -w
    if h < 0:
        y, h = y + h, -h
    if size:
        x2, y2 = min(float(size[0]), x + w), min(float(size[1]), y + h)
        x, y = max(0.0, x), max(0.0, y)
        w, h = x2 - x, y2 - y
    if w < 2 or h < 2:
        raise TriageError("crop is too small")
    return [int(round(x)), int(round(y)), int(round(w)), int(round(h))]


def _clean_text(v) -> str:
    if v is None:
        return ""
    if not isinstance(v, str):
        raise TriageError("text must be a string")
    return v.strip()[:MAX_TEXT]


def _clean_facet(v) -> str | None:
    if v in (None, ""):
        return None
    if v not in FACETS:
        raise TriageError(f"facet must be one of {', '.join(FACETS)}")
    return v


@dataclass
class Run:
    """One run folder. Keeps an undo stack and the set of sidecars backed up this session."""

    root: Path
    _backed_up: set[str] = field(default_factory=set)
    _undo: list[tuple] = field(default_factory=list)

    def __post_init__(self) -> None:
        self.root = Path(self.root)
        if not self.root.is_dir():
            raise TriageError(f"not a folder: {self.root}")

    @property
    def deleted_dir(self) -> Path:
        return self.root / "deleted"

    @property
    def crops_dir(self) -> Path:
        return self.root / "crops"

    # ---- reading ----------------------------------------------------------------------

    def _sidecars(self) -> list[tuple[Path, bool]]:
        out = []
        for folder, deleted in ((self.root, False), (self.deleted_dir, True)):
            if folder.is_dir():
                out += [(p, deleted) for p in folder.iterdir() if _SIDECAR.match(p.name)]
        return out

    def _locate(self, index: int) -> tuple[Path, bool]:
        for folder, deleted in ((self.root, False), (self.deleted_dir, True)):
            p = folder / f"{stem(index)}.json"
            if p.is_file():
                return p, deleted
        raise TriageError(f"no snapshot #{stem(index)}")

    def _read(self, path: Path, deleted: bool) -> dict:
        snap = normalize(json.loads(path.read_text(encoding="utf-8")))
        snap["index"] = int(_SIDECAR.match(path.name).group(1))
        if deleted:
            snap["status"] = "deleted"
        return snap

    def snapshot(self, index: int) -> dict:
        return self._read(*self._locate(index))

    def snapshots(self) -> list[dict]:
        snaps = []
        for path, deleted in self._sidecars():
            try:
                snaps.append(self._read(path, deleted))
            except (OSError, ValueError):
                continue
        return sorted(snaps, key=lambda s: s["index"])

    def png_path(self, index: int) -> Path:
        path, _ = self._locate(index)
        return path.with_suffix(".png")

    def summary(self) -> dict:
        return summarize(self.snapshots())

    # ---- writing ----------------------------------------------------------------------

    def _write(self, path: Path, snap: dict) -> None:
        if path.name not in self._backed_up:
            shutil.copy2(path, path.with_name(path.name + ".bak"))
            self._backed_up.add(path.name)
        raw = json.loads(path.read_text(encoding="utf-8"))
        for key in ("status", "note", "tags", "annotations"):
            raw[key] = snap[key]
        write_json_atomic(path, raw)

    def update(self, index: int, fields: dict) -> dict:
        """Set ``status`` (kept / not-for-review), ``note`` and/or ``tags`` on a live snapshot."""
        path, deleted = self._locate(index)
        if deleted:
            raise TriageError(f"#{stem(index)} is deleted; restore it first")
        snap = self._read(path, deleted)
        if "status" in fields:
            if fields["status"] not in ("kept", "not-for-review"):
                raise TriageError("status must be kept or not-for-review (delete has its own call)")
            snap["status"] = fields["status"]
        if "note" in fields:
            snap["note"] = _clean_text(fields["note"]) or None
        if "tags" in fields:
            tags = fields["tags"]
            if not isinstance(tags, list) or any(t not in FACETS for t in tags):
                raise TriageError(f"tags must be a list of {', '.join(FACETS)}")
            snap["tags"] = [t for t in FACETS if t in tags]
        self._write(path, snap)
        self.export_csv()
        return self.snapshot(index)

    def _move(self, index: int, src: Path, dst: Path) -> None:
        dst.mkdir(parents=True, exist_ok=True)
        s = stem(index)
        for name in (f"{s}.png", f"{s}.exr", f"{s}.json.bak", f"{s}.json"):
            if (src / name).exists():
                if (dst / name).exists():
                    raise TriageError(f"{dst.name}/{name} already exists; not overwriting it")
                shutil.move(str(src / name), str(dst / name))

    def delete(self, index: int, record_undo: bool = True) -> dict:
        path, deleted = self._locate(index)
        if deleted:
            return self.snapshot(index)
        snap = self._read(path, deleted)
        previous = snap["status"]
        snap["status"] = "deleted"
        self._write(path, snap)
        self._move(index, self.root, self.deleted_dir)
        if record_undo:
            self._undo.append(("delete", index, previous))
        self.export_csv()
        return self.snapshot(index)

    def restore(self, index: int, status: str = "kept") -> dict:
        path, deleted = self._locate(index)
        if not deleted:
            return self.snapshot(index)
        self._move(index, self.deleted_dir, self.root)
        path = self.root / path.name
        snap = self._read(path, False)
        snap["status"] = status if status in ("kept", "not-for-review") else "kept"
        self._write(path, snap)
        self.export_csv()
        return self.snapshot(index)

    # ---- annotations ------------------------------------------------------------------

    def _image_size(self, index: int) -> tuple[int, int] | None:
        from PIL import Image

        try:
            with Image.open(self.png_path(index)) as im:
                return im.size
        except OSError:
            return None

    def _live(self, index: int) -> tuple[Path, dict]:
        path, deleted = self._locate(index)
        if deleted:
            raise TriageError(f"#{stem(index)} is deleted; restore it first")
        return path, self._read(path, deleted)

    def add_annotation(self, index: int, rect, text: str = "", facet: str | None = None) -> dict:
        path, snap = self._live(index)
        used = {a.get("id") for a in snap["annotations"]}
        n = 1
        while f"a{n}" in used:
            n += 1
        ann = {"id": f"a{n}", "rect": _clean_rect(rect, self._image_size(index)),
               "text": _clean_text(text), "facet": _clean_facet(facet), "created_utc": now_utc()}
        snap["annotations"].append(ann)
        self._write(path, snap)
        self.write_crop(index, ann)
        self.export_csv()
        return self.snapshot(index)

    def update_annotation(self, index: int, ann_id: str, fields: dict) -> dict:
        path, snap = self._live(index)
        ann = next((a for a in snap["annotations"] if a.get("id") == ann_id), None)
        if ann is None:
            raise TriageError(f"#{stem(index)} has no annotation {ann_id}")
        if "rect" in fields:
            ann["rect"] = _clean_rect(fields["rect"], self._image_size(index))
        if "text" in fields:
            ann["text"] = _clean_text(fields["text"])
        if "facet" in fields:
            ann["facet"] = _clean_facet(fields["facet"])
        self._write(path, snap)
        self.write_crop(index, ann)
        self.export_csv()
        return self.snapshot(index)

    def delete_annotation(self, index: int, ann_id: str) -> dict:
        path, snap = self._live(index)
        pos = next((i for i, a in enumerate(snap["annotations"]) if a.get("id") == ann_id), None)
        if pos is None:
            raise TriageError(f"#{stem(index)} has no annotation {ann_id}")
        ann = snap["annotations"].pop(pos)
        self._write(path, snap)
        crop = self.crops_dir / f"{stem(index)}-{ann_id}.png"
        if crop.is_file():
            removed = self.crops_dir / "removed"
            removed.mkdir(parents=True, exist_ok=True)
            _replace(crop, removed / f"{stem(index)}-{ann_id}-{int(time.time())}.png")
        self._undo.append(("annotation", index, pos, ann))
        self.export_csv()
        return self.snapshot(index)

    def write_crop(self, index: int, ann: dict) -> Path:
        from PIL import Image

        self.crops_dir.mkdir(parents=True, exist_ok=True)
        out = self.crops_dir / f"{stem(index)}-{ann['id']}.png"
        with Image.open(self.png_path(index)) as im:
            crop = im.crop(crop_box(ann["rect"], im.size))
            buf = io.BytesIO()
            crop.save(buf, format="PNG")
        tmp = out.with_name(out.name + ".tmp")
        tmp.write_bytes(buf.getvalue())
        _replace(tmp, out)
        return out

    # ---- undo -------------------------------------------------------------------------

    def undo(self) -> dict | None:
        """Revert the last delete or annotation delete; returns the snapshot it touched."""
        while self._undo:
            item = self._undo.pop()
            try:
                if item[0] == "delete":
                    return self.restore(item[1], item[2])
                _, index, pos, ann = item
                path, snap = self._live(index)
                if any(a.get("id") == ann["id"] for a in snap["annotations"]):
                    continue
                snap["annotations"].insert(min(pos, len(snap["annotations"])), ann)
                self._write(path, snap)
                self.write_crop(index, ann)
                self.export_csv()
                return self.snapshot(index)
            except TriageError:
                continue
        return None

    # ---- export -----------------------------------------------------------------------

    def csv_rows(self) -> list[dict]:
        rows = []
        for s in self.snapshots():
            if s["status"] != "kept":
                continue
            base = {"index": stem(s["index"]), "level": s.get("level") or "",
                    "png": f"{stem(s['index'])}.png"}
            rows.append({**base, "annotation": "", "tags": ";".join(s["tags"]),
                         "note": s["note"] or "", "crop": ""})
            for a in s["annotations"]:
                rows.append({**base, "annotation": a.get("id", ""), "tags": a.get("facet") or "",
                             "note": a.get("text") or "",
                             "crop": f"crops/{stem(s['index'])}-{a.get('id', '')}.png"})
        return rows

    def export_csv(self) -> Path:
        buf = io.StringIO()
        w = csv.DictWriter(buf, fieldnames=CSV_COLUMNS, lineterminator="\n")
        w.writeheader()
        w.writerows(self.csv_rows())
        out = self.root / CSV_NAME
        write_text_atomic(out, buf.getvalue())
        return out
