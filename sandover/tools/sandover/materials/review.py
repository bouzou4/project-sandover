"""``sandover-review-materials``: one texture at a time, keyboard-driven (kb/howto/materials.md).

Gradio, hardened: analytics and the version
check disabled, no font CDN, loopback only, a project-local temp dir. Every answer is
written to the CSV immediately, so closing the page loses nothing. "Finish" folds
``human_class`` into ``materials.csv`` and the FiftyOne dataset via the existing ``apply``
path, then reports human/judge agreement per class.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from html import escape as html_escape
from pathlib import Path

from sandover.paths import data_dir

# Must land before `import gradio`: it reads these at import and at Blocks() time.
os.environ.setdefault("GRADIO_ANALYTICS_ENABLED", "False")
os.environ.setdefault("GRADIO_SERVER_NAME", "127.0.0.1")
os.environ.setdefault("GRADIO_SHARE", "False")
os.environ.setdefault("GRADIO_TEMP_DIR", str(data_dir() / "gradio_tmp"))

import gradio as gr  # noqa: E402

from sandover.materials import (  # noqa: E402
    CLASS_DESCRIPTIONS,
    CLASS_KEYS,
    CLASSES,
    TAG_DESCRIPTIONS,
    TAG_KEYS,
    TAGS,
)
from sandover.materials import browse, emissive_review, material_panel, table  # noqa: E402
from sandover.materials.queue import (  # noqa: E402
    agreement_by_class,
    agreement_markdown,
    apply_action,
    changed_rows,
    clamp,
    commit,
    first_unanswered,
    load_queue,
    merge_needs_review,
    progress,
    record,
    row_tags,
    save_note,
    sort_queue_by_confidence,
    toggle_tags,
    write_queue,
)
from sandover.materials.sheets import (  # noqa: E402
    alpha_thumbnail,
    compose_over_checker,
    fit_for_review,
    render_texture,
)
from sandover.materials.viewer_link import fetch_models_using, viewer_url  # noqa: E402

# Plain strings only -- keeps the page off the Google Fonts CDN.
THEME = gr.themes.Default(
    font=["ui-sans-serif", "system-ui", "sans-serif"],
    font_mono=["ui-monospace", "Consolas", "monospace"],
)
CUSTOM_CSS = (".gradio-container { background: #222226 !important; }\n" + emissive_review.EMISSIVE_CSS
              + material_panel.PANEL_CSS)

# skip / back / toggle-context, and the emissive flags N/E/U (Enter is not a single char)
_RESERVED_KEYS = {"s", "b", "v", "n", "e", "u"}


def _case_variants(by_key: dict[str, str]) -> dict[str, str]:
    """Both cases of every key -- ``materials.toml`` keys are matched case-insensitively."""
    out: dict[str, str] = {}
    for key, value in by_key.items():
        out[key.upper()] = value
        out[key.lower()] = value
    return out


_all_keys = {k.lower() for k in (*CLASS_KEYS.values(), *TAG_KEYS.values())}
if _all_keys & _RESERVED_KEYS:
    raise ValueError(f"materials.toml: class/tag key collides with reviewer's reserved keys "
                      f"{sorted(_RESERVED_KEYS)}: {sorted(_all_keys & _RESERVED_KEYS)}")

# CLASS_KEYS/TAG_KEYS come from materials.toml (sandover.materials.load_taxonomy): keys 1-6/9/0
# are the eight original classes, 7/8 the tags, and any new class or tag gets a spare letter.
KEY_ACTION: dict[str, str] = _case_variants({key: cls for cls, key in CLASS_KEYS.items()})
KEY_ACTION.update({"s": "skip", "S": "skip", "b": "back", "B": "back", "Enter": "accept"})
KEY_ACTION.update(_case_variants({key: f"toggle:{tag}" for tag, key in TAG_KEYS.items()}))

KEY_LEGEND_HTML = (
    "keys: " + " &middot; ".join(
        [f'<kbd>{CLASS_KEYS[c]}</kbd> <span title="{html_escape(CLASS_DESCRIPTIONS[c])}">{c}</span>'
         for c in CLASSES]
        + [f'<kbd>{TAG_KEYS[t]}</kbd> <span title="{html_escape(TAG_DESCRIPTIONS[t])}">toggle {t}</span>'
           for t in TAGS]
        + ["<kbd>Enter</kbd> accept judges' class", "<kbd>S</kbd> skip", "<kbd>B</kbd> back",
           "<kbd>V</kbd> toggle 3D context"]
    )
)

# Ignored while typing in the note field; capture phase so a focused button can't eat it.
# "V" toggles the Context panel entirely client-side (no row state to change).
_JS_KEYS = sorted(set(KEY_ACTION) - {"Enter"} | {"s", "S", "b", "B"}) + ["Enter"]
REVIEW_JS_ON_LOAD_TEMPLATE = """
function isTyping(t) {
  if (!t || !t.tagName) return false;
  if (t.tagName === "TEXTAREA" || t.isContentEditable) return true;
  if (t.tagName !== "INPUT") return false;
  var ty = (t.type || "text").toLowerCase();
  return ["range", "checkbox", "radio", "button", "submit"].indexOf(ty) < 0;
}
var keys = __KEYS__;
document.addEventListener("keydown", function (e) {
  if (e.ctrlKey || e.metaKey || e.altKey) return;
  if (isTyping(e.target)) return;
  if (element.offsetParent === null) return;  // another tab is showing
  if (keys.indexOf(e.key) < 0) return;
  e.preventDefault();
  trigger("submit", { key: e.key });
}, true);
document.addEventListener("keydown", function (e) {
  if (e.ctrlKey || e.metaKey || e.altKey) return;
  if (isTyping(e.target)) return;
  if (element.offsetParent === null) return;
  if (e.key !== "v" && e.key !== "V") return;
  e.preventDefault();
  var el = document.getElementById("contextPanel");
  if (el) el.style.display = (el.style.display === "none") ? "" : "none";
}, true);
"""
REVIEW_JS_ON_LOAD = REVIEW_JS_ON_LOAD_TEMPLATE.replace("__KEYS__", json.dumps(_JS_KEYS))


# ---- FiftyOne glue (queue/CSV logic itself lives in sandover.materials.queue) -------------

def fetch_needs_review_candidates() -> list[dict]:
    """Rows for every sample in the FiftyOne ``needs-review`` saved view (table.load_fiftyone)."""
    from sandover.materials.sheets import hex_id

    _, ds = table._dataset()
    view = ds.load_saved_view("needs-review")
    combo, tpage, name, cls, conf, agree = view.values(
        ["combo_id", "tpage_name", "name", "material_class", "material_confidence", "judge_agreement"])
    return [
        {"combo_id": hex_id(c), "tpage": tp, "name": n, "judged_class": cl,
         "confidence": f"{cf:.3f}", "judge_agreement": ag}
        for c, tp, n, cl, cf, ag in zip(combo, tpage, name, cls, conf, agree)
    ]


def ordered_queue(csv_path: Path, order: str = "confidence") -> list[dict]:
    rows = load_queue(csv_path) if csv_path.is_file() else []
    return sort_queue_by_confidence(rows) if order == "confidence" else rows


def answer(csv_path: Path, rows: list[dict], pos: int, note: str, action: str) -> tuple[list[dict], int]:
    """One key/button press, saved at once: (rows refreshed from the CSV, new position)."""
    new_rows, new_pos = apply_action(rows, pos, note, action)
    if new_rows is rows:
        return rows, new_pos
    return commit(csv_path, new_rows, changed_rows(rows, new_rows)), new_pos


def row_status_md(row: dict, true_size: tuple[int, int]) -> str:
    return (f"**{row['name']}**  &middot;  tpage `{row['tpage']}`  &middot;  "
            f"{true_size[0]}x{true_size[1]} (true size)")


def judge_status_md(row: dict) -> str:
    agree = row.get("judge_agreement", "") or "-"
    conf = row.get("confidence", "") or "-"
    return f"judges: **{row.get('judged_class', '?')}**  (confidence {conf}, {agree})"


def tags_status_md(row: dict) -> str:
    tags = row_tags(row)
    return "tags: " + (", ".join(t for t in TAGS if t in tags) if tags else "none")


# ---- 3D context panel (I/O: talks to the viewer server) -----------------------------------

def render_context_html(viewer_port: int, row: dict) -> str:
    """iframe on the first (most useful) model using this texture, links to swap in the rest."""
    stem = row.get("name", "")
    models = fetch_models_using(viewer_port, stem) if stem else None
    if models is None:
        return (f'<div class="hint">Context: could not reach the viewer at '
                f'127.0.0.1:{viewer_port}. Start it, or pass --viewer-port.</div>')
    if not models:
        return '<div class="hint">no model uses this texture in the rip</div>'
    first_src = viewer_url(viewer_port, models[0]["id"], stem)
    # onclick is single-quoted: json.dumps below always double-quotes its JS string, so the two
    # never collide (unlike the src="..." attribute, which does need double-quote escaping).
    links = " &middot; ".join(
        f"<a href=\"#\" onclick='document.getElementById(\"contextFrame\").src="
        f"{json.dumps(viewer_url(viewer_port, m['id'], stem))};return false;'>{html_escape(m['id'])}</a>"
        for m in models[1:]
    ) or "(no other models use it)"
    return (
        f'<iframe id="contextFrame" src="{html_escape(first_src)}" title="3D context: {html_escape(stem)}" '
        f'style="width:100%;height:440px;border:1px solid #3a3e46;border-radius:6px;"></iframe>'
        f'<div class="hint" style="margin-top:4px;">other models: {links}</div>'
    )


# ---- Gradio app -----------------------------------------------------------------------

def build_app(csv_path: Path, rows: list[dict], viewer_port: int = 8765, hub_port: int = 7870,
              order: str = "confidence") -> gr.Blocks:
    def refresh(rows: list[dict], pos: int):
        if not rows:
            return "0/0", "no rows left", "", "tags: none", None, None, "", \
                '<div class="hint">no rows left</div>'
        pos = clamp(pos, len(rows))
        row = rows[pos]
        main, alpha, size = render_texture(row)
        return (progress(rows), row_status_md(row, size), judge_status_md(row), tags_status_md(row),
                main, alpha, row.get("human_note", ""), render_context_html(viewer_port, row))

    def do_action(rows: list[dict], pos: int, note: str, action: str):
        new_rows, new_pos = answer(csv_path, rows, pos, note, action)
        return (new_rows, new_pos, *refresh(new_rows, new_pos))

    def do_finish(rows: list[dict]):
        table.apply_corrections(csv_path, from_fiftyone=False)
        table.load_fiftyone()
        return agreement_markdown(load_queue(csv_path))

    def on_load():
        rows = ordered_queue(csv_path, order)
        pos = first_unanswered(rows)
        return (rows, pos, *refresh(rows, pos))

    with gr.Blocks(analytics_enabled=False, title="Material spot-check reviewer") as demo:
        gr.Markdown("## Material spot-check reviewer")
        rows_state = gr.State(rows)
        pos_state = gr.State(first_unanswered(rows))

        with gr.Tabs() as tabs:
            with gr.Tab("Review", id="review"):
                progress_md = gr.Markdown()
                name_md = gr.Markdown()
                judge_md = gr.Markdown()
                tags_md = gr.Markdown()
                with gr.Row():
                    main_img = gr.Image(show_label=False, interactive=False, buttons=[])
                    alpha_img = gr.Image(show_label=False, interactive=False, buttons=[],
                                          label="alpha (grey)")
                note_box = gr.Textbox(
                    label="Context for the judges", lines=2,
                    info="Guides a correction and rejudge --with-notes; a class is optional here.")

                with gr.Row():
                    class_btns = [gr.Button(f"{CLASS_KEYS[c]}. {c}") for c in CLASSES]
                with gr.Row():
                    tag_btns = [gr.Button(f"{TAG_KEYS[t]}. toggle {t}") for t in TAGS]
                with gr.Row():
                    accept_btn = gr.Button("Accept judges' class (Enter)")
                    skip_btn = gr.Button("Skip (S)")
                    back_btn = gr.Button("< Back (B)")
                    note_btn = gr.Button("Save context only (no class)")
                key_bridge = gr.HTML(KEY_LEGEND_HTML, js_on_load=REVIEW_JS_ON_LOAD)

                with gr.Accordion("Context (3D viewer)", open=True, elem_id="contextPanel"):
                    gr.Markdown("Focus this panel steals keyboard shortcuts (`V` toggles it back "
                                "off); see kb/howto/materials.md.")
                    context_html = gr.HTML()

                finish_btn = gr.Button("Finish: apply corrections", variant="primary")
                finish_md = gr.Markdown()

            outputs = [rows_state, pos_state, progress_md, name_md, judge_md, tags_md, main_img,
                       alpha_img, note_box, context_html]

            with gr.Tab("Browse", id="browse") as browse_tab:
                browse_hooks = browse.build_browse_tab(rows_state, pos_state, outputs, refresh, tabs,
                                                       "review", viewer_port=viewer_port,
                                                       queue_csv=csv_path, hub_port=hub_port)

            with gr.Tab("Emissive", id="emissive") as emissive_tab:
                emissive = emissive_review.build_emissive_tab(rows_state, csv_path, viewer_port=viewer_port,
                                                              hub_port=hub_port)

        # Re-read overrides on entering a tab, so a flag set in the other tab or the viewer shows.
        browse_tab.select(browse_hooks["select"], inputs=browse_hooks["select_inputs"],
                          outputs=browse_hooks["select_outputs"])
        emissive_tab.select(emissive["select"], inputs=emissive["select_inputs"], outputs=emissive["outputs"])

        for i, cls in enumerate(CLASSES):
            class_btns[i].click(
                lambda rows, pos, note, cls=cls: do_action(rows, pos, note, cls),
                inputs=[rows_state, pos_state, note_box], outputs=outputs)
        for i, t in enumerate(TAGS):
            tag_btns[i].click(
                lambda rows, pos, note, t=t: do_action(rows, pos, note, f"toggle:{t}"),
                inputs=[rows_state, pos_state, note_box], outputs=outputs)
        accept_btn.click(lambda rows, pos, note: do_action(rows, pos, note, "accept"),
                          inputs=[rows_state, pos_state, note_box], outputs=outputs)
        skip_btn.click(lambda rows, pos, note: do_action(rows, pos, note, "skip"),
                        inputs=[rows_state, pos_state, note_box], outputs=outputs)
        back_btn.click(lambda rows, pos, note: do_action(rows, pos, note, "back"),
                        inputs=[rows_state, pos_state, note_box], outputs=outputs)
        note_btn.click(lambda rows, pos, note: do_action(rows, pos, note, "note"),
                        inputs=[rows_state, pos_state, note_box], outputs=outputs)

        def on_key(rows, pos, note, evt: gr.EventData):
            action = KEY_ACTION.get((evt._data or {}).get("key", ""))
            if not action:
                return (rows, pos, *refresh(rows, pos))
            return do_action(rows, pos, note, action)

        key_bridge.submit(on_key, inputs=[rows_state, pos_state, note_box], outputs=outputs,
                          trigger_mode="multiple")  # queue fast key presses; "once" drops them

        finish_btn.click(do_finish, inputs=[rows_state], outputs=[finish_md])

        # Every page load starts from the CSV, not the queue as it was when the server started.
        demo.load(on_load, outputs=outputs).then(
            emissive["load"], inputs=emissive["load_inputs"], outputs=emissive["outputs"])

    return demo


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="sandover-review-materials", description=__doc__.splitlines()[0])
    ap.add_argument("--csv", required=True, type=Path)
    ap.add_argument("--all-needs-review", action="store_true",
                     help="extend the queue with the FiftyOne needs-review view")
    ap.add_argument("--port", type=int, default=7864)
    ap.add_argument("--viewer-port", type=int, default=8765,
                     help="3D viewer server for the Context panel (sandover-viewer)")
    ap.add_argument("--hub-port", type=int, default=7870,
                     help="static server over $SANDOVER_DATA that capture links in the emissive panel open")
    ap.add_argument("--order", choices=["confidence", "file"], default="confidence",
                     help="queue order: least confident (disagreements first) or CSV order")
    args = ap.parse_args(argv)

    if args.all_needs_review:
        write_queue(args.csv, merge_needs_review(load_queue(args.csv), fetch_needs_review_candidates()))
    rows = ordered_queue(args.csv, args.order)

    demo = build_app(args.csv, rows, viewer_port=args.viewer_port, hub_port=args.hub_port,
                     order=args.order)
    demo.launch(
        theme=THEME,
        css=CUSTOM_CSS,
        share=False,
        server_name="127.0.0.1",
        server_port=args.port,
        inbrowser=False,
        show_error=True,
        allowed_paths=[],
        footer_links=[],
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
