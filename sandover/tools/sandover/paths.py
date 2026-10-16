"""Repository and data locations, from ``$SANDOVER_ROOT`` / ``$SANDOVER_DATA`` or the checkout."""

import os
from pathlib import Path

_TOOLS = Path(__file__).resolve().parents[1]


def root() -> Path:
    return Path(os.environ.get("SANDOVER_ROOT") or _TOOLS.parents[1])


def data_dir() -> Path:
    return Path(os.environ.get("SANDOVER_DATA") or root() / "sandover" / "data")
