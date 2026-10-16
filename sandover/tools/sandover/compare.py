"""Pairwise A/B compare view for one shot: wipe, flicker, synced zoom/pan, enlarge.

The pure helpers below (side selection, cycling, path resolution) are unit-tested
directly. ``compare_html`` renders the markup for a ``gr.HTML`` component -- no external
assets, and it reuses the wipe-slider CSS from ``sandover.report`` (same look, same
``--pos`` mechanism) so the two views stay visually and mechanically in sync.

``COMPARE_JS_ON_LOAD`` is passed to ``gr.HTML(..., js_on_load=...)`` by a page embedding
the compare view instead of an inline ``<script>`` tag: Gradio's ``gr.HTML`` sets its content via
``innerHTML``, which browsers never execute ``<script>`` tags from, but ``js_on_load``
runs once against the component's own element and uses event delegation so it keeps
working across every shot/side change (which replaces the element's children).

Images are served through Gradio's own ``/gradio_api/file=`` route (same origin as the
embedding page, scoped by ``allowed_paths`` to the round directory), not as
absolute file:// links. ``side_image_path`` resolves straight to the manifest's own
candidate/vanilla PNG -- there is no thumbnail, resize or re-encode step in this module,
so the wipe, flicker and zoom panes are always the original capture at native resolution.
Only the contact sheet (``sandover.sheets``) is a downscaled JPEG, by design.
"""

from __future__ import annotations

import html
import json
import urllib.parse
from pathlib import Path
from string import Template

from sandover.report import SLIDER_CSS
from sandover.rounds import RoundManifest

VANILLA = "vanilla"


def side_options(manifest: RoundManifest) -> list[str]:
    """Selectable sides for a compare pane: vanilla (if present) then letters, sorted."""
    opts = [VANILLA] if manifest.vanilla else []
    return opts + sorted(manifest.candidates)


def default_sides(manifest: RoundManifest) -> tuple[str, str]:
    """Vanilla vs. the anchor by default, falling back to the first two options."""
    opts = side_options(manifest)
    if not opts:
        raise ValueError("round has no candidates to compare")
    left = VANILLA if VANILLA in opts else opts[0]
    anchor = manifest.anchor_letter
    right = anchor if anchor in opts and anchor != left else next(
        (o for o in opts if o != left), left)
    return left, right


LEFT, RIGHT = "left", "right"


def side_caption(manifest: RoundManifest, side: str) -> str:
    """The candidate's one-line visible-difference caption, or "" (never a preset name).

    A caption equal to the preset name, or the anchor's plain "faithful", adds nothing and
    is dropped.
    """
    cand = manifest.candidates.get(side)
    if cand is None:
        return ""
    label = (cand.label or "").strip()
    if label.lower() in {cand.preset.lower(), "faithful"}:
        return ""
    return label


def side_display(manifest: RoundManifest, side: str) -> str:
    """Letter (with the anchor mark) plus its caption, e.g. ``C — tight contact shadows``."""
    caption = side_caption(manifest, side)
    base = side_label(manifest, side)
    return f"{base} — {caption}" if caption else base


def side_choices(manifest: RoundManifest, sides: list[str] | None = None) -> list[tuple[str, str]]:
    """``(display, value)`` pairs for a Gradio Dropdown/Radio: shows captions, returns letters."""
    return [(side_display(manifest, s), s) for s in (side_options(manifest) if sides is None else sides)]


def key_to_side(key: str, options: list[str]) -> str | None:
    """``v`` is vanilla, a candidate letter is that letter; anything else is not a side key."""
    if len(key) != 1:
        return None
    if key.lower() == "v" and VANILLA in options:
        return VANILLA
    return key.upper() if key.upper() in options else None


def toggle_active(active: str) -> str:
    """Tab flips which pane the side keys drive."""
    return LEFT if active == RIGHT else RIGHT


def assign_side(left: str, right: str, active: str, side: str) -> tuple[str, str]:
    """Put ``side`` on the active pane; the other pane is untouched."""
    return (side, right) if active == LEFT else (left, side)


def step_shot(idx: int, n_shots: int, delta: int) -> int:
    """Previous/next shot, wrapping around."""
    return (idx + delta) % n_shots


def apply_compare_event(options: list[str], n_shots: int, idx: int, left: str, right: str,
                        last_seq: int, data: dict | None) -> tuple[int, str, str, int] | None:
    """Validate a client sync event (``{idx, left, right, seq}``) into ``(idx, left, right, seq)``.

    The page owns the compare state and sends it whole, so only the newest event matters:
    one whose ``seq`` is not above ``last_seq`` arrived out of order and yields None.
    Unknown sides or a bad index keep the current value.
    """
    data = data or {}
    try:
        seq = int(data["seq"])
    except (KeyError, TypeError, ValueError):
        seq = last_seq
    else:
        if seq <= last_seq:
            return None
    new_left = data.get("left") if data.get("left") in options else left
    new_right = data.get("right") if data.get("right") in options else right
    try:
        new_idx = int(data.get("idx")) % n_shots
    except (TypeError, ValueError):
        new_idx = idx
    return new_idx, new_left, new_right, seq


def side_image_path(manifest: RoundManifest, side: str, shot: str) -> Path:
    """Resolve 'vanilla' or a candidate letter to its image path for a shot."""
    if side == VANILLA:
        return manifest.vanilla_path(shot)
    return manifest.image_path(side, shot)


def side_label(manifest: RoundManifest, side: str) -> str:
    """Letters only, no scores or preset names -- the anchor is marked."""
    if side == VANILLA:
        return "vanilla"
    return f"{side} (faithful)" if side == manifest.anchor_letter else side


def file_url(path: Path) -> str:
    """Gradio's local-file route: same-origin, no external request."""
    return "/gradio_api/file=" + urllib.parse.quote(str(path), safe="/")


_COMPARE_TEMPLATE = Template("""
<div class="cmp-root" data-render="${nonce}" data-idx="${idx}" data-left="${left}" data-right="${right}"
     data-active="${active}" data-shots="${shots_json}" data-labels="${labels_json}">
<style>
${slider_css}
.cmp-root { width: 100%; box-sizing: border-box; }
.cmp-root .slider { max-width: none; width: 100%; }
.cmp-legend { display: flex; flex-wrap: wrap; gap: 4px 18px; font-size: 13px; color: #bbb;
              margin-bottom: 8px; }
.cmp-legend .item { display: inline-flex; gap: 6px; align-items: baseline; }
.cmp-legend kbd, .cmp-keys kbd { font-family: ui-monospace, Consolas, monospace; color: #eee;
              border: 1px solid #555; border-radius: 3px; padding: 0 5px; background: #2a2a2e; }
.cmp-legend .item.anchor kbd { border-color: #e0b050; color: #e0b050; }
.cmp-legend .on { font-size: 11px; color: #6cf; font-weight: bold; }
.cmp-labels { display: flex; justify-content: space-between; gap: 12px; font-size: 13px;
              color: #999; margin-bottom: 4px; }
.cmp-labels .pane { padding: 2px 8px; border: 1px solid transparent; border-radius: 4px;
                    cursor: pointer; }
.cmp-labels .pane.active { border-color: #6cf; color: #fff; }
.cmp-shot { color: #ccc; font-size: 13px; text-align: center; flex: 1; }
.cmp-keys { color: #888; font-size: 12px; margin-top: 6px; line-height: 1.9; }
.cmp-stage { position: absolute; inset: 0; transform-origin: center center; }
.cmp-root.enlarged { position: fixed; inset: 0; width: 100vw; height: 100vh; z-index: 10000;
                     margin: 0; background: #111; padding: 8px; box-sizing: border-box;
                     display: flex; flex-direction: column; }
.cmp-root.enlarged .slider { display: flex; flex-direction: column; flex: 1; min-height: 0; }
.cmp-root.enlarged .frame { flex: 1; min-height: 0; aspect-ratio: auto; }
</style>
<div class="cmp-legend">${legend}</div>
<div class="cmp-labels">
  <span class="pane pane-left">left: <b class="lbl-left">${left_lbl}</b></span>
  <span class="cmp-shot">${shot_lbl}</span>
  <span class="pane pane-right">right: <b class="lbl-right">${right_lbl}</b></span>
</div>
<div class="slider">
  <div class="frame">
    <div class="cmp-stage">
      <img class="left" src="${left_url}" alt="left">
      <div class="right-wrap">
        <img class="right" src="${right_url}" alt="right">
      </div>
      <div class="divider"></div>
    </div>
  </div>
  <div class="controls">
    <input type="range" class="drag" min="0" max="100" value="50">
    <button type="button" class="cycle-btn cmp-flicker-btn">Flicker</button>
    <button type="button" class="cycle-btn cmp-actual-btn">1:1</button>
    <button type="button" class="cycle-btn cmp-enlarge-btn">Enlarge</button>
  </div>
  <div class="cmp-keys">${keymap}</div>
</div>
</div>
""")


def keymap_html(manifest: RoundManifest) -> str:
    """The on-screen key map; the letter range comes from the round's own candidates."""
    letters = sorted(manifest.candidates)
    span = (f"<kbd>{letters[0]}</kbd>–<kbd>{letters[-1]}</kbd>" if len(letters) > 1
            else f"<kbd>{letters[0]}</kbd>")
    parts = ["<kbd>V</kbd> vanilla"] if manifest.vanilla else []
    parts += [
        f"{span} letter on the active side",
        "<kbd>Tab</kbd> switch active side",
        "<kbd>[</kbd> <kbd>]</kbd> or <kbd>←</kbd> <kbd>→</kbd> prev/next shot",
        "<kbd>Space</kbd> flicker",
        "wheel or <kbd>+</kbd> <kbd>-</kbd> zoom",
        "<kbd>1</kbd> actual size (1:1 pixels)",
        "<kbd>0</kbd> reset",
        "<kbd>Enter</kbd> enlarge",
        "<kbd>Esc</kbd> close",
    ]
    return "keys: " + " &middot; ".join(parts)


def legend_html(manifest: RoundManifest) -> str:
    """One chip per side: its key, the anchor mark and the caption. JS adds L/R markers."""
    items = []
    for side in side_options(manifest):
        if side == VANILLA:
            key, text = "V", "vanilla"
        else:
            mark = "(faithful)" if side == manifest.anchor_letter else ""
            key, text = side, " ".join(t for t in (mark, side_caption(manifest, side)) if t)
        cls = "item anchor" if side == manifest.anchor_letter else "item"
        items.append(f'<span class="{cls}" data-side="{html.escape(side)}">'
                     f'<kbd>{html.escape(key)}</kbd><span>{html.escape(text)}</span>'
                     f'<span class="on"></span></span>')
    return "".join(items)


def shot_caption(idx: int, n_shots: int, shot: str) -> str:
    return f"shot {idx + 1} of {n_shots}: {shot}"


def compare_html(manifest: RoundManifest, shot: str, left: str, right: str,
                 active: str = "", nonce: int = 0) -> str:
    """Wipe/flicker/zoom compare markup for one shot's two chosen sides.

    Every shot's and side's URL rides along in ``data-shots`` so the key map switches
    sides and shots in the page with no round trip. ``active`` ("left"/"right") moves the
    key focus; "" keeps the page's. ``nonce`` makes each server render distinct, since the
    page may have moved on since the last one and an identical value is not re-sent.
    Pairs with ``COMPARE_JS_ON_LOAD``.
    """
    opts = side_options(manifest)
    shots = [{"name": s, "caption": shot_caption(i, len(manifest.shots), s),
              "urls": {o: file_url(side_image_path(manifest, o, s)) for o in opts}}
             for i, s in enumerate(manifest.shots)]
    labels = {o: side_display(manifest, o) for o in opts}
    idx = manifest.shots.index(shot)
    return _COMPARE_TEMPLATE.substitute(
        nonce=nonce,
        idx=idx,
        left=html.escape(left),
        right=html.escape(right),
        active=html.escape(active),
        shots_json=html.escape(json.dumps(shots), quote=True),
        labels_json=html.escape(json.dumps(labels), quote=True),
        legend=legend_html(manifest),
        keymap=keymap_html(manifest),
        shot_lbl=html.escape(shots[idx]["caption"]),
        left_url=shots[idx]["urls"][left],
        right_url=shots[idx]["urls"][right],
        left_lbl=html.escape(labels[left]),
        right_lbl=html.escape(labels[right]),
        slider_css=SLIDER_CSS,
    )


# Runs once on mount. The page owns shot/side state and syncs it to the server (seq-numbered,
# newest wins); a server re-render (dropdown or shot button) is adopted wholesale in load().
COMPARE_JS_ON_LOAD = """
var st = { scale: 1, panX: 0, panY: 0, enlarged: false, active: "right", idx: 0,
           left: null, right: null, shots: [], labels: {}, timer: null, savedPos: 50, seq: 0 };
function root() { return element.querySelector(".cmp-root"); }
function q(sel) { return element.querySelector(sel); }
function setPos(v) { var sl = q(".slider"); if (sl) sl.style.setProperty("--pos", v + "%"); }
function applyTransform() {
  var stage = q(".cmp-stage");
  if (stage) stage.style.transform = "scale(" + st.scale + ") translate(" + st.panX + "px," + st.panY + "px)";
}
function zoomBy(d) {
  st.scale = Math.min(4, Math.max(1, st.scale + d));
  if (st.scale === 1) { st.panX = 0; st.panY = 0; }
  applyTransform();
}
function resetView() {
  st.scale = 1; st.panX = 0; st.panY = 0; applyTransform();
  var r = q(".drag"); if (r) r.value = 50;
  setPos(50);
}
// The "fit" frame can already exceed native pixels on a wide window, and zoomBy's 1-4x
// range can't get back below that -- compute the exact scale from the image's own size.
function actualSize() {
  var img = q("img.right") || q("img.left");
  var frame = q(".frame"); // unscaled ancestor of .cmp-stage -- its rect is the base size
  if (!img || !frame || !img.naturalWidth) return;
  var dpr = window.devicePixelRatio || 1;
  var rect = frame.getBoundingClientRect();
  if (!rect.width) return;
  st.scale = Math.max(0.05, img.naturalWidth / (dpr * rect.width));
  st.panX = 0; st.panY = 0; applyTransform();
}
function stopFlicker() {
  if (!st.timer) return;
  clearInterval(st.timer); st.timer = null; setPos(st.savedPos);
  var b = q(".cmp-flicker-btn"); if (b) b.textContent = "Flicker";
}
function toggleFlicker() {
  if (st.timer) { stopFlicker(); return; }
  var r = q(".drag"); st.savedPos = r ? r.value : 50;
  var showRight = false;
  st.timer = setInterval(function () { showRight = !showRight; setPos(showRight ? 100 : 0); }, 500);
  var b = q(".cmp-flicker-btn"); if (b) b.textContent = "Stop flicker";
}
function setEnlarged(on) { st.enlarged = on; var r = root(); if (r) r.classList.toggle("enlarged", on); }
function preload(i) {
  var s = st.shots[i]; if (!s) return;
  Object.keys(s.urls).forEach(function (k) { var im = new Image(); im.src = s.urls[k]; });
}
function paint() {
  var shot = st.shots[st.idx]; if (!shot) return;
  var il = q("img.left"), ir = q("img.right");
  if (il && il.getAttribute("src") !== shot.urls[st.left]) il.src = shot.urls[st.left];
  if (ir && ir.getAttribute("src") !== shot.urls[st.right]) ir.src = shot.urls[st.right];
  var ll = q(".lbl-left"), lr = q(".lbl-right"), sl = q(".cmp-shot");
  if (ll) ll.textContent = st.labels[st.left] || st.left;
  if (lr) lr.textContent = st.labels[st.right] || st.right;
  if (sl) sl.textContent = shot.caption;
  var pl = q(".pane-left"), pr = q(".pane-right");
  if (pl) pl.classList.toggle("active", st.active === "left");
  if (pr) pr.classList.toggle("active", st.active === "right");
  element.querySelectorAll(".cmp-legend .item").forEach(function (it) {
    var s = it.getAttribute("data-side"), on = [];
    if (s === st.left) on.push("L");
    if (s === st.right) on.push("R");
    it.querySelector(".on").textContent = on.join(" ");
  });
}
function load() {
  var r = root(); if (!r) return;
  try {
    st.shots = JSON.parse(r.getAttribute("data-shots"));
    st.labels = JSON.parse(r.getAttribute("data-labels"));
  } catch (e) { st.shots = []; st.labels = {}; }
  var idx = parseInt(r.getAttribute("data-idx"), 10) || 0;
  if (idx !== st.idx) { st.scale = 1; st.panX = 0; st.panY = 0; }
  st.idx = idx; st.left = r.getAttribute("data-left"); st.right = r.getAttribute("data-right");
  var a = r.getAttribute("data-active"); if (a === "left" || a === "right") st.active = a;
  if (st.timer) { clearInterval(st.timer); st.timer = null; }
  r.classList.toggle("enlarged", st.enlarged);
  applyTransform(); paint(); preload(st.idx);
}
function sync() {
  st.seq += 1;
  trigger("submit", { idx: st.idx, left: st.left, right: st.right, seq: st.seq });
}
function keyToSide(k) {
  if (!k || k.length !== 1) return null;
  if (k.toLowerCase() === "v" && st.labels.vanilla) return "vanilla";
  return st.labels[k.toUpperCase()] ? k.toUpperCase() : null;
}
function chooseSide(side) {
  if (st.active === "left") st.left = side; else st.right = side;
  paint(); sync();
}
function stepShot(d) {
  var n = st.shots.length; if (!n) return;
  st.idx = ((st.idx + d) % n + n) % n;
  st.scale = 1; st.panX = 0; st.panY = 0; applyTransform(); stopFlicker();
  paint(); preload(((st.idx + d) % n + n) % n); sync();
}
function isTyping(t) {
  if (!t || !t.tagName) return false;
  if (t.tagName === "TEXTAREA" || t.isContentEditable) return true;
  if (t.tagName !== "INPUT") return false;
  var ty = (t.type || "text").toLowerCase();
  return ["range", "checkbox", "radio", "button", "submit"].indexOf(ty) < 0;
}

element.addEventListener("input", function (e) {
  if (e.target.classList.contains("drag")) { stopFlicker(); setPos(e.target.value); }
});
element.addEventListener("click", function (e) {
  var b = e.target.closest("button");
  if (e.target.closest(".cmp-flicker-btn")) toggleFlicker();
  else if (e.target.closest(".cmp-actual-btn")) actualSize();
  else if (e.target.closest(".cmp-enlarge-btn")) setEnlarged(!st.enlarged);
  else if (e.target.closest(".cmp-labels .pane")) {
    st.active = e.target.closest(".pane-left") ? "left" : "right"; paint();
  } else {
    var it = e.target.closest(".cmp-legend .item");
    if (it) chooseSide(it.getAttribute("data-side"));
  }
  if (b) b.blur();
});
element.addEventListener("wheel", function (e) {
  var stage = q(".cmp-stage");
  if (!stage || !stage.contains(e.target)) return;
  e.preventDefault(); zoomBy(e.deltaY < 0 ? 0.25 : -0.25);
}, { passive: false });
element.addEventListener("mousedown", function (e) {
  var stage = q(".cmp-stage");
  if (!stage || !stage.contains(e.target) || st.scale <= 1) return;
  var lastX = e.clientX, lastY = e.clientY;
  function onMove(ev) {
    st.panX += (ev.clientX - lastX) / st.scale; st.panY += (ev.clientY - lastY) / st.scale;
    lastX = ev.clientX; lastY = ev.clientY; applyTransform();
  }
  function onUp() { window.removeEventListener("mousemove", onMove); window.removeEventListener("mouseup", onUp); }
  window.addEventListener("mousemove", onMove);
  window.addEventListener("mouseup", onUp);
});

// Capture phase, so a focused wipe slider or button cannot swallow the shortcuts.
document.addEventListener("keydown", function (e) {
  if (e.ctrlKey || e.metaKey || e.altKey) return;
  if (e.key === "Escape") { if (st.enlarged) { e.preventDefault(); setEnlarged(false); } return; }
  if (isTyping(e.target)) return;
  var side = keyToSide(e.key), handled = true;
  if (side) chooseSide(side);
  else if (e.key === "Tab") { st.active = st.active === "left" ? "right" : "left"; paint(); }
  else if (e.key === "[" || e.key === "ArrowLeft") stepShot(-1);
  else if (e.key === "]" || e.key === "ArrowRight") stepShot(1);
  else if (e.key === " ") toggleFlicker();
  else if (e.key === "+" || e.key === "=") zoomBy(0.25);
  else if (e.key === "-") zoomBy(-0.25);
  else if (e.key === "1") actualSize();
  else if (e.key === "0") resetView();
  else if (e.key === "Enter") setEnlarged(!st.enlarged);
  else handled = false;
  if (handled) { e.preventDefault(); e.stopPropagation(); }
}, true);

load();
watch("value", load);
"""
