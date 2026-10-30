"""``emissive-overrides.csv``: human verdicts from the viewer's inspect panel.

Shared by the viewer's ``POST /api/mark`` (writer) and the reviewer's Browse tab / emissive
class query (readers), and by the in-game inspect tool on ``inspect-tool``, which also
writes here -- the columns below are the agreed contract across all three, do not rename
them. Append-only, like ``spotcheck.csv``: closing a page never loses a verdict. The last
row for a given key (``combo_id``, or ``name`` when ``combo_id`` is blank) wins.
"""

from __future__ import annotations

import csv
from datetime import datetime, timezone
from pathlib import Path

from sandover.materials.sheets import materials_dir, read_csv

FIELDS = ["combo_id", "tpage", "name", "level", "draw_id", "verdict", "note", "timestamp"]
VERDICTS = {"emissive", "not-emissive"}


def overrides_path() -> Path:
    return materials_dir() / "emissive-overrides.csv"


def override_key(row: dict) -> str:
    """The join key overrides, the emissive table and materials.csv rows all share."""
    return row.get("combo_id") or row.get("name", "")


def append_override(path: Path, row: dict) -> dict:
    """Append one verdict row; creates the file (with header) if absent.

    ``verdict`` must be ``"emissive"``, ``"not-emissive"`` or ``""`` (clears a prior verdict
    for this key). Anything else raises ``ValueError``. Returns the row actually written
    (every field coerced to a string, timestamp filled in server-side).
    """
    verdict = str(row.get("verdict", ""))
    if verdict and verdict not in VERDICTS:
        raise ValueError(f"bad verdict {verdict!r}, expected one of {sorted(VERDICTS)} or ''")
    out = {k: str(row.get(k, "") or "") for k in FIELDS}
    out["verdict"] = verdict
    out["timestamp"] = datetime.now(timezone.utc).isoformat(timespec="seconds")
    path.parent.mkdir(parents=True, exist_ok=True)
    is_new = not path.exists()
    with path.open("a", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        if is_new:
            w.writeheader()
        w.writerow(out)
    return out


def read_overrides(path: Path) -> list[dict]:
    if not path.is_file():
        return []
    return read_csv(path)


def latest_verdicts(rows: list[dict]) -> dict[str, str]:
    """key -> most recent non-empty verdict; a later empty (clear) row removes the key.

    Rows are taken in file order (append-only), so the last row per key wins.
    """
    out: dict[str, str] = {}
    for row in rows:
        key = override_key(row)
        if not key:
            continue
        verdict = row.get("verdict", "")
        if verdict:
            out[key] = verdict
        else:
            out.pop(key, None)
    return out
