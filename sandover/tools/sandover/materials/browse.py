"""Browse tab: page through materials.csv by class, including three derived from emissive data.

The taxonomy classes (`sandover.materials.CLASSES`, from `materials.toml`) plus:

- ``emissive``: textures that ship emissive. A human override
  (`sandover.materials.overrides`, written by the viewer's inspect panel or the in-game
  inspect tool) always wins; otherwise the emitter score (`emitter-score.json`, see
  `sandover.materials.emitter_score` and `emissive_strengths.is_emitter`) says the texture is
  an emitter -- score at or above its threshold, a ladder anchor, or a human ladder emitter.
  A texture with no score entry (no baked E, so `export-sidecar` never scales it) falls back
  to the raw E table exceeding `EMISSIVE_THRESHOLD`, same as before the emitter score existed.
- ``receiver``: raw baked E over `EMISSIVE_THRESHOLD` (what ``emissive`` alone used to mean)
  but not shipping -- the emitter score's lit-by-neighbours set, scaled to 0 at export
  (`export-sidecar`'s `receiver_scale`, issue #171). Kept browsable so a wrongly-scored
  emitter can still be corrected with the N/E keys. Excludes overridden textures, which
  already have their own outcome either way.
- ``not-emissive-override``: a human override says ``not-emissive`` -- its own bucket so a
  batch of "table said yes, human said no" cases can be reviewed together.

Picking a class shows a paged thumbnail grid (`sandover.materials.sheets.render_texture`,
same alpha x2-over-checker convention as the main queue); clicking a tile shows that texture
in the shared panel (`sandover.materials.material_panel`): its emissive mask, why it is
emissive, where it appears, N/E/U flags and its class/tags/note. "Open in Review queue" jumps
the Review tab's queue to it, adding it first if it wasn't there (see `jump_to_texture`).
"""

from __future__ import annotations

import os
from pathlib import Path

from sandover.paths import data_dir

# Same hardening as sandover.materials.review; must land before `import gradio`
# in case this module is imported on its own (e.g. from a test) rather than via review.py.
os.environ.setdefault("GRADIO_ANALYTICS_ENABLED", "False")
os.environ.setdefault("GRADIO_SERVER_NAME", "127.0.0.1")
os.environ.setdefault("GRADIO_SHARE", "False")
os.environ.setdefault("GRADIO_TEMP_DIR", str(data_dir() / "gradio_tmp"))

import gradio as gr  # noqa: E402

from sandover.materials import CLASSES  # noqa: E402
from sandover.materials.emissive_strengths import is_emitter  # noqa: E402
from sandover.materials.emissive_table import EMISSIVE_THRESHOLD, load_table, table_path  # noqa: E402
from sandover.materials.emitter_score import load_scores  # noqa: E402
from sandover.materials.overrides import (  # noqa: E402
    latest_verdicts,
    override_key,
    overrides_path,
    read_overrides,
)
from sandover.materials.queue import jump_to_texture  # noqa: E402,F401 -- re-exported
from sandover.materials.material_panel import FLAG_KEYS, MaterialPanel, keys_js, legend_html  # noqa: E402
from sandover.materials.sheets import read_csv, render_texture  # noqa: E402
from sandover.materials.table import csv_path  # noqa: E402

EMISSIVE = "emissive"
RECEIVER = "receiver"
NOT_EMISSIVE_OVERRIDE = "not-emissive-override"
PAGE_SIZE = 60

CLASS_LABELS = {EMISSIVE: "emissive (ships)", RECEIVER: "receiver (E scaled to 0 at export)"}


# ---- pure logic (unit-tested without gradio or a display) ---------------------------------

def browse_classes() -> tuple[str, ...]:
    """Taxonomy classes plus the three derived from the emissive table/score/overrides."""
    return (*CLASSES, EMISSIVE, RECEIVER, NOT_EMISSIVE_OVERRIDE)


def class_label(cls: str) -> str:
    """Display label for a browse class: the derived classes spell out what they mean."""
    return CLASS_LABELS.get(cls, cls)


def ships_emissive(row: dict, verdicts: dict[str, str], e_table: dict[str, float], scores: dict,
                   threshold: float = EMISSIVE_THRESHOLD) -> bool:
    """A texture ships emissive at export. A human override always wins (``export-sidecar``
    forces E to 1 or 0 from it); otherwise the emitter score decides (`emissive_strengths.
    is_emitter`: score at or above its threshold, a ladder anchor, or a human ladder emitter).
    A texture with no ``emitter-score.json`` entry (no baked E, so the export never scales it)
    falls back to the raw E table exceeding ``threshold``."""
    verdict = verdicts.get(override_key(row))
    if verdict == "not-emissive":
        return False
    if verdict == "emissive":
        return True
    name = row.get("name", "")
    rec = (scores or {}).get("textures", {}).get(name)
    if rec is not None:
        return is_emitter(rec, float((scores or {}).get("threshold", 0.5)))
    return e_table.get(name, 0.0) > threshold


def is_receiver(row: dict, verdicts: dict[str, str], e_table: dict[str, float], scores: dict,
                threshold: float = EMISSIVE_THRESHOLD) -> bool:
    """Raw baked E over ``threshold`` but not shipping emissive (:func:`ships_emissive`) -- the
    lit-by-neighbours set the emitter score scales to 0 at export. Overridden textures are
    excluded: they already have their own outcome, in ``emissive`` or ``not-emissive-override``."""
    if verdicts.get(override_key(row)):
        return False
    if e_table.get(row.get("name", ""), 0.0) <= threshold:
        return False
    return not ships_emissive(row, verdicts, e_table, scores, threshold)


def class_members(rows: list[dict], cls: str, verdicts: dict[str, str], e_table: dict[str, float],
                  scores: dict | None = None, threshold: float = EMISSIVE_THRESHOLD) -> list[dict]:
    """materials.csv rows in browse class ``cls``, in CSV order."""
    scores = scores or {}
    if cls == EMISSIVE:
        return [r for r in rows if ships_emissive(r, verdicts, e_table, scores, threshold)]
    if cls == RECEIVER:
        return [r for r in rows if is_receiver(r, verdicts, e_table, scores, threshold)]
    if cls == NOT_EMISSIVE_OVERRIDE:
        return [r for r in rows if verdicts.get(override_key(r)) == "not-emissive"]
    return [r for r in rows if r.get("class", "") == cls]


def paginate(rows: list[dict], page: int, page_size: int = PAGE_SIZE) -> tuple[list[dict], int, int]:
    """(rows on this page, clamped page index, total page count >= 1)."""
    n_pages = max(1, -(-len(rows) // page_size)) if page_size > 0 else 1
    page = max(0, min(page, n_pages - 1))
    start = page * page_size
    return rows[start:start + page_size], page, n_pages


def tile_caption(row: dict) -> str:
    conf = row.get("confidence", "") or "-"
    return f"{row.get('name', '?')}  ({conf})"


def load_browse_data(materials_csv: Path | None = None, e_table_path: Path | None = None,
                      overrides_csv: Path | None = None, scores_path: Path | None = None
                      ) -> tuple[list[dict], dict[str, float], dict[str, str], dict]:
    """(materials.csv rows, E table, latest override verdicts, emitter-score.json as loaded);
    empty/default where a file is missing."""
    rows = read_csv(materials_csv or csv_path()) if (materials_csv or csv_path()).is_file() else []
    e_table = load_table(e_table_path or table_path())
    verdicts = latest_verdicts(read_overrides(overrides_csv or overrides_path()))
    scores = load_scores(scores_path)
    return rows, e_table, verdicts, scores


# ---- Gradio tab -----------------------------------------------------------------------

BROWSE_KEYS = FLAG_KEYS


def build_browse_tab(rows_state: gr.State, pos_state: gr.State, review_outputs: list,
                      refresh_fn, tabs: gr.Tabs, review_tab_id: str, viewer_port: int = 8765,
                      queue_csv: Path | None = None, hub_port: int = 7870) -> dict:
    """Wires the Browse tab into an existing ``gr.Blocks``.

    A tile click shows the texture in the shared panel (``material_panel``) under the grid,
    where N/E/U flag it and class/tags/note are saved to the review queue (``queue_csv``).
    "Open in Review queue" does what a tile click used to: updates ``rows_state``/``pos_state``
    and ``review_outputs`` (the Review tab's queue and outputs, see ``build_app``) and switches
    ``tabs`` to ``review_tab_id``. Returns ``{"select", "select_inputs", "select_outputs"}`` for
    the caller to hook to the tab's select event (reloads the data).
    """
    all_rows, e_table, verdicts, scores = load_browse_data()
    panel = MaterialPanel(viewer_port, hub_port, queue_csv)

    def render(cls: str, page: int):
        members = class_members(all_rows, cls, verdicts, e_table, scores)
        page_rows, page, n_pages = paginate(members, page)
        items = [(render_texture(r)[0], tile_caption(r)) for r in page_rows]
        label = f"{len(members)} texture(s) in **{class_label(cls)}**, page {page + 1}/{n_pages}"
        return items, page_rows, page, label

    items0, page_rows0, page0, label0 = render(EMISSIVE, 0)

    gr.Markdown("Pick a class; click a tile to see why it is (or is not) emissive and edit it below.")
    with gr.Row():
        class_dd = gr.Dropdown([(class_label(c), c) for c in browse_classes()], value=EMISSIVE, label="Class")
        reload_btn = gr.Button("Reload materials.csv / emissive table / score / overrides")
    page_md = gr.Markdown(label0)
    with gr.Row():
        prev_btn = gr.Button("< Prev page")
        page_state = gr.State(page0)
        next_btn = gr.Button("Next page >")
    gallery = gr.Gallery(value=items0, show_label=False, columns=8, height=480, allow_preview=False)
    page_rows_state = gr.State(page_rows0)
    target_state = gr.State(None)
    key_bridge = gr.HTML(legend_html(BROWSE_KEYS, ["click a tile to select it"]),
                         js_on_load=keys_js(BROWSE_KEYS))
    with gr.Row():
        with gr.Column(scale=3):
            panel.build_view()
        with gr.Column(scale=2):
            panel.build_controls()
            open_review_btn = gr.Button("Open in Review queue")

    render_outputs = [gallery, page_rows_state, page_state, page_md]
    panel_inputs = [rows_state, panel.show_mask, panel.opacity]

    def reload_data():
        nonlocal all_rows, e_table, verdicts, scores
        all_rows, e_table, verdicts, scores = load_browse_data()
        panel.data.reload()

    class_dd.change(render, inputs=[class_dd, page_state], outputs=render_outputs)
    prev_btn.click(lambda c, p: render(c, p - 1), inputs=[class_dd, page_state], outputs=render_outputs)
    next_btn.click(lambda c, p: render(c, p + 1), inputs=[class_dd, page_state], outputs=render_outputs)

    def on_reload(cls, page, target, rows, show, opacity):
        reload_data()
        return (*render(cls, page), *panel.render(target, rows, show, opacity))

    reload_inputs = [class_dd, page_state, target_state, *panel_inputs]
    reload_outputs = [*render_outputs, *panel.view_outputs]
    reload_btn.click(on_reload, inputs=reload_inputs, outputs=reload_outputs)

    def on_select(page_rows, rows, show, opacity, evt: gr.SelectData):
        if not page_rows or evt.index is None or not (0 <= evt.index < len(page_rows)):
            return (gr.skip(), *([gr.skip()] * len(panel.view_outputs)))
        target = page_rows[evt.index]
        return (target, *panel.render(target, rows, show, opacity))

    gallery.select(on_select, inputs=[page_rows_state, *panel_inputs],
                   outputs=[target_state, *panel.view_outputs])
    for ctl in (panel.show_mask, panel.opacity):
        ctl.change(lambda t, rows, show, opacity: panel.render(t, rows, show, opacity),
                   inputs=[target_state, *panel_inputs], outputs=panel.view_outputs)

    def on_flag(action, cls, page, target, rows, show, opacity, note, undo):
        msg, undo, name = panel.do_flag(target, action, note, undo)
        reload_data()
        if action == "undo" and name and (not target or target.get("name") != name):
            target = next((r for r in all_rows if r.get("name") == name), target)
        return (*render(cls, page), target, *panel.render(target, rows, show, opacity), undo, msg)

    flag_inputs = [class_dd, page_state, target_state, *panel_inputs, panel.note, panel.undo_state]
    flag_outputs = [*render_outputs, target_state, *panel.view_outputs, panel.undo_state, panel.flash]
    for btn, a in ((panel.not_btn, "not-emissive"), (panel.is_btn, "emissive"), (panel.undo_btn, "undo")):
        btn.click(lambda c, p, t, r, s, o, n, u, a=a: on_flag(a, c, p, t, r, s, o, n, u),
                  inputs=flag_inputs, outputs=flag_outputs)

    def on_key(c, p, t, r, s, o, n, u, evt: gr.EventData):
        action = str((evt._data or {}).get("action", ""))
        if action not in {a for _, a, _ in BROWSE_KEYS}:
            return tuple(gr.skip() for _ in flag_outputs)
        return on_flag(action, c, p, t, r, s, o, n, u)

    key_bridge.submit(on_key, inputs=flag_inputs, outputs=flag_outputs, trigger_mode="multiple")

    panel.save_btn.click(lambda t, rows, c, tg, n: panel.do_save(t, rows, c, tg, n),
                         inputs=[target_state, rows_state, panel.cls, panel.tags, panel.note],
                         outputs=[rows_state, panel.flash])

    def on_open_review(target, rows):
        if not target:
            return (*([gr.skip()] * len(review_outputs)), gr.skip())
        new_rows, pos = jump_to_texture(rows, target)
        return (new_rows, pos, *refresh_fn(new_rows, pos), gr.Tabs(selected=review_tab_id))

    open_review_btn.click(on_open_review, inputs=[target_state, rows_state],
                          outputs=[*review_outputs, tabs])

    return {"select": on_reload, "select_inputs": reload_inputs, "select_outputs": reload_outputs}
