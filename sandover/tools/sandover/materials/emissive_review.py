"""Emissive tab of ``sandover-review-materials``: place each emissive texture on a strength ladder.

Queue: every texture with an emissive map (``sandover-materials emissive-maps``) or a human
``emissive`` override, minus ``not-emissive`` overrides; ordered by emitter score
(``emitter-score.json``) when it exists, so lit receivers come last, else strongest default first.
Per texture:

- left, the shared texture panel (``sandover.materials.material_panel``): albedo, albedo with
  the emissive mask as an orange overlay (toggle, opacity), the mask alone, why the texture is
  emissive (source, E, ladder step, emitter score), where it appears, and its class/tags/note;
- right, the 3D viewer on a model drawing the texture with ``?bloom=1&emissive=<stem>&strength=<f>``
  (see ``viewer/static/bloom.js``); strength changes are posted to the iframe, not reloaded;
- bottom, the ladder: a log rail (``emissive_strengths.LADDER_MIN``..``LADDER_MAX``) with the
  anchors from ``emissive-maps/index.json`` as fixed marks (torch at 1.0), the texture as a
  draggable marker and its nearest neighbours as thumbnails at their current strengths.

Keys (``TAB_KEYS``, also the on-page legend): Up/Down nudge a quarter octave, R reset to the
default, Enter accept, S skip, B back, N not emissive, E is emissive, U undo the last N/E.
Every ladder change appends a row to ``emissive-strengths.csv`` at once (``source`` = ``human``,
or ``default`` for a reset or accepting the untouched default); N/E/U append to
``emissive-overrides.csv`` and the texture leaves (or rejoins) the queue immediately.
"""

from __future__ import annotations

import base64
import io
import json
import os
from html import escape
from pathlib import Path
from urllib.parse import quote

from PIL import Image

from sandover.paths import data_dir

os.environ.setdefault("GRADIO_ANALYTICS_ENABLED", "False")
os.environ.setdefault("GRADIO_SERVER_NAME", "127.0.0.1")
os.environ.setdefault("GRADIO_SHARE", "False")
os.environ.setdefault("GRADIO_TEMP_DIR", str(data_dir() / "gradio_tmp"))

import gradio as gr  # noqa: E402

from sandover.materials import emissive_strengths as es  # noqa: E402
from sandover.materials.emissive_maps import load_index, override_names  # noqa: E402
from sandover.materials.overrides import overrides_path, read_overrides  # noqa: E402
from sandover.materials.sheets import compose_over_checker, load_rgba, read_csv, textures_dir  # noqa: E402
from sandover.materials.table import csv_path  # noqa: E402
from sandover.materials.material_panel import (  # noqa: E402
    FLAG_KEYS,
    LADDER_KEYS,
    MaterialPanel,
    key_bindings,
    legend_html,
)
from sandover.materials.emitter_score import load_scores  # noqa: E402
from sandover.materials.viewer_link import fetch_models_using  # noqa: E402

THUMB_PX = 36

EMISSIVE_CSS = """
.em-ladder { position: relative; height: 170px; margin: 8px 24px 4px; user-select: none; touch-action: none; }
.em-rail { position: absolute; left: 0; right: 0; top: 116px; height: 6px; background: #3a3e46; border-radius: 3px; cursor: pointer; }
.em-tick { position: absolute; top: 126px; font-size: 11px; color: #9aa0aa; transform: translateX(-50%); }
.em-anchor { position: absolute; top: 100px; height: 38px; border-left: 2px solid #7fb2ff; }
.em-anchor span { position: absolute; left: 3px; font-size: 11px; color: #7fb2ff; white-space: nowrap; }
.em-anchor.torch { border-left-color: #ffcf3f; } .em-anchor.torch span { color: #ffcf3f; }
.em-nb-tick { position: absolute; top: 110px; height: 18px; border-left: 1px solid #bbb; opacity: 0.6; }
.em-nb { position: absolute; top: 0; transform: translateX(-50%); text-align: center; font-size: 10px; color: #bbb; }
.em-nb img { width: 36px; height: 36px; image-rendering: pixelated; border: 1px solid #555; display: block; margin: 0 auto; }
.em-marker { position: absolute; top: 104px; width: 14px; height: 30px; margin-left: -7px; background: #ff8c1a;
  border: 2px solid #fff; border-radius: 4px; cursor: grab; }
.em-marker-label { position: absolute; top: 148px; transform: translateX(-50%); font-weight: 600; color: #ffb35c; }
.em-default { position: absolute; top: 110px; height: 18px; border-left: 2px dashed #ff8c1a; opacity: 0.7; }
"""

# `element` / `trigger` / `watch` come from gr.HTML's js_on_load (gradio 6).
LADDER_JS = r"""
function isTyping(t) {
  if (!t || !t.tagName) return false;
  if (t.tagName === "TEXTAREA" || t.isContentEditable) return true;
  if (t.tagName !== "INPUT") return false;
  var ty = (t.type || "text").toLowerCase();
  return ["range", "checkbox", "radio", "button", "submit"].indexOf(ty) < 0;
}
function ladder() { return element.querySelector(".em-ladder"); }
function toStrength(pos, box) {
  var lo = Math.log(parseFloat(box.dataset.min)), hi = Math.log(parseFloat(box.dataset.max));
  return Math.exp(lo + Math.min(Math.max(pos, 0), 1) * (hi - lo));
}
function post(s) {
  var f = document.getElementById("emissiveFrame");
  if (f && f.contentWindow) f.contentWindow.postMessage({ type: "sandover-bloom-strength", strength: s }, "*");
}
function place(box, pos, s) {
  var m = box.querySelector(".em-marker"), l = box.querySelector(".em-marker-label");
  m.style.left = (pos * 100) + "%";
  l.style.left = (pos * 100) + "%";
  l.textContent = s.toFixed(3);
}
var drag = null;
element.addEventListener("pointerdown", function (e) {
  var box = ladder();
  if (!box || !box.contains(e.target)) return;
  var rail = box.querySelector(".em-rail").getBoundingClientRect();
  drag = { box: box, rail: rail };
  box.setPointerCapture(e.pointerId);
  move(e);
});
function move(e) {
  if (!drag) return;
  var pos = (e.clientX - drag.rail.left) / drag.rail.width;
  pos = Math.min(Math.max(pos, 0), 1);
  drag.s = toStrength(pos, drag.box);
  place(drag.box, pos, drag.s);
  post(drag.s);
}
element.addEventListener("pointermove", move);
element.addEventListener("pointerup", function (e) {
  if (!drag) return;
  var s = drag.s;
  drag = null;
  if (s) trigger("submit", { action: "set", strength: s });
});
watch("value", function () {
  var box = ladder();
  if (box) post(parseFloat(box.dataset.strength));
});
var keys = __KEYS__;
document.addEventListener("keydown", function (e) {
  if (e.ctrlKey || e.metaKey || e.altKey || isTyping(e.target)) return;
  if (element.offsetParent === null) return;
  var a = keys[e.key];
  if (!a) return;
  e.preventDefault();
  e.stopPropagation();
  trigger("submit", { action: a });
}, true);
"""


# ---- pure logic -----------------------------------------------------------------------------

def build_queue(index: dict, verdicts: dict[str, str], materials_rows: list[dict]) -> list[dict]:
    """Queue rows (name, tpage, combo_id, default, source, stats).

    ``verdicts`` is texture name -> latest override verdict (``emissive_maps.override_names``).
    A human ``emissive`` texture not yet in the maps index joins with source ``override`` at
    ``LADDER_MIN``. Strongest default first; :func:`load_state` then applies
    ``emissive_strengths.scored_queue`` when ``emitter-score.json`` exists.
    """
    by_tp = {(r.get("tpage", ""), r.get("name", "")): r.get("combo_id", "") for r in materials_rows}
    by_name: dict[str, dict] = {}
    for r in materials_rows:
        by_name.setdefault(r.get("name", ""), r)
    out = []
    textures = index.get("textures", {})
    for name, t in textures.items():
        if verdicts.get(name) == "not-emissive":
            continue
        if t.get("source") == "bake" and not t.get("coverage", 0) > 0 and verdicts.get(name) != "emissive":
            continue
        out.append({
            "name": name, "tpage": t.get("tpage", ""),
            "combo_id": by_tp.get((t.get("tpage", ""), name)) or by_name.get(name, {}).get("combo_id", ""),
            "default": float(t.get("default_strength", 0.0)) or es.LADDER_MIN,
            "source": t.get("source", ""), "role": t.get("role", ""), "levels": t.get("levels", []),
            "coverage": t.get("coverage", 0.0), "peak": t.get("peak", 0.0),
        })
    for name, v in sorted(verdicts.items()):
        if v == "emissive" and name not in textures and name in by_name:
            m = by_name[name]
            out.append({"name": name, "tpage": m.get("tpage", ""), "combo_id": m.get("combo_id", ""),
                        "default": es.LADDER_MIN, "source": "override", "role": "", "levels": [],
                        "coverage": 0.0, "peak": 0.0})
    out.sort(key=lambda r: (-r["default"], r["name"]))
    return out


def current_strength(row: dict, latest: dict[str, dict]) -> float:
    rec = latest.get(row["name"])
    if rec:
        try:
            return es.clamp_strength(float(rec["strength"]))
        except (KeyError, ValueError):
            pass
    return es.clamp_strength(row["default"])


def ladder_action(row: dict, strength: float, action: str, value: float | None = None) -> tuple[float, str | None, int]:
    """(new strength, CSV source to write or None, queue step) for one ladder action.

    ``set`` (a drag) and ``up``/``down`` write ``human``; ``reset`` writes ``default`` at the
    default; ``accept`` writes the current value (``default`` if it still equals the default,
    except for a scored receiver, ``row["emitter"] is False``, whose accepted rail-bottom
    default is written as ``human`` so export-sidecar applies it) and moves on;
    ``skip``/``back`` move without writing.
    """
    default = es.clamp_strength(row["default"])
    if action == "set" and value is not None:
        return es.clamp_strength(value), "human", 0
    if action == "up":
        return es.nudge(strength, 1), "human", 0
    if action == "down":
        return es.nudge(strength, -1), "human", 0
    if action == "reset":
        return default, "default", 0
    if action == "accept":
        same = abs(strength - default) < 1e-9 * default
        return strength, ("default" if same and row.get("emitter") is not False else "human"), 1
    if action == "skip":
        return strength, None, 1
    if action == "back":
        return strength, None, -1
    return strength, None, 0


def load_state(index_file: Path | None = None) -> tuple[dict, list[dict], dict[str, dict]]:
    """(maps index, queue, latest strength rows by name) from the files on disk."""
    index = load_index(index_file)
    verdicts = override_names(read_overrides(overrides_path()))
    rows = read_csv(csv_path()) if csv_path().is_file() else []
    scores = load_scores()
    queue = build_queue(index, verdicts, rows)
    if scores.get("textures"):
        queue = es.scored_queue(queue, scores)
    return index, queue, es.latest_strengths(es.read_strengths())


# ---- rendering ------------------------------------------------------------------------------

def _data_uri(im: Image.Image) -> str:
    buf = io.BytesIO()
    im.save(buf, "PNG")
    return "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode()


def _texture_png(tpage: str, name: str) -> Path:
    return textures_dir() / tpage / f"{name}.png"


def pick_model(models: list[dict], levels: list[str]) -> dict | None:
    """A background chunk from a level the texture's glow was baked in, else any background,
    else the first model (the viewer's own usefulness order)."""
    bg = [m for m in models if m.get("kind") == "background"]
    for m in bg:
        if any(m["id"].startswith(f"{lvl}/") for lvl in levels):
            return m
    return bg[0] if bg else (models[0] if models else None)


def bloom_url(viewer_port: int, model_id: str, stem: str, strength: float) -> str:
    return (f"http://127.0.0.1:{viewer_port}/?model={quote(model_id, safe='')}"
            f"&bloom=1&emissive={quote(stem, safe='')}&strength={strength:.4f}")


def viewer_panel_html(viewer_port: int, row: dict, strength: float) -> str:
    models = fetch_models_using(viewer_port, row["name"])
    if models is None:
        return (f'<div class="hint">3D: could not reach the viewer at 127.0.0.1:{viewer_port}. '
                f'Start it, or pass --viewer-port.</div>')
    m = pick_model(models, row.get("levels", []))
    if m is None:
        return '<div class="hint">no model uses this texture in the rip</div>'
    src = bloom_url(viewer_port, m["id"], row["name"], strength)
    return (f'<iframe id="emissiveFrame" src="{escape(src)}" title="bloom preview: {escape(row["name"])}" '
            f'style="width:100%;height:420px;border:1px solid #3a3e46;border-radius:6px;"></iframe>'
            f'<div class="hint">{escape(m["id"])} &middot; bloom is an approximation of the engine\'s</div>')


class ThumbCache:
    def __init__(self) -> None:
        self._c: dict[tuple[str, str], str] = {}

    def get(self, tpage: str, name: str) -> str:
        key = (tpage, name)
        if key not in self._c:
            png = _texture_png(tpage, name)
            if png.is_file():
                im = compose_over_checker(load_rgba(png))
                im.thumbnail((THUMB_PX, THUMB_PX), Image.NEAREST)
                self._c[key] = _data_uri(im)
            else:
                self._c[key] = ""
        return self._c[key]


def ladder_html(row: dict, strength: float, index: dict, queue: list[dict], latest: dict[str, dict],
                thumbs: ThumbCache) -> str:
    """The rail: power-of-two ticks, anchors, neighbour thumbnails, the default and the marker."""
    pct = lambda s: f"{es.rail_pos(s) * 100:.3f}%"  # noqa: E731
    parts = [f'<div class="em-ladder" data-min="{es.LADDER_MIN}" data-max="{es.LADDER_MAX}" '
             f'data-strength="{strength:.6f}">', '<div class="em-rail"></div>']
    s = es.LADDER_MIN
    while s <= es.LADDER_MAX * 1.0001:
        label = f"1/{round(1 / s)}" if s < 1 else f"{s:g}"
        parts.append(f'<div class="em-tick" style="left:{pct(s)}">{label}</div>')
        s *= 4
    anchors = dict(index.get("anchors", {}))
    ref = index.get("reference", {}).get("texture")
    if ref and "torch" not in anchors:
        anchors["torch"] = {"name": ref, "strength": 1.0}
    for i, (role, a) in enumerate(sorted(anchors.items(), key=lambda kv: float(kv[1]["strength"]))):
        cls = "em-anchor torch" if role == "torch" else "em-anchor"
        parts.append(f'<div class="{cls}" style="left:{pct(float(a["strength"]))}" '
                     f'title="{escape(a["name"])} {float(a["strength"]):.3f}">'
                     f'<span style="top:{-16 - 14 * (i % 2)}px">{escape(role)}</span></div>')
    by_name = {q["name"]: q for q in queue}
    current = {q["name"]: current_strength(q, latest) for q in queue}
    nbs = es.neighbours(current, row["name"], strength)
    for (n, v), x in zip(nbs, neighbour_slots(len(nbs), es.rail_pos(strength))):
        q = by_name[n]
        uri = thumbs.get(q["tpage"], n)
        img = f'<img src="{uri}" alt="">' if uri else ""
        parts.append(f'<div class="em-nb" style="left:{x * 100:.3f}%" title="{escape(n)} {v:.3f}">'
                     f'{img}{v:.2f}</div>')
        parts.append(f'<div class="em-nb-tick" style="left:{pct(v)}" title="{escape(n)}"></div>')
    parts.append(f'<div class="em-default" style="left:{pct(row["default"])}" title="default"></div>')
    parts.append(f'<div class="em-marker" style="left:{pct(strength)}"></div>')
    parts.append(f'<div class="em-marker-label" style="left:{pct(strength)}">{strength:.3f}</div>')
    parts.append("</div>")
    return "".join(parts)


def neighbour_slots(n: int, centre: float, gap: float = 0.08) -> list[float]:
    """Evenly spaced rail positions (0..1) for ``n`` neighbour thumbnails around ``centre``, so
    textures at the same strength don't stack; each also gets a tick at its true position."""
    if n <= 0:
        return []
    half = (n - 1) * gap / 2
    start = min(max(centre - half, 0.0), max(1.0 - 2 * half, 0.0))
    return [start + i * gap for i in range(n)]


def status_md(queue: list[dict], pos: int, strength: float, latest: dict[str, dict]) -> str:
    if not queue:
        return "no emissive textures: run `sandover-materials emissive-maps` first"
    row = queue[pos]
    rec = latest.get(row["name"])
    done = sum(1 for q in queue if q["name"] in latest)
    lv = ", ".join(row.get("levels", [])[:4]) or "-"
    return (f"{pos + 1}/{len(queue)} ({done} placed) &middot; **{row['name']}** &middot; tpage `{row['tpage']}` "
            f"&middot; default {row['default']:.3f} ({row['source']}, {row.get('role') or '-'}) &middot; "
            f"coverage {row.get('coverage', 0):.0%} &middot; levels {lv}  \n"
            f"strength **{strength:.3f}** x torch"
            + (f" &middot; last saved {rec['strength']} ({rec['source']})" if rec else " &middot; not saved yet"))


# ---- Gradio tab -----------------------------------------------------------------------------

TAB_KEYS = LADDER_KEYS + FLAG_KEYS
LADDER_JS = LADDER_JS.replace("__KEYS__", json.dumps(key_bindings(TAB_KEYS)))


def build_emissive_tab(rows_state: gr.State, queue_csv: Path | None, viewer_port: int = 8765,
                       capture_port: int = 7870, strengths_csv: Path | None = None) -> dict:
    """Wires the Emissive tab into an enclosing ``gr.Blocks`` / ``gr.Tab``.

    ``rows_state``/``queue_csv`` are the Review tab's queue, where the shared panel saves class,
    tags and note. Returns ``{"outputs", "load", "load_inputs", "select", "select_inputs"}``; the
    caller hooks ``load`` to ``demo.load`` and ``select`` to the tab's select event (a reload that
    picks up flags written elsewhere).
    """
    csv_file = strengths_csv or es.strengths_path()
    index, queue, latest = load_state()
    thumbs = ThumbCache()
    st = {"index": index, "queue": queue, "latest": latest}
    panel = MaterialPanel(viewer_port, capture_port, queue_csv)

    def view(pos: int, strength: float | None, rows: list[dict], show: bool, opacity: float,
             with_panels: bool):
        q = st["queue"]
        if not q:
            empty = '<div class="hint">no emissive textures</div>'
            return (0, 0.0, status_md(q, 0, 0.0, st["latest"]), empty, "",
                    *panel.render(None, rows, show, opacity))
        pos = max(0, min(pos, len(q) - 1))
        row = q[pos]
        s = current_strength(row, st["latest"]) if strength is None else strength
        v3d = viewer_panel_html(viewer_port, row, s) if with_panels else gr.skip()
        return (pos, s, status_md(q, pos, s, st["latest"]), v3d,
                ladder_html(row, s, st["index"], q, st["latest"], thumbs),
                *panel.render(row, rows, show, opacity, rank=(pos + 1, len(q)), images=with_panels))

    def reload_all():
        st["index"], st["queue"], st["latest"] = load_state()
        panel.data.reload()

    def act(pos, strength, rows, show, opacity, note, undo, action, value=None):
        q = st["queue"]
        if action in ("not-emissive", "emissive", "undo"):
            target = q[pos] if q and 0 <= pos < len(q) else None
            msg, undo, name = panel.do_flag(target, action, note, undo)
            reload_all()
            names = [r["name"] for r in st["queue"]]
            if action == "undo" and name in names:
                pos = names.index(name)
            return (*view(pos, None, rows, show, opacity, True), undo, msg)
        if not q:
            return (*view(0, None, rows, show, opacity, True), undo, gr.skip())
        row = q[pos]
        new_s, source, step = ladder_action(row, strength, action, value)
        if source:
            written = es.append_strength(csv_file, {
                "combo_id": row["combo_id"], "tpage": row["tpage"], "name": row["name"],
                "strength": new_s, "source": source, "note": note or ""})
            st["latest"][row["name"]] = written
            panel.data.strengths = st["latest"]
        if step:
            return (*view(pos + step, None, rows, show, opacity, True), undo, "")
        return (*view(pos, new_s, rows, show, opacity, False), undo, gr.skip())

    gr.Markdown("Every texture the bake (or a human) calls emissive. Left: the texture, where it glows, "
                "why, and where it is used; press **N** if it is only lit, not a light. Right: the glow "
                "in 3D. Below: its strength against the torch (1.0); every change is saved at once. "
                "See kb/howto/materials.md \"Emissive review\".")
    pos_state = gr.State(0)
    strength_state = gr.State(0.0)
    with gr.Row():
        status = gr.Markdown()
        goto_box = gr.Textbox(label="Go to texture", placeholder="texture name, then Enter", scale=0,
                              min_width=260)
    with gr.Row():
        with gr.Column(scale=2):
            panel.build_view()
        with gr.Column(scale=3):
            v3d_html = gr.HTML()
    ladder = gr.HTML(js_on_load=LADDER_JS)
    gr.HTML(legend_html(TAB_KEYS, ["drag the orange marker", "blue marks: anchors (torch = 1.0, gold)",
                                   "dashed line: the default"]))
    panel.build_controls()
    with gr.Row():
        down_btn = gr.Button("Down")
        up_btn = gr.Button("Up")
        reset_btn = gr.Button("Reset (R)")
        accept_btn = gr.Button("Accept (Enter)", variant="primary")
        skip_btn = gr.Button("Skip (S)")
        back_btn = gr.Button("< Back (B)")
        reload_btn = gr.Button("Reload maps / overrides")

    view_outputs = [pos_state, strength_state, status, v3d_html, ladder, *panel.view_outputs]
    outputs = [*view_outputs, panel.undo_state, panel.flash]
    inputs = [pos_state, strength_state, rows_state, panel.show_mask, panel.opacity, panel.note,
              panel.undo_state]
    for btn, a in ((down_btn, "down"), (up_btn, "up"), (reset_btn, "reset"), (accept_btn, "accept"),
                   (skip_btn, "skip"), (back_btn, "back"), (panel.not_btn, "not-emissive"),
                   (panel.is_btn, "emissive"), (panel.undo_btn, "undo")):
        btn.click(lambda *args, a=a: act(*args, a), inputs=inputs, outputs=outputs)

    def on_ladder(p, s, rows, show, opacity, note, undo, evt: gr.EventData):
        data = evt._data or {}
        try:
            v = float(data["strength"]) if "strength" in data else None
        except (TypeError, ValueError):
            v = None
        return act(p, s, rows, show, opacity, note, undo, str(data.get("action", "")), v)

    ladder.submit(on_ladder, inputs=inputs, outputs=outputs, trigger_mode="multiple")

    def on_reload(p, rows, show, opacity):
        reload_all()
        return view(p, None, rows, show, opacity, True)

    select_inputs = [pos_state, rows_state, panel.show_mask, panel.opacity]
    reload_btn.click(on_reload, inputs=select_inputs, outputs=view_outputs)
    for ctl in (panel.show_mask, panel.opacity):
        ctl.change(lambda p, s, rows, show, opacity: view(p, s, rows, show, opacity, True),
                   inputs=[pos_state, strength_state, rows_state, panel.show_mask, panel.opacity],
                   outputs=view_outputs)

    def on_goto(name, p, rows, show, opacity):
        name = (name or "").strip()
        names = [r["name"] for r in st["queue"]]
        if name in names:
            return (*view(names.index(name), None, rows, show, opacity, True), "")
        return (*view(p, None, rows, show, opacity, True),
                f"**{name}** is not on the ladder (not emissive, or marked not emissive)")

    goto_box.submit(on_goto, inputs=[goto_box, *select_inputs], outputs=[*view_outputs, panel.flash])

    def on_save(p, rows, cls, tags, note):
        q = st["queue"]
        return panel.do_save(q[p] if q and 0 <= p < len(q) else None, rows, cls, tags, note)

    panel.save_btn.click(on_save, inputs=[pos_state, rows_state, panel.cls, panel.tags, panel.note],
                         outputs=[rows_state, panel.flash])

    def first_unplaced() -> int:
        for i, q in enumerate(st["queue"]):
            if q["name"] not in st["latest"]:
                return i
        return 0

    def on_load(rows, show, opacity):
        return view(first_unplaced(), None, rows, show, opacity, True)

    return {"outputs": view_outputs, "load": on_load,
            "load_inputs": [rows_state, panel.show_mask, panel.opacity],
            "select": on_reload, "select_inputs": select_inputs}
