"""The Sandover bridge's C++ source: kmachine.cpp plus the per-feature files beside it."""
from __future__ import annotations

from pathlib import Path

KERNEL = Path("game") / "kernel" / "common"


def bridge_files(root: Path) -> list[Path]:
    """kmachine.cpp, then sandover/bridge_*.cpp, then the registration table (bridge.cpp)."""
    d = root / KERNEL / "sandover"
    parts = sorted(p for p in d.glob("bridge_*.cpp"))
    return [root / KERNEL / "kmachine.cpp", *parts, d / "bridge.cpp"]


def bridge_text(root: Path) -> str:
    return "\n".join(p.read_text(encoding="utf-8") for p in bridge_files(root))
