"""Talking to a running ``sandover-viewer`` server: shared by the reviewer's Context panel
and Browse tab (``sandover.materials.review``, ``sandover.materials.browse``).
"""

from __future__ import annotations

import json
import urllib.error
import urllib.request
from urllib.parse import quote


def fetch_models_using(viewer_port: int, stem: str, timeout: float = 2.0) -> list[dict] | None:
    """The viewer's ``/api/models-using/<stem>``; ``None`` (not ``[]``) means unreachable."""
    url = f"http://127.0.0.1:{viewer_port}/api/models-using/{quote(stem, safe='')}"
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:  # noqa: S310 -- loopback only
            return json.loads(r.read().decode("utf-8"))
    except (urllib.error.URLError, OSError, ValueError, TimeoutError):
        return None


def viewer_url(viewer_port: int, model_id: str, stem: str) -> str:
    return (f"http://127.0.0.1:{viewer_port}/?model={quote(model_id, safe='')}"
            f"&highlight={quote(stem, safe='')}")
