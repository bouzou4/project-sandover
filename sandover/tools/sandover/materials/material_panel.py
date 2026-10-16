"""One-texture panel shared by the reviewer's Browse and Emissive tabs.

Everything about one texture on one panel, so it can be judged and edited without changing tabs:

- the albedo, the albedo with its emissive mask (``emissive-maps/<tpage>/<name>.png``) as an
  orange overlay (toggle, opacity), and the mask alone; a missing mask is said in the panel;
- why it is (or is not) emissive: the verdict, the E source (sidecar effect slot, night light,
  static glow, luminance from a human override), E max and coverage, the ladder step, and the
  emitter score (``emitter-score.json``) with its components when that file exists;
- where it appears: the levels its E was baked in, the viewer models that draw it (links open
  the viewer with that model loaded), and rig captures of those levels (thumbnails linking to
  the full image; a capture is matched by level, so the texture may be out of frame);
- class, tags and note, saved to the review queue CSV like the Review tab's keys;
- flags: ``N`` not emissive, ``E`` is emissive, ``U`` undo. Each appends a row to
  ``emissive-overrides.csv`` through :func:`sandover.materials.overrides.append_override`, the
  same writer the viewer's ``POST /api/mark`` uses; undo appends the key's previous verdict.
"""

from __future__ import annotations

import base64
import io
import json
import math
import os
from html import escape
from pathlib import Path

import numpy as np
from PIL import Image

from sandover.paths import data_dir

os.environ.setdefault("GRADIO_ANALYTICS_ENABLED", "False")
os.environ.setdefault("GRADIO_SERVER_NAME", "127.0.0.1")
os.environ.setdefault("GRADIO_SHARE", "False")
os.environ.setdefault("GRADIO_TEMP_DIR", str(data_dir() / "gradio_tmp"))

import gradio as gr  # noqa: E402

from sandover.materials import CLASSES, TAGS  # noqa: E402
from sandover.materials import emissive_strengths as es  # noqa: E402
from sandover.materials.emissive_maps import COVERED, load_index, maps_dir, override_names  # noqa: E402
from sandover.materials.emissive_table import EMISSIVE_THRESHOLD, load_table, table_path  # noqa: E402
from sandover.materials.emitter_score import load_scores  # noqa: E402
from sandover.materials.overrides import (  # noqa: E402
    append_override,
    latest_verdicts,
    override_key,
    overrides_path,
    read_overrides,
)
from sandover.materials.queue import changed_rows, commit, jump_to_texture, row_tags  # noqa: E402
from sandover.materials.sheets import (  # noqa: E402
    compose_over_checker,
    fit_for_review,
    load_rgba,
    read_csv,
    textures_dir,
)
from sandover.materials.table import csv_path  # noqa: E402
from sandover.materials.viewer_link import fetch_models_using, viewer_url  # noqa: E402

OVERLAY_RGB = (255, 140, 0)
CAPTURE_THUMB_W = 360
MAX_MODELS = 6
EMITTER_ROLES = {"flames", "light", "lava", "fade-lights"}

# (key, what it does); the keydown handlers are built from these lists so legend and keys agree.
FLAG_KEYS: list[tuple[str, str, str]] = [
    ("N", "not-emissive", "not emissive: drops it from the emissive class and the ladder"),
    ("E", "emissive", "is emissive: for a light the bake missed; adds it to the emissive class and "
                      "the ladder (export forces full E on it)"),
    ("U", "undo", "undo the last N / E"),
]
LADDER_KEYS: list[tuple[str, str, str]] = [
    ("Up", "up", "brighter (quarter octave)"),
    ("Down", "down", "dimmer"),
    ("R", "reset", "reset to the default strength"),
    ("Enter", "accept", "accept and go to the next"),
    ("S", "skip", "skip"),
    ("B", "back", "back"),
]
JS_KEY_NAMES = {"Up": ["ArrowUp"], "Down": ["ArrowDown"], "Enter": ["Enter"]}

ROLE_TEXT = {
    "night": "night light: brighter at night than moonlight explains, in the bake (a lamp, "
             "or a surface a lamp lights)",
    "static": "static glow: lit at every hour in the bake (self-lit, or next to a constant light)",
}


# ---- pure logic -----------------------------------------------------------------------------

def key_bindings(entries: list[tuple[str, str, str]]) -> dict[str, str]:
    """JS ``KeyboardEvent.key`` -> action, both cases for letters."""
    out: dict[str, str] = {}
    for key, action, _ in entries:
        for k in JS_KEY_NAMES.get(key, [key.upper(), key.lower()] if len(key) == 1 else [key]):
            out[k] = action
    return out


def legend_html(entries: list[tuple[str, str, str]], extra: list[str] = ()) -> str:
    items = [f"<kbd>{escape(k)}</kbd> {escape(d)}" for k, _, d in entries] + list(extra)
    return '<div class="hint em-legend">keys: ' + " &middot; ".join(items) + "</div>"


def keys_js(entries: list[tuple[str, str, str]]) -> str:
    """``js_on_load`` body: triggers ``submit`` with ``{action}`` while this tab is visible."""
    return """
function isTyping(t) {
  if (!t || !t.tagName) return false;
  if (t.tagName === "TEXTAREA" || t.isContentEditable) return true;
  if (t.tagName !== "INPUT") return false;
  var ty = (t.type || "text").toLowerCase();
  return ["range", "checkbox", "radio", "button", "submit"].indexOf(ty) < 0;
}
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
""".replace("__KEYS__", json.dumps(key_bindings(entries)))


def verdict_for_key(rows: list[dict], key: str) -> str:
    return latest_verdicts(rows).get(key, "")


def verdict_for(target: dict, rows: list[dict]) -> str:
    """The verdict the reviewer acts on: this row's own key, else any row for the same name."""
    latest = latest_verdicts(rows)
    return (latest.get(override_key(target)) or latest.get(target.get("name", ""))
            or override_names(rows).get(target.get("name", ""), ""))


def override_row(target: dict, verdict: str, note: str) -> dict:
    return {"combo_id": target.get("combo_id", ""), "tpage": target.get("tpage", ""),
            "name": target.get("name", ""), "level": "", "draw_id": "reviewer",
            "verdict": verdict, "note": note or ""}


def flag(path: Path, target: dict, verdict: str, note: str, undo: list[dict]) -> tuple[dict, list[dict]]:
    """Append ``verdict`` for ``target``; returns (row written, undo stack with its prior verdict)."""
    row = override_row(target, verdict, note)
    prior = verdict_for_key(read_overrides(path), override_key(row))
    written = append_override(path, row)
    return written, [*undo, {"row": written, "prior": prior}]


def undo_last(path: Path, undo: list[dict]) -> tuple[dict | None, list[dict]]:
    """Append the previous verdict (``""`` clears) for the last flag; (row written or None, stack)."""
    if not undo:
        return None, undo
    last = undo[-1]
    row = {**last["row"], "verdict": last["prior"], "note": f"undo {last['row'].get('verdict', '')}"}
    return append_override(path, row), undo[:-1]


def ladder_step(strength: float) -> int:
    """Quarter-octave steps from the torch (1.0): negative is dimmer."""
    return round(math.log(es.clamp_strength(strength)) / math.log(es.NUDGE))


def source_text(t: dict | None) -> str:
    if not t:
        return "no emissive map"
    if t.get("source") == "luminance":
        return "a human override (mask from the texture's own bright texels, not from the bake)"
    role = t.get("role", "") or "static"
    if role in ROLE_TEXT:
        return ROLE_TEXT[role]
    kind = ("a light-source slot: it sits on or next to that emitter" if role in EMITTER_ROLES
            else "light arriving from outside: usually a lit surface, not a light")
    return f"sidecar effect slot '{role}' ({kind})"


def emissive_reason(name: str, verdict: str, index: dict, e_table: dict[str, float],
                    scores: dict | None = None) -> dict:
    """Why ``name`` is or is not emissive, as plain data (see :func:`why_markdown`).

    ``scores`` is ``emitter-score.json`` as loaded (``emitter_score.load_scores``) or None.
    """
    t = index.get("textures", {}).get(name)
    e = float(e_table.get(name, 0.0))
    baked = bool(t and t.get("source") == "bake" and t.get("coverage", 0) > 0)
    if verdict == "not-emissive":
        emissive, because = False, "a human marked it not emissive"
    elif verdict == "emissive":
        emissive, because = True, "a human marked it emissive"
    elif baked:
        emissive, because = True, "the bake: " + source_text(t)
    elif e > EMISSIVE_THRESHOLD:
        emissive, because = True, f"the bake: sidecar E {e:.2f} > {EMISSIVE_THRESHOLD}, no map rasterized"
    else:
        emissive, because = False, f"no baked E above {EMISSIVE_THRESHOLD}"
    return {"name": name, "emissive": emissive, "because": because, "verdict": verdict,
            "source": source_text(t) if t else "", "role": (t or {}).get("role", ""),
            "e_max": e, "coverage": float((t or {}).get("coverage", 0.0)),
            "default": float((t or {}).get("default_strength", 0.0)),
            "in_index": t is not None, "score": (scores or {}).get("textures", {}).get(name),
            "threshold": float((scores or {}).get("threshold", 0.5))}


def why_markdown(reason: dict, strength_rec: dict | None, rank: tuple[int, int] | None,
                 mask_exists: bool) -> str:
    state = "EMISSIVE" if reason["emissive"] else "NOT EMISSIVE"
    lines = [f"### {state}", f"**Why:** {escape(reason['because'])}."]
    if reason["verdict"] and reason["source"]:
        lines.append(f"Bake source (overridden): {escape(reason['source'])}.")
    lines.append(f"E max **{reason['e_max']:.2f}** &middot; coverage **{reason['coverage']:.0%}** of texels "
                 f"at E >= {COVERED}")
    sc = reason.get("score")
    if sc:
        comps = ", ".join(f"{escape(str(k))} {v:.2f}" if isinstance(v, (int, float)) else
                          f"{escape(str(k))} {escape(str(v))}" for k, v in sc.get("components", {}).items())
        verdict = "emitter" if es.is_emitter(sc, reason["threshold"]) else "lit receiver"
        extra = "".join(f", {k} {escape(str(sc[k]))}" for k in ("anchor", "human") if sc.get(k))
        lines.append(f"Emitter score **{float(sc.get('score', 0)):.2f}** -> **{verdict}** "
                     f"(threshold {reason['threshold']:g}{extra})" + (f": {comps}" if comps else ""))
    else:
        lines.append("Emitter score: none for this texture (`emitter-score.json` absent or not scored)")
    if reason["emissive"]:
        if strength_rec or reason["default"]:
            s = float(strength_rec["strength"]) if strength_rec else es.clamp_strength(reason["default"])
            who = f"placed by {strength_rec['source']}" if strength_rec else "default, not placed yet"
            pos = f" &middot; queue {rank[0]}/{rank[1]}" if rank else ""
            lines.append(f"Ladder: **{s:.3f} x torch** (step {ladder_step(s):+d} from the torch), {who}{pos}")
        else:
            lines.append("Ladder: not on it yet (run `sandover-materials emissive-maps --reindex`)")
    if not mask_exists:
        lines.append("**No emissive mask** for this texture: " + (
            "a human marked it emissive after the last `emissive-maps` run; `--reindex` makes one."
            if reason["verdict"] == "emissive" else
            "the bake gave it no E on a tfrag/tie draw (shrub and actor draws are not mapped)."
            if not reason["emissive"] else "the sidecar table has E but no map was rasterized."))
    if reason["role"] in {"night", "static"} or (reason["role"] and reason["role"] not in EMITTER_ROLES):
        lines.append("_Night and static sources also mark surfaces a lamp lights. If only the lamp "
                     "should glow, press **N**._")
    return "  \n".join(lines)


def find_captures(root: Path | None = None) -> dict[str, list[dict]]:
    """level -> rig captures (``{"png", "continue", "hour"}``) from the newest golden of each set
    and from ``shots/``, matched by the capture's continue point (``<level>-...``)."""
    base = root or data_dir()
    jsons: list[Path] = []
    for s in sorted((base / "goldens").glob("*")):
        shas = [d for d in s.iterdir() if d.is_dir() and "." not in d.name] if s.is_dir() else []
        if shas:
            jsons += sorted(max(shas, key=lambda d: d.stat().st_mtime).glob("*.json"))
    jsons += sorted((base / "shots").glob("*/*.json"))
    out: dict[str, list[dict]] = {}
    for j in jsons:
        png = j.with_suffix(".png")
        if not png.is_file():
            continue
        try:
            meta = json.loads(j.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        cont = meta.get("continue") if isinstance(meta, dict) else None
        if not cont:
            continue
        out.setdefault(cont.split("-")[0], []).append(
            {"png": png, "continue": cont, "hour": float(meta.get("hour", 12.0))})
    return out


def captures_for(levels: list[str], captures: dict[str, list[dict]], n: int = 2) -> list[dict]:
    """Up to ``n`` captures of the first level that has any: night first, then noon."""
    for lvl in levels:
        caps = captures.get(lvl, [])
        if caps:
            night = min(caps, key=lambda c: (min(c["hour"], 24 - c["hour"]), c["png"].name))
            day = min(caps, key=lambda c: (abs(c["hour"] - 12), c["png"].name))
            return [night] + ([day] if day is not night else [])[: n - 1]
    return []


def target_edit_state(target: dict, rows: list[dict]) -> tuple[str, list[str], str, str]:
    """(human class or "", tags, note, judge's class) for ``target`` from the queue, else materials.csv."""
    combo = target.get("combo_id", "")
    q = next((r for r in rows if combo and r.get("combo_id") == combo), None)
    judged = target.get("class", "") or target.get("judged_class", "") or (q or {}).get("judged_class", "")
    if q:
        return q.get("human_class", ""), [t for t in TAGS if t in row_tags(q)], q.get("human_note", ""), judged
    return "", [t for t in TAGS if t in row_tags(target)], "", judged


def save_edit(rows: list[dict], target: dict, cls: str, tags: list[str], note: str) -> tuple[list[dict], int]:
    """Queue rows with ``target``'s class/tags/note set (queued first if needed); pure."""
    from datetime import datetime, timezone

    new_rows, pos = jump_to_texture(rows, target)
    new_rows = [dict(r) for r in new_rows]
    r = new_rows[pos]
    if cls in CLASSES:
        r["human_class"] = cls
    r["tags"] = ";".join(t for t in TAGS if t in set(tags or []))
    r["human_note"] = note or ""
    r["timestamp"] = datetime.now(timezone.utc).isoformat(timespec="seconds")
    return new_rows, pos


# ---- rendering ------------------------------------------------------------------------------

def _data_uri(im: Image.Image, fmt: str = "PNG") -> str:
    buf = io.BytesIO()
    im.save(buf, fmt)
    return f"data:image/{fmt.lower()};base64," + base64.b64encode(buf.getvalue()).decode()


def mask_path(tpage: str, name: str) -> Path:
    return maps_dir() / tpage / f"{name}.png"


def mask_overlay(emap: Image.Image, opacity: float) -> Image.Image:
    """Mask (L, E x 255) -> RGBA: ``OVERLAY_RGB`` wherever E >= ``COVERED``, at ``opacity``."""
    a = np.asarray(emap.convert("L")).astype(np.float32) / 255.0
    rgba = np.zeros((*a.shape, 4), np.uint8)
    rgba[..., :3] = OVERLAY_RGB
    rgba[..., 3] = np.where(a >= COVERED, round(255 * min(max(opacity, 0.0), 1.0)), 0)
    return Image.fromarray(rgba, "RGBA")


def images_html(target: dict, show_mask: bool, opacity: float) -> str:
    tpage, name = target.get("tpage", ""), target.get("name", "")
    png = textures_dir() / tpage / f"{name}.png"
    if not png.is_file():
        return f'<div class="hint">texture not found: {escape(tpage)}/{escape(name)}</div>'
    raw = load_rgba(png)
    disp = fit_for_review(raw)
    base = compose_over_checker(disp)
    mp = mask_path(tpage, name)
    figs = [("albedo", base)]
    if mp.is_file():
        with Image.open(mp) as em:
            m = em.convert("L").resize(disp.size, Image.NEAREST)
        over = base.convert("RGBA")
        if show_mask:
            over = Image.alpha_composite(over, mask_overlay(m, opacity))
        figs.append(("orange = counted emissive (E >= %.1f)" % COVERED if show_mask else "overlay off", over))
        figs.append(("mask alone: white E 1.0, black 0", m.convert("RGB")))
        missing = ""
    else:
        missing = ('<div class="em-missing">no emissive mask on disk for this texture '
                   f'(<code>emissive-maps/{escape(tpage)}/{escape(name)}.png</code>)</div>')
    cells = "".join(f'<figure><img alt="{escape(cap)}" src="{_data_uri(im)}"><figcaption>{escape(cap)}'
                    f'</figcaption></figure>' for cap, im in figs)
    return (f'<div class="em-panel-imgs">{cells}</div>{missing}'
            f'<div class="hint">{escape(name)} &middot; tpage {escape(tpage)} &middot; '
            f'{raw.size[0]}x{raw.size[1]}</div>')


class CaptureThumbs:
    def __init__(self) -> None:
        self._c: dict[Path, str] = {}

    def get(self, png: Path) -> str:
        if png not in self._c:
            try:
                with Image.open(png) as im:
                    im = im.convert("RGB")
                    im.thumbnail((CAPTURE_THUMB_W, CAPTURE_THUMB_W), Image.BILINEAR)
                    self._c[png] = _data_uri(im, "JPEG")
            except OSError:
                self._c[png] = ""
        return self._c[png]


def where_html(target: dict, levels: list[str], viewer_port: int, hub_port: int,
               captures: dict[str, list[dict]], thumbs: CaptureThumbs) -> str:
    name = target.get("name", "")
    parts = ['<div class="em-where"><b>Where it appears</b><br>',
             f'levels (baked E): {escape(", ".join(levels)) if levels else "none recorded"}<br>']
    models = fetch_models_using(viewer_port, name) if name else None
    if models is None:
        parts.append(f'<span class="hint">viewer not reachable on 127.0.0.1:{viewer_port}</span><br>')
    elif not models:
        parts.append('<span class="hint">no viewer model draws it</span><br>')
    else:
        links = " &middot; ".join(
            f'<a href="{escape(viewer_url(viewer_port, m["id"], name))}" target="_blank" rel="noopener">'
            f'{escape(m["id"])}</a>' for m in models[:MAX_MODELS])
        more = f" (+{len(models) - MAX_MODELS} more)" if len(models) > MAX_MODELS else ""
        parts.append(f"drawn by: {links}{more}<br>")
    caps = captures_for(levels, captures)
    if caps:
        root = data_dir()
        for c in caps:
            rel = c["png"].relative_to(root).as_posix()
            uri = thumbs.get(c["png"])
            parts.append(f'<figure class="em-cap"><a href="http://127.0.0.1:{hub_port}/{escape(rel)}" '
                         f'target="_blank" rel="noopener"><img alt="capture" src="{uri}"></a>'
                         f'<figcaption>{escape(c["continue"])} at {c["hour"]:g}h (whole level shot; '
                         f'the texture may be out of frame)</figcaption></figure>')
    else:
        parts.append('<span class="hint">no rig capture of these levels; use a viewer link above '
                     'to see the texture in its level.</span>')
    parts.append("</div>")
    return "".join(parts)


PANEL_CSS = """
.em-panel-imgs { display: flex; gap: 8px; flex-wrap: wrap; }
.em-panel-imgs figure { margin: 0; flex: 1 1 30%; min-width: 140px; }
.em-panel-imgs img { width: 100%; image-rendering: pixelated; display: block; }
.em-panel-imgs figcaption, .em-cap figcaption { font-size: 11px; color: #9aa0aa; }
.em-missing { margin-top: 6px; padding: 6px 8px; border: 1px dashed #ff8c1a; color: #ffb35c; border-radius: 4px; }
.em-cap { margin: 6px 8px 0 0; display: inline-block; width: 48%; vertical-align: top; }
.em-cap img { width: 100%; display: block; }
.em-legend kbd { padding: 0 4px; border: 1px solid #666; border-radius: 3px; }
"""


# ---- data and Gradio component ----------------------------------------------------------------

class PanelData:
    """Everything the panel reads from disk; ``reload()`` re-reads it all."""

    def __init__(self) -> None:
        self.reload()

    def reload(self) -> None:
        self.index = load_index()
        self.e_table = load_table(table_path())
        self.override_rows = read_overrides(overrides_path())
        self.strengths = es.latest_strengths(es.read_strengths())
        self.scores = load_scores()
        self.captures = find_captures()
        mats = read_csv(csv_path()) if csv_path().is_file() else []
        self.materials = {(r.get("tpage", ""), r.get("name", "")): r for r in mats}

    def material_row(self, target: dict) -> dict:
        return {**self.materials.get((target.get("tpage", ""), target.get("name", "")), {}), **target}


class MaterialPanel:
    """The shared panel's components. Call :meth:`build_view` and :meth:`build_controls` inside the
    enclosing layout; ``view_outputs`` and :meth:`render` go together."""

    def __init__(self, viewer_port: int, hub_port: int, queue_csv: Path | None, data: PanelData | None = None):
        self.viewer_port, self.hub_port, self.queue_csv = viewer_port, hub_port, queue_csv
        self.data = data or PanelData()
        self.thumbs = CaptureThumbs()
        self.overrides_csv = overrides_path()

    def build_view(self) -> None:
        self.images = gr.HTML()
        with gr.Row():
            self.show_mask = gr.Checkbox(True, label="Show emissive mask overlay")
            self.opacity = gr.Slider(0.1, 1.0, value=0.6, step=0.05, label="Overlay opacity")
        self.why = gr.Markdown()
        self.where = gr.HTML()

    def build_controls(self) -> None:
        with gr.Row():
            self.cls = gr.Dropdown([("(not set)", "")] + [(c, c) for c in CLASSES], value="",
                                   label="Class")
            self.tags = gr.CheckboxGroup(list(TAGS), label="Tags")
        self.note = gr.Textbox(label="Note (kept with class/tags, and with N/E flags)", lines=1)
        with gr.Row():
            self.not_btn = gr.Button("Not emissive (N)", variant="stop")
            self.is_btn = gr.Button("Is emissive (E)")
            self.undo_btn = gr.Button("Undo flag (U)")
            self.save_btn = gr.Button("Save class / tags / note", variant="primary")
        self.flash = gr.Markdown()
        self.undo_state = gr.State([])

    @property
    def view_outputs(self) -> list:
        return [self.images, self.why, self.where, self.cls, self.tags, self.note]

    def render(self, target: dict | None, rows: list[dict], show_mask: bool, opacity: float,
               rank: tuple[int, int] | None = None, images: bool = True) -> tuple:
        if not target:
            empty = '<div class="hint">nothing selected</div>'
            return empty, "", "", gr.update(value="", label="Class"), [], ""
        d = self.data
        t = d.material_row(target)
        name = t.get("name", "")
        reason = emissive_reason(name, verdict_for(t, d.override_rows), d.index, d.e_table, d.scores)
        if "default" in target:
            reason["default"] = float(target["default"])
        mask_ok = mask_path(t.get("tpage", ""), name).is_file()
        why = why_markdown(reason, d.strengths.get(name), rank, mask_ok)
        if not images:
            return gr.skip(), why, gr.skip(), gr.skip(), gr.skip(), gr.skip()
        levels = d.index.get("textures", {}).get(name, {}).get("levels", []) or \
            [x for x in t.get("levels", "").split(";") if x]
        cls, tags, note, judged = target_edit_state(t, rows)
        if not note:
            note = (d.strengths.get(name) or {}).get("note", "")
        return (images_html(t, show_mask, opacity), why,
                where_html(t, levels, self.viewer_port, self.hub_port, d.captures, self.thumbs),
                gr.update(value=cls, label=f"Class (judges said: {judged or '-'})"), tags, note)

    def do_flag(self, target: dict | None, action: str, note: str,
                undo: list[dict]) -> tuple[str, list[dict], str | None]:
        """(message, undo stack, name to show) for N / E / U; reloads the overrides it wrote."""
        if action == "undo":
            row, undo = undo_last(self.overrides_csv, undo)
            msg = (f"undid: **{row['name']}** is back to "
                   f"{row['verdict'] or 'the bake (no override)'}" if row else "nothing to undo")
            name = row["name"] if row else None
        elif target:
            t = self.data.material_row(target)
            d = self.data
            verdict = verdict_for(t, d.override_rows)
            if verdict == action:
                return f"**{t.get('name', '')}** is already marked {action.replace('-', ' ')}", undo, None
            baked = emissive_reason(t.get("name", ""), "", d.index, d.e_table)["emissive"]
            if action == "emissive" and not verdict and baked:
                return (f"**{t.get('name', '')}** is already emissive from the bake; nothing written "
                        "(E forces full E at export, so it is for lights the bake missed)"), undo, None
            row, undo = flag(self.overrides_csv, t, action, note, undo)
            msg = f"saved: **{row['name']}** is {row['verdict'].replace('-', ' ')} (U to undo)"
            name = row["name"]
        else:
            return "select a texture first", undo, None
        self.data.override_rows = read_overrides(self.overrides_csv)
        return msg, undo, name

    def do_save(self, target: dict | None, rows: list[dict], cls: str, tags: list[str], note: str):
        if not target or self.queue_csv is None:
            return rows, "select a texture first"
        new_rows, _ = save_edit(rows, self.data.material_row(target), cls, tags, note)
        new_rows = commit(self.queue_csv, new_rows, changed_rows(rows, new_rows))
        return new_rows, f"saved class/tags/note for **{target.get('name', '')}** to the review queue"
