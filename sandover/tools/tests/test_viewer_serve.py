import base64
import http.client
import json
import socket
import struct
import threading
from http.server import ThreadingHTTPServer
from pathlib import Path

import pytest

from sandover.viewer import serve

PNG = serve.PNG_MAGIC + b"\x00" * 16


def _glb(path: Path, gltf: dict) -> Path:
    body = json.dumps(gltf).encode()
    body += b" " * (-len(body) % 4)
    header = struct.pack("<4sII", b"glTF", 2, 12 + 8 + len(body))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + struct.pack("<II", len(body), serve.GLB_JSON_CHUNK) + body)
    return path


@pytest.fixture
def models(tmp_path: Path) -> Path:
    root = tmp_path / "levels"
    two_prims = {"meshes": [{"primitives": [{}, {}]}]}
    _glb(root / "beach" / "mayor-lod0.glb", {**two_prims, "skins": [{"joints": [0]}]})
    _glb(root / "beach" / "barrel-lod0.glb", {"meshes": [{"primitives": [{}]}]})
    _glb(root / "beach" / "beach-background.glb", two_prims)
    (root / "beach" / "broken.glb").write_bytes(b"not a glb")
    return root


def _tex_gltf(image_names, indices_count=None, position_count=None, skinned=False):
    accessors = []
    prim = {}
    if position_count is not None:
        accessors.append({"count": position_count})
        prim["attributes"] = {"POSITION": len(accessors) - 1}
    if indices_count is not None:
        accessors.append({"count": indices_count})
        prim["indices"] = len(accessors) - 1
    gltf = {
        "images": [{"name": n} for n in image_names],
        "textures": [{"source": i} for i in range(len(image_names))],
        "meshes": [{"primitives": [prim]}],
        "accessors": accessors,
    }
    if skinned:
        gltf["skins"] = [{"joints": [0]}]
    return gltf


@pytest.fixture
def models_tex(tmp_path: Path) -> Path:
    root = tmp_path / "levels"
    _glb(root / "beach" / "mayor-lod0.glb",
         _tex_gltf(["mayor-flesh", "shared-tex"], indices_count=300, skinned=True))  # 100 tri
    _glb(root / "beach" / "crate-lod0.glb", _tex_gltf(["shared-tex"], indices_count=15))  # 5 tri
    _glb(root / "beach" / "barrel-lod0.glb", _tex_gltf(["shared-tex"], indices_count=30))  # 10 tri
    _glb(root / "beach" / "beach-background.glb", _tex_gltf(["shared-tex"], indices_count=1500))  # 500 tri
    return root


def test_index_models_kinds_and_counts(models: Path):
    entries = {e["file"]: e for e in serve.index_models(models)}
    assert entries["mayor-lod0.glb"]["kind"] == "skinned"
    assert entries["mayor-lod0.glb"]["primitives"] == 2
    assert entries["mayor-lod0.glb"]["level"] == "beach"
    assert entries["barrel-lod0.glb"]["kind"] == "prop"
    assert entries["beach-background.glb"]["kind"] == "background"
    assert "error" in entries["broken.glb"]


def _report_body(meta: dict, png: bytes = PNG) -> bytes:
    return json.dumps({"png": base64.b64encode(png).decode(), "meta": meta}).encode()


def test_build_report_filters_meta_and_truncates_note():
    png, meta = serve.build_report(
        _report_body({"model": "beach/mayor-lod0.glb", "note": "x" * 9000, "path": "../evil"})
    )
    assert png == PNG
    assert "path" not in meta
    assert len(meta["note"]) == serve.MAX_NOTE_CHARS


@pytest.mark.parametrize(
    "body",
    [b"{", json.dumps({"png": "!!", "meta": {}}).encode(), _report_body({}, b"GIF89a"), _report_body(None)],
)
def test_build_report_rejects_bad_input(body: bytes):
    with pytest.raises(ValueError):
        serve.build_report(body)


def test_write_and_list_reports(tmp_path: Path):
    a = serve.write_report(tmp_path, PNG, {"model": "m", "note": "n"})
    b = serve.write_report(tmp_path, PNG, {"model": "m", "note": "n2"})
    assert a != b
    assert (tmp_path / a / "shot.png").read_bytes() == PNG
    listed = serve.list_reports(tmp_path)
    assert [r["id"] for r in listed] == sorted([a, b], reverse=True)
    assert json.loads((tmp_path / a / "report.json").read_text())["timestamp"]


@pytest.fixture
def server(models: Path, tmp_path: Path):
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    tex = tmp_path / "tex"
    tex.mkdir()
    handler = serve.make_handler(
        None, models, {"vanilla": tex}, tmp_path / "shots", tmp_path / "reports", "127.0.0.1", port
    )
    httpd = ThreadingHTTPServer(("127.0.0.1", port), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    yield port, tmp_path
    httpd.shutdown()


def _request(port, method, path, body=None, host=None):
    conn = http.client.HTTPConnection("127.0.0.1", port)
    headers = {"Host": host or f"127.0.0.1:{port}"}
    conn.request(method, path, body=body, headers=headers)
    resp = conn.getresponse()
    return resp.status, resp.read()


def test_server_catalogue_model_and_report(server):
    port, tmp = server
    status, data = _request(port, "GET", "/models.json")
    assert status == 200
    ids = {m["id"] for m in json.loads(data)["models"]}
    assert "beach/mayor-lod0.glb" in ids
    status, data = _request(port, "GET", "/models/beach/mayor-lod0.glb")
    assert status == 200 and data.startswith(b"glTF")
    status, _ = _request(port, "POST", "/report", _report_body({"note": "hair"}))
    assert status == 200
    status, data = _request(port, "GET", "/reports.json")
    assert json.loads(data)[0]["note"] == "hair"


def test_server_refuses_foreign_host_and_traversal(server):
    port, _ = server
    assert _request(port, "GET", "/models.json", host="evil.example:80")[0] == 403
    assert _request(port, "GET", "/models/..%2F..%2Fsecret.glb")[0] in (400, 404)
    assert _request(port, "GET", "/models/beach/../../tex")[0] in (400, 404)
    assert _request(port, "GET", "/reports/../shots/x.png")[0] in (400, 404)


def test_texture_stems_prefers_texture_name_falls_back_to_image():
    gltf = {"images": [{"name": "img-a"}, {"name": "img-b"}],
            "textures": [{"source": 0, "name": "tex-override"}, {"source": 1}]}
    assert serve._texture_stems(gltf) == {"tex-override", "img-b"}


def test_triangle_count_prefers_indices_falls_back_to_position():
    assert serve._triangle_count(_tex_gltf(["a"], indices_count=30, position_count=12)) == 10
    assert serve._triangle_count(_tex_gltf(["a"], position_count=9)) == 3
    assert serve._triangle_count(_tex_gltf(["a"])) == 0


def test_build_texture_model_index_sorts_skinned_prop_background_then_triangles(models_tex: Path):
    catalogue = serve.index_models(models_tex)
    idx = serve.build_texture_model_index(models_tex, catalogue)
    assert [r["id"] for r in idx["shared-tex"]] == [
        "beach/mayor-lod0.glb",
        "beach/crate-lod0.glb",
        "beach/barrel-lod0.glb",
        "beach/beach-background.glb",
    ]
    assert idx["mayor-flesh"] == [{"id": "beach/mayor-lod0.glb", "kind": "skinned", "triangles": 100}]
    assert "no-such-stem" not in idx


def test_texture_index_cache_round_trips_and_invalidates_on_mtime(
    tmp_path: Path, models_tex: Path, monkeypatch
):
    monkeypatch.setenv("SANDOVER_DATA", str(tmp_path / "data"))
    catalogue = serve.index_models(models_tex)
    idx1, hit1 = serve.load_or_build_texture_index(models_tex, catalogue)
    assert hit1 is False
    idx2, hit2 = serve.load_or_build_texture_index(models_tex, catalogue)
    assert hit2 is True and idx1 == idx2

    import os
    import time
    st = models_tex.stat()
    time.sleep(0.01)
    os.utime(models_tex, (st.st_atime, st.st_mtime + 5))
    idx3, hit3 = serve.load_or_build_texture_index(models_tex, catalogue)
    assert hit3 is False and idx3 == idx1


def test_server_models_using_endpoint(models_tex: Path, tmp_path: Path):
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    tex = tmp_path / "tex2"
    tex.mkdir()
    handler = serve.make_handler(
        None, models_tex, {"vanilla": tex}, tmp_path / "shots2", tmp_path / "reports2", "127.0.0.1", port
    )
    httpd = ThreadingHTTPServer(("127.0.0.1", port), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    try:
        status, data = _request(port, "GET", "/api/models-using/shared-tex")
        assert status == 200
        ids = [r["id"] for r in json.loads(data)]
        assert ids[0] == "beach/mayor-lod0.glb"
        status, data = _request(port, "GET", "/api/models-using/no-such-stem")
        assert status == 200 and json.loads(data) == []
    finally:
        httpd.shutdown()


# ---- inspect panel: material lookup, vertex-E lookup, mark-emissive --------------------

def _write_unbake(path: Path, trees: list[tuple[int, int, "object"]]) -> None:
    """Minimal ``.unbake`` with only E populated, matching ``unbake.sidecar.read``'s layout."""
    import numpy as np

    hdr = b"SOVU" + struct.pack("<7I", 2, 43, len(trees), 0, 0, 0, 0)
    glob = bytes(64 * 8)
    data_start = len(hdr) + len(glob) + 32 * len(trees)
    tbl, blocks, pos = bytearray(), [], data_start
    for kind, idx, e in trees:
        rec = np.zeros((len(e), 4), np.uint32)
        rec[:, 3] = np.clip(np.rint(np.asarray(e) * 255), 0, 255).astype(np.uint32) << np.uint32(16)
        blk = rec.tobytes()
        tbl += struct.pack("<4I2Q", kind, idx, len(e), 0, 0, pos)
        blocks.append(blk)
        pos += len(blk)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(hdr + glob + bytes(tbl) + b"".join(blocks))


@pytest.fixture
def inspect_models(tmp_path: Path) -> Path:
    root = tmp_path / "levels"
    _glb(root / "beach" / "beach-background.glb",
         {"scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"name": "beach-tfrag-0"}], "meshes": []})
    _glb(root / "beach" / "mayor-lod0.glb", {"meshes": [{"primitives": [{}]}]})  # not a level chunk
    return root


@pytest.fixture
def inspect_server(inspect_models: Path, tmp_path: Path):
    lighting = tmp_path / "lighting"
    _write_unbake(lighting / "beach.unbake", [(0, 0, [0.0, 128 / 255, 26 / 255])])
    materials_csv = tmp_path / "materials.csv"
    materials_csv.write_text(
        "combo_id,tpage,name,class,confidence,judge_agreement,source,envmap_swatch,has_alpha,"
        "levels,reviewed,tags,note\n0xAABBCCDD,tp,torch,special,0.9,agree,judge,False,True,beach,"
        "True,,\n", encoding="utf-8")
    overrides_csv = tmp_path / "emissive-overrides.csv"
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    handler = serve.make_handler(
        None, inspect_models, {"vanilla": tmp_path / "tex"}, tmp_path / "shots", tmp_path / "reports",
        "127.0.0.1", port, lighting_dir=lighting, materials_csv=materials_csv, overrides_csv=overrides_csv,
    )
    (tmp_path / "tex").mkdir()
    httpd = ThreadingHTTPServer(("127.0.0.1", port), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    yield port, overrides_csv
    httpd.shutdown()


def test_material_endpoint_hit_and_miss(inspect_server):
    port, _ = inspect_server
    status, data = _request(port, "GET", "/api/material/torch")
    assert status == 200
    row = json.loads(data)
    assert row["class"] == "special" and row["combo_id"] == "0xAABBCCDD"
    status, data = _request(port, "GET", "/api/material/no-such-texture")
    assert status == 200 and json.loads(data) == {}


def test_material_endpoint_without_materials_csv_configured(models: Path, tmp_path: Path):
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    tex = tmp_path / "tex3"
    tex.mkdir()
    handler = serve.make_handler(
        None, models, {"vanilla": tex}, tmp_path / "shots3", tmp_path / "reports3", "127.0.0.1", port
    )
    httpd = ThreadingHTTPServer(("127.0.0.1", port), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    try:
        status, data = _request(port, "GET", "/api/material/anything")
        assert status == 200 and json.loads(data) == {}
    finally:
        httpd.shutdown()


def test_vertex_e_endpoint_hits_sidecar(inspect_server):
    port, _ = inspect_server
    status, data = _request(
        port, "GET", "/api/vertex-e?model=beach%2Fbeach-background.glb&tree=beach-tfrag-0&vertex=1")
    assert status == 200
    # the endpoint rounds to 4 decimal places
    assert json.loads(data)["e"] == pytest.approx(128 / 255, abs=1e-4)


def test_vertex_e_endpoint_out_of_range_and_no_sidecar(inspect_server):
    port, _ = inspect_server
    status, data = _request(
        port, "GET", "/api/vertex-e?model=beach%2Fbeach-background.glb&tree=beach-tfrag-0&vertex=99")
    assert json.loads(data) == {"e": None, "reason": "no sidecar"}
    # not a level-chunk model: no matching tfrag/tie tree name at all
    status, data = _request(
        port, "GET", "/api/vertex-e?model=beach%2Fmayor-lod0.glb&tree=whatever&vertex=0")
    assert json.loads(data) == {"e": None, "reason": "no sidecar"}
    status, data = _request(port, "GET", "/api/vertex-e?model=nope.glb&tree=x&vertex=0")
    assert json.loads(data) == {"e": None, "reason": "bad request"}


def test_mark_endpoint_appends_override(inspect_server):
    port, overrides_csv = inspect_server
    body = json.dumps({"combo_id": "0xAABBCCDD", "tpage": "tp", "name": "torch", "level": "beach",
                        "verdict": "emissive", "note": "lantern"}).encode()
    status, data = _request(port, "POST", "/api/mark", body)
    assert status == 200
    saved = json.loads(data)["saved"]
    assert saved["verdict"] == "emissive" and saved["timestamp"]
    assert overrides_csv.is_file()
    from sandover.materials.overrides import read_overrides
    assert len(read_overrides(overrides_csv)) == 1


def test_mark_endpoint_rejects_bad_verdict(inspect_server):
    port, _ = inspect_server
    body = json.dumps({"name": "torch", "verdict": "maybe"}).encode()
    status, _ = _request(port, "POST", "/api/mark", body)
    assert status == 400


def test_mark_endpoint_without_overrides_configured(models: Path, tmp_path: Path):
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    tex = tmp_path / "tex4"
    tex.mkdir()
    handler = serve.make_handler(
        None, models, {"vanilla": tex}, tmp_path / "shots4", tmp_path / "reports4", "127.0.0.1", port
    )
    httpd = ThreadingHTTPServer(("127.0.0.1", port), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    try:
        status, _ = _request(port, "POST", "/api/mark", json.dumps({"verdict": "emissive"}).encode())
        assert status == 404
    finally:
        httpd.shutdown()


def test_emissive_map_endpoint_resolves_stem_through_index(models: Path, tmp_path: Path):
    maps = tmp_path / "maps"
    (maps / "tp").mkdir(parents=True)
    (maps / "tp" / "lamp.png").write_bytes(PNG)
    (maps / "index.json").write_text(json.dumps({"textures": {"lamp": {"tpage": "tp"},
                                                              "evil": {"tpage": ".."}}}))
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    tex = tmp_path / "tex4"
    tex.mkdir()
    handler = serve.make_handler(None, models, {"vanilla": tex}, tmp_path / "s4", tmp_path / "r4", "127.0.0.1",
                                 port, emissive_maps_dir=maps)
    httpd = ThreadingHTTPServer(("127.0.0.1", port), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    try:
        status, data = _request(port, "GET", "/api/emissive-map/lamp")
        assert status == 200 and data == PNG
        assert _request(port, "GET", "/api/emissive-map/missing")[0] == 404
        assert _request(port, "GET", "/api/emissive-map/evil")[0] in (400, 404)
    finally:
        httpd.shutdown()


def test_pbr_endpoints_resolve_stem_through_index(models: Path, tmp_path: Path):
    maps = tmp_path / "pbr"
    (maps / "tp").mkdir(parents=True)
    (maps / "tp" / "rock.normal.png").write_bytes(PNG)
    (maps / "tp" / "rock.rough.png").write_bytes(PNG)
    (maps / "index.json").write_text(json.dumps({"textures": {
        "tp/rock": {"tpage": "tp", "name": "rock", "metallic": 0.0, "recipe": "stone"},
        "zz/rock": {"tpage": "zz", "name": "rock", "metallic": 1.0, "recipe": "metal"},
        "../evil": {"tpage": "..", "name": "evil"}}}))
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    tex = tmp_path / "tex5"
    tex.mkdir()
    handler = serve.make_handler(None, models, {"vanilla": tex}, tmp_path / "s5", tmp_path / "r5", "127.0.0.1",
                                 port, pbr_maps_dir=maps)
    httpd = ThreadingHTTPServer(("127.0.0.1", port), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    try:
        status, data = _request(port, "GET", "/api/pbr-index")
        idx = json.loads(data)
        assert status == 200 and idx["rock"]["tpage"] == "tp" and idx["rock"]["recipe"] == "stone"
        assert _request(port, "GET", "/api/pbr-map/rock/normal") == (200, PNG)
        assert _request(port, "GET", "/api/pbr-map/rock/rough")[0] == 200
        assert _request(port, "GET", "/api/pbr-map/rock/height")[0] == 404
        assert _request(port, "GET", "/api/pbr-map/missing/normal")[0] == 404
        assert _request(port, "GET", "/api/pbr-map/evil/normal")[0] == 404
    finally:
        httpd.shutdown()


def test_pbr_index_empty_without_maps_dir(models: Path, tmp_path: Path):
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    tex = tmp_path / "tex6"
    tex.mkdir()
    handler = serve.make_handler(None, models, {"vanilla": tex}, tmp_path / "s6", tmp_path / "r6", "127.0.0.1", port)
    httpd = ThreadingHTTPServer(("127.0.0.1", port), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    try:
        assert _request(port, "GET", "/api/pbr-index") == (200, b"{}")
        assert _request(port, "GET", "/api/pbr-map/rock/normal")[0] == 404
    finally:
        httpd.shutdown()
