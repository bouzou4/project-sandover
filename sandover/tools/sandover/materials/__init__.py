"""Material classification of ripped Jak 1 textures (class schema in `kb/howto/materials.md`).

Flow: ``sheets`` renders numbered contact sheets, the vision judge labels every sheet in
two independent passes, ``merge`` writes ``materials.csv``, ``load`` pushes the labels
into the FiftyOne dataset, ``spotcheck`` draws a stratified set for the maintainer, and
``apply`` folds human corrections back into the CSV. See ``kb/howto/materials.md``.

The taxonomy (classes and tags, with reviewer keys and prompt/legend descriptions) is
data-driven: loaded from ``sandover/presets/materials.toml``, the single source of truth
for the judge prompt, the reviewer's buttons/legend/keyboard shortcuts, CSV validation and
the FiftyOne fields. Add a class or tag by editing that file.
"""

from __future__ import annotations

import re
import tomllib
from dataclasses import dataclass
from pathlib import Path

from sandover.paths import root

_KEY_RE = re.compile(r"^[A-Za-z0-9]$")


@dataclass(frozen=True)
class Entry:
    name: str
    key: str
    description: str


def preset_path() -> Path:
    return root() / "sandover" / "presets" / "materials.toml"


def _entries(raw: list[dict], kind: str) -> tuple[Entry, ...]:
    out: list[Entry] = []
    seen: set[str] = set()
    for e in raw:
        name, key = str(e["name"]), str(e["key"])
        if not _KEY_RE.match(key):
            raise ValueError(f"materials.toml: {kind} {name!r} has an invalid key {key!r} "
                              "(must be exactly one letter or digit)")
        if name in seen:
            raise ValueError(f"materials.toml: duplicate {kind} name {name!r}")
        seen.add(name)
        out.append(Entry(name=name, key=key, description=str(e["description"])))
    return tuple(out)


def load_taxonomy(path: Path | None = None) -> tuple[tuple[Entry, ...], tuple[Entry, ...]]:
    """Parse and validate ``materials.toml``.

    Names are unique within classes and within tags; keys are unique case-insensitively
    across *both* lists together, since the reviewer draws its keyboard shortcuts from one
    shared keymap.
    """
    data = tomllib.loads((path or preset_path()).read_text(encoding="utf-8"))
    classes = _entries(data.get("classes", []), "class")
    tags = _entries(data.get("tags", []), "tag")
    keys = [e.key.lower() for e in (*classes, *tags)]
    if len(keys) != len(set(keys)):
        dupes = sorted({k for k in keys if keys.count(k) > 1})
        raise ValueError(f"materials.toml: duplicate key(s) {dupes} across classes/tags")
    return classes, tags


CLASS_LIST, TAG_LIST = load_taxonomy()

CLASSES: tuple[str, ...] = tuple(c.name for c in CLASS_LIST)
TAGS: tuple[str, ...] = tuple(t.name for t in TAG_LIST)
CLASS_KEYS: dict[str, str] = {c.name: c.key for c in CLASS_LIST}
TAG_KEYS: dict[str, str] = {t.name: t.key for t in TAG_LIST}
CLASS_DESCRIPTIONS: dict[str, str] = {c.name: c.description for c in CLASS_LIST}
TAG_DESCRIPTIONS: dict[str, str] = {t.name: t.description for t in TAG_LIST}
