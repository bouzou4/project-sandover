"""``sandover-triage <run_dir>``: review a snapshot run's snapshots in the browser.

A stdlib HTTP server on loopback serving one static page (``static/``) and a small JSON API
over :class:`sandover.triage.store.Run`. No CDN, no telemetry. Every change is written at
once; when the optional review dashboard is installed, its card caption follows the open-item counts.
See ``kb/design/critic-triage.md``.
"""

from __future__ import annotations

import argparse
import io
import json
import mimetypes
import re
import sys
import threading
from functools import lru_cache
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from sandover.paths import data_dir
from sandover.triage.store import FACET_LABELS, FACETS, Run, TriageError, caption

STATIC_DIR = Path(__file__).parent / "static"
DEFAULT_PORT = 7895
HUB_FEATURE = "critic-run"
THUMB_WIDTH = 320
MAX_BODY = 256 * 1024

_SNAP = re.compile(r"^/api/snap/(\d+)(?:/(delete|restore|annotations)(?:/(a\d+)(/delete)?)?)?$")
_IMG = re.compile(r"^/(img|thumb)/(\d+)\.(png|jpg)$")


@lru_cache(maxsize=2048)
def _thumb(path: str, mtime: float) -> bytes:
    from PIL import Image

    with Image.open(path) as im:
        im = im.convert("RGB")
        im.thumbnail((THUMB_WIDTH, THUMB_WIDTH), Image.Resampling.LANCZOS)
        buf = io.BytesIO()
        im.save(buf, format="JPEG", quality=85)
    return buf.getvalue()


def hub_rel(run_root: Path, data: Path) -> str | None:
    try:
        return run_root.resolve().relative_to(data.resolve()).as_posix()
    except ValueError:
        return None


def sync_hub_caption(run: Run, text: str, data: Path | None = None) -> bool:
    """Rewrite the optional review dashboard's critic-run card caption if the card points at this run. True if changed."""
    try:
        from sandover import hub
    except ImportError:
        return False

    data = data or data_dir()
    rel = hub_rel(run.root, data)
    manifest = data / "hub" / "features.toml"
    if rel is None or not manifest.is_file():
        return False
    features = hub.load(manifest)
    for f in features:
        if f.id == HUB_FEATURE and f.now.round.rstrip("/") == rel:
            if f.now.caption == text:
                return False
            f.now.caption = text
            hub.save(manifest, features)
            return True
    return False


class TriageApp:
    """Shared state for the request handlers: the run, a write lock, the dashboard sync."""

    def __init__(self, run: Run, hub: bool = True):
        self.run = run
        self.hub = hub
        self.lock = threading.Lock()
        self._caption = None

    def payload(self, snap: dict | None = None) -> dict:
        summary = self.run.summary()
        if self.hub:
            text = caption(summary)
            if text != self._caption:
                try:
                    sync_hub_caption(self.run, text)
                    self._caption = text
                except (OSError, ValueError) as exc:
                    print(f"triage: dashboard caption not updated: {exc}", file=sys.stderr)
        out = {"summary": summary}
        if snap is not None:
            out["snapshot"] = snap
        return out

    def run_payload(self) -> dict:
        return {"name": self.run.root.name, "facets": list(FACETS), "facet_labels": FACET_LABELS,
                "snapshots": self.run.snapshots(), **self.payload()}


class Handler(BaseHTTPRequestHandler):
    app: TriageApp

    def log_message(self, *args):
        pass

    def _send(self, code: int, body: bytes, ctype: str, cache: str = "no-store") -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", cache)
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, code: int, obj) -> None:
        self._send(code, json.dumps(obj).encode("utf-8"), "application/json")

    def _error(self, code: int, msg: str) -> None:
        self._json(code, {"error": msg})

    def do_GET(self):  # noqa: N802
        path = self.path.split("?", 1)[0]
        if path in ("/", "/index.html"):
            return self._static("index.html")
        if path.startswith("/static/"):
            return self._static(path[len("/static/"):])
        if path == "/api/run":
            with self.app.lock:
                return self._json(200, self.app.run_payload())
        m = _IMG.match(path)
        if m:
            kind, idx = m.group(1), int(m.group(2))
            try:
                png = self.app.run.png_path(idx)
            except TriageError as exc:
                return self._error(404, str(exc))
            if not png.is_file():
                return self._error(404, f"image missing: {png.name}")
            if kind == "img":
                return self._send(200, png.read_bytes(), "image/png", "no-cache")
            return self._send(200, _thumb(str(png), png.stat().st_mtime), "image/jpeg", "no-cache")
        self._error(404, "not found")

    def _static(self, name: str) -> None:
        p = (STATIC_DIR / name).resolve()
        if STATIC_DIR.resolve() not in p.parents or not p.is_file():
            return self._error(404, "not found")
        ctype = mimetypes.guess_type(p.name)[0] or "application/octet-stream"
        if ctype.startswith("text/") or ctype.endswith("javascript"):
            ctype += "; charset=utf-8"
        self._send(200, p.read_bytes(), ctype)

    def do_POST(self):  # noqa: N802
        length = int(self.headers.get("Content-Length") or 0)
        if length > MAX_BODY:
            return self._error(413, "request too large")
        try:
            body = json.loads(self.rfile.read(length) or b"{}") if length else {}
        except ValueError:
            return self._error(400, "body is not JSON")
        if not isinstance(body, dict):
            return self._error(400, "body must be an object")
        path = self.path.split("?", 1)[0]
        run = self.app.run
        try:
            with self.app.lock:
                if path == "/api/undo":
                    snap = run.undo()
                    return self._json(200, self.app.payload(snap))
                m = _SNAP.match(path)
                if not m:
                    return self._error(404, "not found")
                idx, verb, ann_id, ann_delete = int(m.group(1)), m.group(2), m.group(3), m.group(4)
                if verb is None:
                    snap = run.update(idx, body)
                elif verb == "delete":
                    snap = run.delete(idx)
                elif verb == "restore":
                    snap = run.restore(idx, body.get("status", "kept"))
                elif ann_id is None:
                    snap = run.add_annotation(idx, body.get("rect"), body.get("text", ""),
                                              body.get("facet"))
                elif ann_delete:
                    snap = run.delete_annotation(idx, ann_id)
                else:
                    snap = run.update_annotation(idx, ann_id, body)
                return self._json(200, self.app.payload(snap))
        except TriageError as exc:
            return self._error(400, str(exc))
        except OSError as exc:
            return self._error(500, f"could not write: {exc}")


def make_server(run: Run, port: int = DEFAULT_PORT, hub: bool = True) -> ThreadingHTTPServer:
    handler = type("BoundHandler", (Handler,), {"app": TriageApp(run, hub)})
    return ThreadingHTTPServer(("127.0.0.1", port), handler)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="sandover-triage", description=__doc__.splitlines()[0])
    ap.add_argument("run_dir", type=Path, help="run folder: $SANDOVER_DATA/runs/<YYYYMMDD-HHMM>-<slug>")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--no-hub", action="store_true", help="do not update the review dashboard card caption")
    args = ap.parse_args(argv)
    try:
        run = Run(args.run_dir)
    except TriageError as exc:
        print(f"sandover-triage: {exc}", file=sys.stderr)
        return 2
    run.export_csv()
    srv = make_server(run, args.port, hub=not args.no_hub)
    srv.RequestHandlerClass.app.payload()
    print(f"triage: http://127.0.0.1:{args.port}/  ({run.root.name})", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
