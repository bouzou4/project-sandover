"""Review-queue CSV logic: pure functions, unit-tested without gradio or a display.

Shared by the reviewer's main queue (``sandover.materials.review``) and the Browse tab
(``sandover.materials.browse``), which jumps the queue to a tile a human clicked.
"""

from __future__ import annotations

import os
from datetime import datetime, timezone
from pathlib import Path

from sandover.materials import CLASSES, TAGS
from sandover.materials.sheets import read_csv, write_csv

FIELDS = ["slot", "index", "combo_id", "tpage", "name", "judged_class", "confidence",
          "judge_agreement", "human_class", "tags", "human_note", "timestamp"]


def load_queue(csv_path: Path) -> list[dict]:
    return read_csv(csv_path)


def write_queue(csv_path: Path, rows: list[dict]) -> None:
    tmp = csv_path.with_name(csv_path.name + ".tmp")
    write_csv(tmp, rows, FIELDS)
    os.replace(tmp, csv_path)


def changed_rows(old: list[dict], new: list[dict]) -> list[dict]:
    """Rows of ``new`` that differ from (or are missing in) ``old``, matched by ``slot``."""
    before = {r.get("slot"): r for r in old}
    return [r for r in new if before.get(r.get("slot")) != r]


def commit(csv_path: Path, rows: list[dict], changes: list[dict]) -> list[dict]:
    """Write ``changes`` into the queue on disk and return ``rows`` refreshed from it.

    The file is re-read first and only the changed rows (matched by ``slot``) are replaced, so a
    page holding an older copy of the queue (another tab, or one opened before other answers
    were saved) never overwrites answers it has not seen. A new row whose slot is already taken
    on disk by another texture is renumbered. The returned list keeps ``rows``' order, with any
    rows only on disk appended.
    """
    disk = load_queue(csv_path) if csv_path.is_file() else []
    where = {r.get("slot"): i for i, r in enumerate(disk)}
    renamed: dict[str, str] = {}
    for c in changes:
        c = dict(c)
        i = where.get(c.get("slot"))
        if i is not None and disk[i].get("combo_id") != c.get("combo_id"):
            new_slot = str(max((int(r["slot"]) for r in disk if str(r.get("slot", "")).isdigit()),
                               default=-1) + 1)
            renamed[c.get("slot")] = new_slot
            c["slot"], i = new_slot, None
        if i is None:
            where[c["slot"]] = len(disk)
            disk.append(c)
        else:
            disk[i] = c
    if changes:
        write_queue(csv_path, disk)
    fresh = {r.get("slot"): r for r in disk}
    out, seen = [], set()
    for r in rows:
        slot = renamed.get(r.get("slot"), r.get("slot")) if r in changes else r.get("slot")
        out.append(dict(fresh.get(slot, r)))
        seen.add(slot)
    return out + [dict(r) for r in disk if r.get("slot") not in seen]


def merge_needs_review(rows: list[dict], candidates: list[dict]) -> list[dict]:
    """Append candidates not already in the queue (matched by combo_id), numbering slots on."""
    have = {r["combo_id"] for r in rows}
    next_slot = max((int(r["slot"]) for r in rows if str(r.get("slot", "")).strip().isdigit()),
                    default=len(rows) - 1) + 1
    extra = []
    for c in candidates:
        if not c.get("combo_id") or c["combo_id"] in have:
            continue
        row = {k: c.get(k, "") for k in FIELDS}
        row["slot"] = str(next_slot)
        extra.append(row)
        have.add(c["combo_id"])
        next_slot += 1
    return rows + extra


def jump_to_texture(rows: list[dict], target: dict) -> tuple[list[dict], int]:
    """Review-queue (rows, pos) after jumping to ``target``, a materials.csv row.

    Reuses the same row if ``target`` (by combo_id) is already queued; otherwise appends it
    (`merge_needs_review`) and jumps to the new slot.
    """
    combo = target.get("combo_id", "")
    if combo:
        for i, r in enumerate(rows):
            if r.get("combo_id") == combo:
                return rows, i
    candidate = {
        "combo_id": combo, "tpage": target.get("tpage", ""), "name": target.get("name", ""),
        "judged_class": target.get("class", ""), "confidence": target.get("confidence", ""),
        "judge_agreement": target.get("judge_agreement", ""),
    }
    new_rows = merge_needs_review(rows, [candidate])
    return new_rows, len(new_rows) - 1


def clamp(pos: int, n: int) -> int:
    return max(0, min(pos, n - 1)) if n else 0


def first_unanswered(rows: list[dict], start: int = 0) -> int:
    for i in range(start, len(rows)):
        if not rows[i].get("human_class", "").strip():
            return i
    return max(0, len(rows) - 1)


def progress(rows: list[dict]) -> str:
    n = sum(1 for r in rows if r.get("human_class", "").strip())
    return f"{n}/{len(rows)}"


def record(rows: list[dict], pos: int, cls: str, note: str) -> list[dict]:
    """New rows with rows[pos] answered; pure (copies), so the caller decides when to write."""
    if not rows or not (0 <= pos < len(rows)):
        return rows
    new_rows = [dict(r) for r in rows]
    new_rows[pos]["human_class"] = cls
    new_rows[pos]["human_note"] = note or ""
    new_rows[pos]["timestamp"] = datetime.now(timezone.utc).isoformat(timespec="seconds")
    return new_rows


def save_note(rows: list[dict], pos: int, note: str) -> list[dict]:
    """Context only, no class: pure, leaves ``human_class`` untouched (it stays optional)."""
    if not rows or not (0 <= pos < len(rows)):
        return rows
    new_rows = [dict(r) for r in rows]
    new_rows[pos]["human_note"] = note or ""
    new_rows[pos]["timestamp"] = datetime.now(timezone.utc).isoformat(timespec="seconds")
    return new_rows


def row_tags(row: dict) -> set[str]:
    return {t for t in row.get("tags", "").split(";") if t}


def toggle_tags(rows: list[dict], pos: int, tag: str) -> list[dict]:
    """Pure: flips ``tag`` in rows[pos]["tags"] (semicolon-separated, stable order from TAGS)."""
    if not rows or not (0 <= pos < len(rows)) or tag not in TAGS:
        return rows
    new_rows = [dict(r) for r in rows]
    tags = row_tags(new_rows[pos])
    tags.symmetric_difference_update({tag})
    new_rows[pos]["tags"] = ";".join(t for t in TAGS if t in tags)
    return new_rows


def apply_action(rows: list[dict], pos: int, note: str, action: str) -> tuple[list[dict], int]:
    """One key/button press -> (new_rows, new_pos). ``rows is new_rows`` iff nothing changed."""
    if not rows:
        return rows, 0
    pos = clamp(pos, len(rows))
    if action == "back":
        return rows, max(0, pos - 1)
    if action == "skip":
        return rows, clamp(pos + 1, len(rows))
    if action == "note":
        return save_note(rows, pos, note), clamp(pos + 1, len(rows))
    if action.startswith("toggle:"):
        return toggle_tags(rows, pos, action.split(":", 1)[1]), pos
    cls = rows[pos]["judged_class"] if action == "accept" else action
    if cls not in CLASSES:
        return rows, pos
    new_rows = record(rows, pos, cls, note)
    return new_rows, clamp(pos + 1, len(new_rows))


def sort_queue_by_confidence(rows: list[dict]) -> list[dict]:
    """Least confident first; at equal confidence, judge disagreement (or single/none) before agree."""
    def key(r: dict) -> tuple[float, int]:
        try:
            conf = float(r.get("confidence") or 0.0)
        except ValueError:
            conf = 0.0
        return conf, 0 if r.get("judge_agreement", "") != "agree" else 1
    return sorted(rows, key=key)


def agreement_by_class(rows: list[dict]) -> dict[str, tuple[int, int]]:
    """(agree, answered) per judged class, over rows the maintainer actually answered."""
    counts = {c: [0, 0] for c in CLASSES}
    for r in rows:
        h = r.get("human_class", "").strip().lower()
        j = r.get("judged_class", "").strip().lower()
        if not h or j not in CLASSES:
            continue
        counts[j][1] += 1
        if h == j:
            counts[j][0] += 1
    return {k: (a, n) for k, (a, n) in counts.items()}


def agreement_markdown(rows: list[dict]) -> str:
    counts = agreement_by_class(rows)
    lines = ["| class | human agrees with judges | n |", "|---|---|---|"]
    for c in CLASSES:
        agree, n = counts[c]
        pct = f"{100 * agree / n:.0f}%" if n else "-"
        lines.append(f"| {c} | {pct} | {n} |")
    return "\n".join(lines)
