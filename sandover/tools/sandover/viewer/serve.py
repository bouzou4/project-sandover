"""CLI and HTTP server for the local 3D review viewer (ADR-0015, design 3.6).

Serves the static three.js page, a catalogue of every ripped glb under a models root, and
one or more named texture-set directories (vanilla / pack / candidate). Everything is
local: no external CDN, no telemetry. Texture files are indexed by filename stem (see
kb/howto/viewer.md for the tpage/name mapping this assumes) so the page can look a
material's texture up by name without knowing which tpage subdirectory it lives in.
Screenshots and render-bug reports are written under server-chosen names only.
"""

from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
import ipaddress
import json
import mimetypes
import struct
import sys
import webbrowser
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse

from sandover.materials.overrides import append_override
from sandover.materials.sheets import materials_dir, read_csv as read_materials_csv
from sandover.paths import data_dir
from sandover.unbake import sidecar as unbake_sidecar

STATIC_DIR = Path(__file__).parent / "static"
TEXTURE_EXTS = {".png"}
DUPES_SHOWN = 10
MAX_SCREENSHOT_BYTES = 64 * 1024 * 1024
MAX_REPORT_BYTES = MAX_SCREENSHOT_BYTES * 4 // 3 + 64 * 1024
MAX_NOTE_CHARS = 4000
MAX_MARK_BYTES = 8 * 1024
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"
GLB_MAGIC = b"glTF"
GLB_JSON_CHUNK = 0x4E4F534A
REPORT_META_KEYS = (
    "model",
    "textureSet",
    "shading",
    "alpha",
    "camera",
    "light",
    "note",
    "viewport",
)
CLI_MODEL_ID = "@cli"

mimetypes.add_type("text/javascript", ".js")
mimetypes.add_type("model/gltf-binary", ".glb")
mimetypes.add_type("model/gltf+json", ".gltf")


def _index_textures(root: Path) -> tuple[dict[str, str], dict[str, list[str]]]:
    """Map texture name (filename stem) -> path relative to ``root``, plus stem collisions.

    First match wins when a name appears under more than one tpage; the second dict lists
    every path for each colliding stem. See kb/howto/viewer.md.
    """
    index: dict[str, str] = {}
    dupes: dict[str, list[str]] = {}
    for p in sorted(root.rglob("*")):
        if p.suffix.lower() in TEXTURE_EXTS and p.is_file():
            rel = p.relative_to(root).as_posix()
            if p.stem in index:
                dupes.setdefault(p.stem, [index[p.stem]]).append(rel)
            else:
                index[p.stem] = rel
    return index, dupes


def read_glb_json(path: Path) -> dict:
    """Return the JSON chunk of a binary glTF without reading its binary chunk.

    Raises ``ValueError`` when the file is not a glb.
    """
    with open(path, "rb") as f:
        header = f.read(20)
        if len(header) < 20 or header[:4] != GLB_MAGIC:
            raise ValueError(f"not a glb: {path.name}")
        chunk_len, chunk_type = struct.unpack("<II", header[12:20])
        if chunk_type != GLB_JSON_CHUNK:
            raise ValueError(f"first chunk is not JSON: {path.name}")
        return json.loads(f.read(chunk_len))


def model_entry(root: Path, path: Path) -> dict:
    """Catalogue record for one glb: level, file, size, skinned, primitive count, kind."""
    rel = path.relative_to(root).as_posix()
    entry = {
        "id": rel,
        "level": path.parent.name if path.parent != root else "",
        "file": path.name,
        "size": path.stat().st_size,
        "skinned": False,
        "primitives": 0,
        "kind": "prop",
    }
    try:
        gltf = read_glb_json(path)
    except (OSError, ValueError) as e:
        entry["error"] = str(e)
        return entry
    entry["skinned"] = bool(gltf.get("skins"))
    entry["primitives"] = sum(len(m.get("primitives", [])) for m in gltf.get("meshes", []))
    if entry["skinned"]:
        entry["kind"] = "skinned"
    elif path.stem.endswith("-background"):
        entry["kind"] = "background"
    return entry


def index_models(root: Path) -> list[dict]:
    """Every ``*.glb`` under ``root``, sorted by level then file."""
    if not root.is_dir():
        return []
    entries = [model_entry(root, p) for p in root.rglob("*.glb") if p.is_file()]
    return sorted(entries, key=lambda e: (e["level"], e["file"]))


def _texture_stems(gltf: dict) -> set[str]:
    """Every texture name a glb resolves, matching GLTFLoader (``textureDef.name or sourceDef.name``)."""
    images = gltf.get("images", [])
    out: set[str] = set()
    for tex in gltf.get("textures", []):
        name = tex.get("name")
        if not name:
            src = tex.get("source")
            if isinstance(src, int) and 0 <= src < len(images):
                name = images[src].get("name")
        if name:
            out.add(name)
    return out


def _triangle_count(gltf: dict) -> int:
    """Sum of triangles over every primitive, from accessor counts only (no binary chunk)."""
    accessors = gltf.get("accessors", [])

    def count(acc_idx: int | None) -> int:
        if acc_idx is None or not (0 <= acc_idx < len(accessors)):
            return 0
        return accessors[acc_idx].get("count", 0)

    total = 0
    for mesh in gltf.get("meshes", []):
        for prim in mesh.get("primitives", []):
            indices = prim.get("indices")
            n = count(indices) if indices is not None else count(prim.get("attributes", {}).get("POSITION"))
            total += n // 3
    return total


KIND_RANK = {"skinned": 0, "prop": 1, "background": 2}


def build_texture_model_index(root: Path, catalogue: list[dict]) -> dict[str, list[dict]]:
    """Texture stem -> models using it, most useful for context first.

    Order: skinned characters, then props, then background chunks, then triangle count
    ascending within each group (kb/howto/viewer.md).
    """
    stem_models: dict[str, list[dict]] = {}
    for entry in catalogue:
        if "error" in entry:
            continue
        try:
            gltf = read_glb_json(root / entry["id"])
        except (OSError, ValueError):
            continue
        rec = {"id": entry["id"], "kind": entry["kind"], "triangles": _triangle_count(gltf)}
        for stem in _texture_stems(gltf):
            stem_models.setdefault(stem, []).append(rec)
    for recs in stem_models.values():
        recs.sort(key=lambda r: (KIND_RANK.get(r["kind"], 3), r["triangles"]))
    return stem_models


def _texture_index_cache_path(root: Path) -> Path:
    digest = hashlib.sha1(str(root.resolve()).encode()).hexdigest()[:16]
    return data_dir() / "viewer" / "cache" / f"tex-index-{digest}.json"


def load_or_build_texture_index(root: Path, catalogue: list[dict]) -> tuple[dict[str, list[dict]], bool]:
    """Cached ``build_texture_model_index``, keyed by the rip root's own mtime; (index, from_cache)."""
    cache_file = _texture_index_cache_path(root)
    try:
        root_mtime = root.stat().st_mtime
    except OSError:
        root_mtime = None
    if cache_file.is_file():
        try:
            cached = json.loads(cache_file.read_text(encoding="utf-8"))
            if cached.get("root") == str(root.resolve()) and cached.get("root_mtime") == root_mtime:
                return cached["index"], True
        except (OSError, json.JSONDecodeError, KeyError):
            pass
    index = build_texture_model_index(root, catalogue)
    try:
        cache_file.parent.mkdir(parents=True, exist_ok=True)
        cache_file.write_text(
            json.dumps({"root": str(root.resolve()), "root_mtime": root_mtime, "index": index}),
            encoding="utf-8",
        )
    except OSError:
        pass
    return index, False


def build_report(body: bytes) -> tuple[bytes, dict]:
    """Validate a render-bug report upload; return (png bytes, sanitised metadata).

    Body is JSON ``{"png": <base64>, "meta": {...}}``. Only ``REPORT_META_KEYS`` are kept.
    Raises ``ValueError`` with a short reason on bad input.
    """
    try:
        payload = json.loads(body)
    except (UnicodeDecodeError, json.JSONDecodeError) as e:
        raise ValueError("bad json") from e
    if not isinstance(payload, dict) or not isinstance(payload.get("png"), str):
        raise ValueError("missing png")
    try:
        png = base64.b64decode(payload["png"], validate=True)
    except (binascii.Error, ValueError) as e:
        raise ValueError("bad base64") from e
    if not png.startswith(PNG_MAGIC) or len(png) > MAX_SCREENSHOT_BYTES:
        raise ValueError("not a png")
    raw_meta = payload.get("meta")
    if not isinstance(raw_meta, dict):
        raise ValueError("missing meta")
    meta = {k: raw_meta[k] for k in REPORT_META_KEYS if k in raw_meta}
    meta["note"] = str(meta.get("note", ""))[:MAX_NOTE_CHARS]
    return png, meta


def _unique_dir(parent: Path, stamp: str) -> Path:
    parent.mkdir(parents=True, exist_ok=True)
    for n in range(1000):
        d = parent / (stamp if n == 0 else f"{stamp}-{n}")
        try:
            d.mkdir()
            return d
        except FileExistsError:
            continue
    raise FileExistsError(stamp)


def write_report(reports_dir: Path, png: bytes, meta: dict) -> str:
    """Write ``shot.png`` and ``report.json`` into a new timestamped directory; return its name."""
    now = datetime.now().astimezone()
    d = _unique_dir(reports_dir, f"{now:%Y%m%d-%H%M%S}")
    (d / "shot.png").write_bytes(png)
    record = {"timestamp": now.isoformat(timespec="seconds"), **meta}
    (d / "report.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
    return d.name


def list_reports(reports_dir: Path) -> list[dict]:
    """Existing reports, newest first: id, timestamp, model, note (truncated)."""
    out: list[dict] = []
    if not reports_dir.is_dir():
        return out
    for d in sorted(reports_dir.iterdir(), reverse=True):
        rj = d / "report.json"
        if not rj.is_file():
            continue
        try:
            rec = json.loads(rj.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        out.append(
            {
                "id": d.name,
                "timestamp": rec.get("timestamp", ""),
                "model": rec.get("model", ""),
                "note": str(rec.get("note", ""))[:200],
            }
        )
    return out


def _is_loopback(host: str) -> bool:
    if host == "localhost":
        return True
    try:
        addr = ipaddress.ip_address(host)
        return addr.version == 4 and addr.is_loopback
    except ValueError:
        return False


def _safe_join(root: Path, rel: str) -> Path | None:
    try:
        target = (root / unquote(rel)).resolve()
    except (OSError, ValueError):
        return None
    root = root.resolve()
    if root != target and root not in target.parents:
        return None
    return target


def make_handler(
    model: Path | None,
    models_root: Path,
    textures: dict[str, Path],
    out_dir: Path,
    reports_dir: Path,
    host: str,
    port: int,
    lighting_dir: Path | None = None,
    materials_csv: Path | None = None,
    overrides_csv: Path | None = None,
    emissive_maps_dir: Path | None = None,
    pbr_maps_dir: Path | None = None,
):
    """``lighting_dir``/``materials_csv``/``overrides_csv`` are the inspect panel's optional
    material lookup, vertex-E lookup and mark-emissive write path (kb/howto/viewer.md
    "Inspect panel"); ``None`` disables that one piece (material -> ``{}``, E -> "no
    sidecar", ``POST /api/mark`` -> 404) instead of touching a default location.
    ``emissive_maps_dir`` is ``sandover-materials emissive-maps`` output, served per texture
    stem at ``/api/emissive-map/<stem>`` for the bloom preview (404 when ``None``).
    ``pbr_maps_dir`` is ``sandover-materials derive-maps`` output: ``/api/pbr-index`` lists
    the stems that have maps (with metallic and recipe) and ``/api/pbr-map/<stem>/<normal|rough>``
    serves one map (empty index / 404 when ``None``).
    """
    indexes: dict[str, dict[str, str]] = {}
    for name, d in textures.items():
        indexes[name], dupes = _index_textures(d)
        if dupes:
            sys.stderr.write(
                f"[viewer] WARNING textures[{name}]: {len(dupes)} stem(s) exist under more than"
                " one tpage; first path wins:\n"
            )
            shown = sorted(dupes.items())[:DUPES_SHOWN]
            for stem, paths in shown:
                sys.stderr.write(f"[viewer]   {stem}: {', '.join(paths)}\n")
            if len(dupes) > len(shown):
                sys.stderr.write(f"[viewer]   ... {len(dupes) - len(shown)} more\n")

    catalogue = index_models(models_root)
    default_id = None
    if model is not None:
        if models_root in model.parents:
            default_id = model.relative_to(models_root).as_posix()
        else:
            cli = model_entry(model.parent, model)
            cli.update(id=CLI_MODEL_ID, level="(command line)")
            catalogue.insert(0, cli)
            default_id = CLI_MODEL_ID
    sys.stderr.write(f"[viewer] {len(catalogue)} model(s) in the catalogue\n")
    texture_model_index, tex_index_cached = load_or_build_texture_index(models_root, catalogue)
    sys.stderr.write(
        f"[viewer] texture usage index: {len(texture_model_index)} stem(s) "
        f"({'cache hit' if tex_index_cached else 'built'})\n"
    )
    models_json = json.dumps({"default": default_id, "models": catalogue}).encode()
    allowed_hosts = {f"{h}:{port}" for h in (host, "localhost")}
    catalogue_by_id = {e["id"]: e for e in catalogue}

    material_index: dict[str, dict] = {}
    if materials_csv is not None and materials_csv.is_file():
        for row in read_materials_csv(materials_csv):
            material_index.setdefault(row.get("name", ""), row)

    # Per-model tree-name -> (kind, fr3_tree) and per-level sidecar E arrays, built lazily
    # and cached: a click resolves both from these, see kb/howto/viewer.md "Inspect panel".
    tree_name_cache: dict[str, dict[str, tuple[int, int]]] = {}
    sidecar_cache: dict[str, dict[tuple[int, int], object] | None] = {}

    def tree_names_for(path: Path) -> dict[str, tuple[int, int]]:
        key = str(path)
        if key not in tree_name_cache:
            try:
                tree_name_cache[key] = unbake_sidecar.scan_glb_tree_names(read_glb_json(path))
            except (OSError, ValueError):
                tree_name_cache[key] = {}
        return tree_name_cache[key]

    def sidecar_e_for(level: str):
        if level not in sidecar_cache:
            path = (lighting_dir / f"{level}.unbake") if lighting_dir is not None else None
            if path is None or not path.is_file():
                sidecar_cache[level] = None
            else:
                try:
                    parsed = unbake_sidecar.read(path)
                except (OSError, ValueError):
                    sidecar_cache[level] = None
                else:
                    m = {}
                    for t in parsed["trees"]:
                        rec = t["records"]
                        m[(t["kind"], t["fr3_tree"])] = ((rec[:, 3] >> 16) & 0xFF) / 255.0
                    sidecar_cache[level] = m
        return sidecar_cache[level]

    em_index: dict = {"mtime": None, "textures": {}}

    def emissive_map_path(stem: str) -> Path | None:
        if emissive_maps_dir is None:
            return None
        idx = emissive_maps_dir / "index.json"
        try:
            mtime = idx.stat().st_mtime
            if em_index["mtime"] != mtime:
                em_index["textures"] = json.loads(idx.read_text(encoding="utf-8")).get("textures", {})
                em_index["mtime"] = mtime
        except (OSError, json.JSONDecodeError):
            return None
        rec = em_index["textures"].get(stem)
        if not isinstance(rec, dict):
            return None
        return _safe_join(emissive_maps_dir, f"{rec.get('tpage', '')}/{stem}.png")

    pbr_index: dict = {"mtime": None, "stems": {}}

    def pbr_stems() -> dict[str, dict]:
        """Stem -> {tpage, metallic, recipe, flat}, reloaded when index.json changes; the first
        tpage wins on a stem collision, like the texture-set index."""
        if pbr_maps_dir is None:
            return {}
        idx = pbr_maps_dir / "index.json"
        try:
            mtime = idx.stat().st_mtime
            if pbr_index["mtime"] != mtime:
                textures = json.loads(idx.read_text(encoding="utf-8")).get("textures", {})
                stems: dict[str, dict] = {}
                for key in sorted(textures):
                    rec = textures[key]
                    if isinstance(rec, dict) and rec.get("name") and rec["name"] not in stems:
                        stems[rec["name"]] = {"tpage": rec.get("tpage", ""),
                                              "metallic": rec.get("metallic", 0.0),
                                              "recipe": rec.get("recipe", ""),
                                              "flat": bool(rec.get("flat"))}
                pbr_index["stems"], pbr_index["mtime"] = stems, mtime
        except (OSError, json.JSONDecodeError):
            return {}
        return pbr_index["stems"]

    def pbr_map_path(stem: str, kind: str) -> Path | None:
        rec = pbr_stems().get(stem)
        if rec is None or kind not in ("normal", "rough") or pbr_maps_dir is None:
            return None
        return _safe_join(pbr_maps_dir, f"{rec['tpage']}/{stem}.{kind}.png")

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt: str, *args) -> None:  # quieter default logging
            sys.stderr.write(f"[viewer] {self.address_string()} {fmt % args}\n")

        def _send_bytes(self, data: bytes, content_type: str, status: int = 200) -> None:
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def _send_json(self, obj) -> None:
            self._send_bytes(json.dumps(obj).encode(), "application/json")

        def _send_file(self, path: Path | None) -> None:
            if path is None:
                self._send_bytes(b"bad path", "text/plain", 400)
                return
            if not path.is_file():
                self._send_bytes(b"not found", "text/plain", 404)
                return
            ctype = mimetypes.guess_type(path.name)[0] or "application/octet-stream"
            self._send_bytes(path.read_bytes(), ctype)

        def _host_ok(self) -> bool:
            # Rejects DNS-rebinding and cross-site form posts.
            origin = self.headers.get("Origin")
            if origin is not None and urlparse(origin).netloc not in allowed_hosts:
                return False
            return self.headers.get("Host") in allowed_hosts

        def _read_body(self, cap: int) -> bytes | None:
            try:
                length = int(self.headers.get("Content-Length", ""))
            except ValueError:
                length = -1
            if not 0 < length <= cap:
                self._send_bytes(b"bad length", "text/plain", 400)
                return None
            return self.rfile.read(length)

        def do_GET(self) -> None:
            if not self._host_ok():
                self._send_bytes(b"forbidden", "text/plain", 403)
                return
            path = urlparse(self.path).path
            if path == "/":
                path = "/index.html"

            if path == "/models.json":
                self._send_bytes(models_json, "application/json")
                return
            if path == "/model.glb" and model is not None:
                self._send_file(model)
                return
            if path.startswith("/models/"):
                target = _safe_join(models_root, path[len("/models/") :])
                if target is not None and target.suffix.lower() != ".glb":
                    target = None
                self._send_file(target)
                return

            if path.startswith("/api/models-using/"):
                stem = unquote(path[len("/api/models-using/") :])
                self._send_json(texture_model_index.get(stem, []))
                return
            if path.startswith("/api/material/"):
                stem = unquote(path[len("/api/material/") :])
                self._send_json(material_index.get(stem, {}))
                return
            if path.startswith("/api/emissive-map/"):
                target = emissive_map_path(unquote(path[len("/api/emissive-map/") :]))
                if target is None:
                    self._send_bytes(b"no emissive map", "text/plain", 404)
                else:
                    self._send_file(target)
                return
            if path == "/api/pbr-index":
                self._send_json(pbr_stems())
                return
            if path.startswith("/api/pbr-map/"):
                stem, _, kind = unquote(path[len("/api/pbr-map/") :]).rpartition("/")
                target = pbr_map_path(stem, kind)
                if target is None or not target.is_file():
                    self._send_bytes(b"no pbr map", "text/plain", 404)
                else:
                    self._send_file(target)
                return
            if path == "/api/vertex-e":
                self._get_vertex_e()
                return

            if path == "/reports.json":
                self._send_json(list_reports(reports_dir))
                return
            if path.startswith("/reports/"):
                target = _safe_join(reports_dir, path[len("/reports/") :])
                if target is not None and target.name != "shot.png":
                    target = None
                self._send_file(target)
                return

            if path == "/textures.json":
                self._send_json(list(textures.keys()))
                return
            if path.startswith("/textures/"):
                set_name, _, rel = path[len("/textures/") :].partition("/")
                if set_name not in textures:
                    self._send_bytes(b"unknown texture set", "text/plain", 404)
                    return
                if not rel:
                    self._send_json(indexes[set_name])
                    return
                self._send_file(_safe_join(textures[set_name], rel))
                return

            self._send_file(_safe_join(STATIC_DIR, path.lstrip("/")))

        def _get_vertex_e(self) -> None:
            """``?model=<catalogue id>&tree=<root node name>&vertex=<index>`` -> ``{"e": ...}``.

            ``tree`` is the clicked primitive's nearest scene-root ancestor name (the viewer
            climbs to it at load time); ``e`` is ``null`` with a ``reason`` when the model
            isn't a catalogued background chunk, the tree has no matching sidecar tree, or
            the vertex index is out of range.
            """
            qs = parse_qs(urlparse(self.path).query)
            model_id = (qs.get("model") or [""])[0]
            tree = (qs.get("tree") or [""])[0]
            vertex_s = (qs.get("vertex") or [""])[0]
            entry = catalogue_by_id.get(model_id)
            target = _safe_join(models_root, model_id) if model_id else None
            if entry is None or target is None or target.suffix.lower() != ".glb" or not vertex_s.isdigit():
                self._send_json({"e": None, "reason": "bad request"})
                return
            kind_idx = tree_names_for(target).get(tree)
            e_map = sidecar_e_for(entry.get("level", "")) if kind_idx is not None else None
            e_arr = e_map.get(kind_idx) if e_map is not None else None
            vertex = int(vertex_s)
            if e_arr is None or not (0 <= vertex < len(e_arr)):
                self._send_json({"e": None, "reason": "no sidecar"})
                return
            self._send_json({"e": round(float(e_arr[vertex]), 4)})

        def do_POST(self) -> None:
            if not self._host_ok():
                self._send_bytes(b"forbidden", "text/plain", 403)
                return
            route = urlparse(self.path).path
            if route == "/screenshot":
                self._post_screenshot()
            elif route == "/report":
                self._post_report()
            elif route == "/api/mark":
                self._post_mark()
            else:
                self._send_bytes(b"not found", "text/plain", 404)

        def _post_screenshot(self) -> None:
            data = self._read_body(MAX_SCREENSHOT_BYTES)
            if data is None:
                return
            if not data.startswith(PNG_MAGIC):
                self._send_bytes(b"not a png", "text/plain", 400)
                return
            # Name is server-generated; the client never picks a path.
            out_dir.mkdir(parents=True, exist_ok=True)
            stamp = f"{datetime.now().astimezone():%Y%m%d-%H%M%S}"
            for n in range(1000):
                name = f"{stamp}.png" if n == 0 else f"{stamp}-{n}.png"
                try:
                    with open(out_dir / name, "xb") as f:
                        f.write(data)
                    break
                except FileExistsError:
                    continue
            else:
                self._send_bytes(b"name collision", "text/plain", 500)
                return
            self._send_json({"saved": name})

        def _post_report(self) -> None:
            body = self._read_body(MAX_REPORT_BYTES)
            if body is None:
                return
            try:
                png, meta = build_report(body)
            except ValueError as e:
                self._send_bytes(str(e).encode(), "text/plain", 400)
                return
            self._send_json({"saved": write_report(reports_dir, png, meta)})

        def _post_mark(self) -> None:
            """Emissive-verdict from the inspect panel: appends a row to ``overrides_csv``
            (``sandover.materials.overrides``, shared with the reviewer's Browse tab and the
            in-game inspect tool). Body: JSON object with the override columns (all optional
            except ``verdict``, see ``kb/howto/viewer.md``)."""
            if overrides_csv is None:
                self._send_bytes(b"materials overrides not configured", "text/plain", 404)
                return
            body = self._read_body(MAX_MARK_BYTES)
            if body is None:
                return
            try:
                payload = json.loads(body)
            except (UnicodeDecodeError, json.JSONDecodeError):
                self._send_bytes(b"bad json", "text/plain", 400)
                return
            if not isinstance(payload, dict):
                self._send_bytes(b"bad json", "text/plain", 400)
                return
            try:
                row = append_override(overrides_csv, payload)
            except ValueError as e:
                self._send_bytes(str(e).encode(), "text/plain", 400)
                return
            self._send_json({"saved": row})

    return Handler


def _parse_textures(pairs: list[str]) -> dict[str, Path]:
    out: dict[str, Path] = {}
    for pair in pairs:
        if "=" not in pair:
            raise argparse.ArgumentTypeError(f"expected name=dir, got {pair!r}")
        name, _, raw = pair.partition("=")
        d = Path(raw)
        if not d.is_dir():
            raise argparse.ArgumentTypeError(f"not a directory: {d}")
        out[name] = d
    return out


def main() -> None:
    default_models = data_dir() / "unbake" / "riproot" / "decompiler_out" / "jak1" / "levels"
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "--models",
        type=Path,
        default=default_models,
        help="rip root indexed for the model picker "
        "(default $SANDOVER_DATA/unbake/riproot/decompiler_out/jak1/levels)",
    )
    ap.add_argument("--model", type=Path, help="glb opened first (optional)")
    ap.add_argument(
        "--textures",
        nargs="+",
        required=True,
        metavar="name=dir",
        help="one or more texture-set directories, e.g. vanilla=<dir> pack=<dir>",
    )
    ap.add_argument("--host", default="127.0.0.1", help="IPv4 loopback address only")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument(
        "--out",
        type=Path,
        default=None,
        help="screenshot output dir (default $SANDOVER_DATA/viewer)",
    )
    ap.add_argument(
        "--reports",
        type=Path,
        default=None,
        help="render-bug report dir (default $SANDOVER_DATA/viewer/reports)",
    )
    ap.add_argument(
        "--lighting",
        type=Path,
        default=None,
        help="per-level .unbake sidecars for the inspect panel's E lookup "
        "(default $SANDOVER_DATA/lighting)",
    )
    ap.add_argument(
        "--emissive-maps",
        type=Path,
        default=None,
        help="emissive maps for ?bloom=1 (default $SANDOVER_DATA/materials/emissive-maps)",
    )
    ap.add_argument(
        "--pbr-maps",
        type=Path,
        default=None,
        help="derived normal/roughness maps for the PBR preview "
        "(default $SANDOVER_DATA/materials/pbr-maps)",
    )
    ap.add_argument("--no-browser", action="store_true")
    args = ap.parse_args()

    if args.model is not None and not args.model.is_file():
        ap.error(f"model not found: {args.model}")
    if not _is_loopback(args.host):
        ap.error(f"--host must be a loopback address, got {args.host!r}")
    textures = _parse_textures(args.textures)
    out_dir = args.out or (data_dir() / "viewer")
    reports_dir = args.reports or (data_dir() / "viewer" / "reports")
    lighting_dir = args.lighting or (data_dir() / "lighting")
    materials_csv = materials_dir() / "materials.csv"
    overrides_csv = materials_dir() / "emissive-overrides.csv"

    host = "127.0.0.1" if args.host == "localhost" else args.host
    handler = make_handler(
        args.model.resolve() if args.model else None,
        args.models.resolve(),
        {k: v.resolve() for k, v in textures.items()},
        out_dir,
        reports_dir,
        host,
        args.port,
        lighting_dir=lighting_dir,
        materials_csv=materials_csv,
        overrides_csv=overrides_csv,
        emissive_maps_dir=(args.emissive_maps or materials_dir() / "emissive-maps").resolve(),
        pbr_maps_dir=(args.pbr_maps or materials_dir() / "pbr-maps").resolve(),
    )
    httpd = ThreadingHTTPServer((host, args.port), handler)
    url = f"http://{host}:{args.port}/"
    print(f"[viewer] models={args.models}")
    for name, d in textures.items():
        print(f"[viewer] textures[{name}]={d}")
    print(f"[viewer] screenshots -> {out_dir}")
    print(f"[viewer] reports -> {reports_dir}")
    print(f"[viewer] lighting sidecars -> {lighting_dir}")
    print(f"[viewer] materials.csv -> {materials_csv} ({'found' if materials_csv.is_file() else 'not found'})")
    print(f"[viewer] serving {url}")
    if not args.no_browser:
        webbrowser.open(url)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
