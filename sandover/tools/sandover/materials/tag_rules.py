"""Name rules for material tags: ``[tag_rules.<tag>]`` in ``sandover/presets/materials.toml``.

A rule lists texture-name globs (``fnmatch``, case-sensitive). The tag is a property of the
texture's art, not of one texture page, so it is set on every ``materials.csv`` row whose name
matches, in every tpage that holds a copy, and cleared on every row that does not: the rule is
the whole truth for its tag. ``sandover-materials tag-rules`` applies the rules (``merge``
applies them too, since a re-merge clears the tags of rows nobody reviewed).
"""

from __future__ import annotations

import fnmatch
import shutil
import tomllib
from datetime import datetime
from pathlib import Path

from sandover.materials import TAGS, preset_path


def load_rules(path: Path | None = None) -> dict[str, tuple[str, ...]]:
    """tag -> name globs, from ``[tag_rules.<tag>] names``. Unknown tags are rejected."""
    data = tomllib.loads((path or preset_path()).read_text(encoding="utf-8"))
    rules: dict[str, tuple[str, ...]] = {}
    for tag, body in data.get("tag_rules", {}).items():
        if tag not in TAGS:
            raise ValueError(f"materials.toml: tag_rules.{tag} names no tag in [[tags]]")
        names = tuple(str(n) for n in body.get("names", []))
        if not names:
            raise ValueError(f"materials.toml: tag_rules.{tag} has no names")
        rules[tag] = names
    return rules


def matches(name: str, globs: tuple[str, ...]) -> bool:
    return any(fnmatch.fnmatchcase(name, g) for g in globs)


def _tags(row: dict) -> list[str]:
    return [t for t in (row.get("tags") or "").split(";") if t]


def apply(rows: list[dict], rules: dict[str, tuple[str, ...]]) -> dict[str, tuple[list[dict], list[dict]]]:
    """Set each ruled tag on the matching rows and clear it elsewhere, in place.
    Returns tag -> (rows that gained it, rows that lost it)."""
    changes: dict[str, tuple[list[dict], list[dict]]] = {}
    for tag, globs in rules.items():
        added, removed = [], []
        for r in rows:
            tags = _tags(r)
            want = matches(r["name"], globs)
            if want and tag not in tags:
                tags.append(tag)
                added.append(r)
            elif not want and tag in tags:
                tags.remove(tag)
                removed.append(r)
            else:
                continue
            r["tags"] = ";".join(tags)
        changes[tag] = (added, removed)
    return changes


def unmatched_globs(rows: list[dict], rules: dict[str, tuple[str, ...]]) -> list[tuple[str, str]]:
    """(tag, glob) pairs that match no row: a typo or a texture that is not in the rip."""
    names = [r["name"] for r in rows]
    return [(tag, g) for tag, globs in rules.items() for g in globs
            if not any(fnmatch.fnmatchcase(n, g) for n in names)]


def run(csv: Path, dry_run: bool = False) -> int:
    from sandover.materials.sheets import read_csv, write_csv
    from sandover.materials.table import FIELDS

    rows = read_csv(csv)
    rules = load_rules()
    changes = apply(rows, rules)
    for tag, glob in unmatched_globs(rows, rules):
        print(f"warning: tag_rules.{tag} glob {glob!r} matches no texture")
    for tag, (added, removed) in changes.items():
        n = sum(tag in _tags(r) for r in rows)
        print(f"{tag}: {n} rows tagged, +{len(added)} -{len(removed)}")
        for sign, rs in (("+", added), ("-", removed)):
            for r in rs:
                print(f"  {sign} {r['combo_id']} {r['tpage']} {r['name']}")
    if dry_run or not any(a or r for a, r in changes.values()):
        return 0
    backup = csv.parent / "backup" / f"{csv.stem}.before-tag-rules-{datetime.now():%Y%m%d-%H%M%S}.csv"
    backup.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(csv, backup)
    write_csv(csv, rows, FIELDS)
    print(f"wrote {csv} (backup {backup.name})")
    return 0
