"""Round directory schema shared by sheets.py and report.py.

A round is a folder with a ``manifest.json`` (candidates x shots, written by whatever
proposes the round) and, once a human has picked, a ``result.json`` (written when a candidate is
picked). The manifest names the phase, round number, shots, one candidate per letter A-E
(``preset``, ``role`` anchor or candidate, ``label``) and per letter and shot an image path
relative to the folder; ``_validate`` below enforces the schema.
"""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass, field
from datetime import UTC, datetime
from pathlib import Path

SCHEMA = 1
LETTERS = "ABCDE"


@dataclass
class Candidate:
    preset: str
    role: str  # "anchor" or "candidate"
    label: str


@dataclass
class RoundManifest:
    """One round: a shot list and, per letter, a candidate and its image per shot."""

    dir: Path
    schema: int
    phase: str
    round: int
    pick_set: str
    shots: list[str]
    candidates: dict[str, Candidate]
    images: dict[str, dict[str, str]]  # letter -> shot -> path relative to dir
    vanilla: dict[str, str] = field(default_factory=dict)  # shot -> path relative to dir

    @property
    def anchor_letter(self) -> str | None:
        return next((l for l, c in self.candidates.items() if c.role == "anchor"), None)

    def image_path(self, letter: str, shot: str) -> Path:
        return self.dir / self.images[letter][shot]

    def vanilla_path(self, shot: str) -> Path:
        return self.dir / self.vanilla[shot]

    @classmethod
    def load(cls, round_dir: Path) -> "RoundManifest":
        data = json.loads((round_dir / "manifest.json").read_text(encoding="utf-8"))
        if data.get("schema") != SCHEMA:
            raise ValueError(f"unsupported round schema {data.get('schema')!r}")
        _validate(data)
        candidates = {k: Candidate(**v) for k, v in data["candidates"].items()}
        return cls(
            dir=round_dir,
            schema=data["schema"],
            phase=data["phase"],
            round=data["round"],
            pick_set=data.get("pick_set", ""),
            shots=list(data["shots"]),
            candidates=candidates,
            images=data["images"],
            vanilla=data.get("vanilla", {}),
        )


def _validate(data: dict) -> None:
    """Reject a manifest that could not be shown faithfully; raises ValueError."""
    for key in ("phase", "round", "shots", "candidates", "images"):
        if key not in data:
            raise ValueError(f"manifest is missing {key!r}")
    shots, cands, images = data["shots"], data["candidates"], data["images"]
    if not isinstance(data["round"], int) or isinstance(data["round"], bool):
        raise ValueError("round must be an integer")
    if not isinstance(shots, list) or not shots or len(set(shots)) != len(shots):
        raise ValueError("shots must be a non-empty list of unique names")
    if not isinstance(cands, dict) or not cands:
        raise ValueError("candidates must be a non-empty object")
    for letter, c in cands.items():
        if len(letter) != 1 or letter not in LETTERS:
            raise ValueError(f"candidate key {letter!r} is not one of {LETTERS}")
        if not isinstance(c, dict) or set(c) != {"preset", "role", "label"}:
            raise ValueError(f"candidate {letter} needs exactly preset, role, label")
        if c["role"] not in ("anchor", "candidate"):
            raise ValueError(f"candidate {letter} has unknown role {c['role']!r}")
    if sum(c["role"] == "anchor" for c in cands.values()) != 1:
        raise ValueError("exactly one candidate must have role 'anchor'")
    if not isinstance(images, dict) or set(images) != set(cands):
        raise ValueError("images must cover exactly the candidate letters")
    for letter, per_shot in images.items():
        if not isinstance(per_shot, dict) or set(per_shot) != set(shots):
            raise ValueError(f"images[{letter}] must have one entry per shot")
    vanilla = data.get("vanilla", {})
    if not isinstance(vanilla, dict) or not set(vanilla) <= set(shots):
        raise ValueError("vanilla keys must be shot names")
    rel = [p for per_shot in images.values() for p in per_shot.values()] + list(vanilla.values())
    for path in rel:
        if not isinstance(path, str) or Path(path).is_absolute() or ".." in Path(path).parts:
            raise ValueError(f"image path {path!r} must be relative and inside the round")


@dataclass
class RoundResult:
    schema: int
    round: int
    picked: str
    more_like_this: list[str]
    broken: dict[str, str]
    generator: str
    timestamp: str


def write_result(
    manifest: RoundManifest,
    picked: str,
    more_like_this: list[str],
    broken: dict[str, str],
    generator: str = "human-pick",
) -> Path:
    """Validate a pick against the manifest and write ``result.json`` next to it."""
    if picked not in manifest.candidates:
        raise ValueError(f"picked {picked!r} is not a candidate letter in this round")
    for letter in more_like_this:
        if letter not in manifest.candidates:
            raise ValueError(f"more_like_this letter {letter!r} is not in this round")
        if letter == picked:
            raise ValueError("more_like_this cannot include the pick itself")
    for shot in broken:
        if shot not in manifest.shots:
            raise ValueError(f"broken shot {shot!r} is not in this round's shot list")

    result = RoundResult(
        schema=SCHEMA,
        round=manifest.round,
        picked=picked,
        more_like_this=sorted(more_like_this),
        broken=dict(broken),
        generator=generator,
        timestamp=datetime.now(UTC).isoformat(),
    )
    out_path = manifest.dir / "result.json"
    out_path.write_text(json.dumps(asdict(result), indent=2) + "\n", encoding="utf-8")
    return out_path


def read_result(round_dir: Path) -> dict | None:
    path = round_dir / "result.json"
    if not path.exists():
        return None
    return json.loads(path.read_text(encoding="utf-8"))
