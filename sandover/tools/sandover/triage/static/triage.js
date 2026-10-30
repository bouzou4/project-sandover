"use strict";
// Snapshot triage page (kb/design/snapshot-triage.md). One snapshot at a time, keyboard first.

const $ = (id) => document.getElementById(id);
const FACET_VAR = {
  "performance": "--f1", "artistic-integrity": "--f2",
  "remaster-fanatic": "--f3", "game-developer": "--f4",
};
const FACET_TIP = {
  "performance": "Frame time, VRAM, hitching",
  "artistic-integrity": "Does it respect the game's art?",
  "remaster-fanatic": "Is it enough of a modern remaster?",
  "game-developer": "Clipping, rendering bugs, seams, popping",
};
const FILTERS = ["all", "kept", "not-for-review"];
const FILTER_LABEL = { "all": "All", "kept": "Kept", "not-for-review": "Not for review" };
const STATUS_LABEL = { "kept": "Kept", "not-for-review": "Not for review", "deleted": "Deleted" };
const KNOWN = new Set(["index", "time_utc", "level", "continue", "jak_pos", "cam_pos", "cam_rows",
  "fov", "hour", "mood_weights", "mood", "toggles", "build_sha", "status", "note", "tags",
  "annotations"]);

let FACETS = [];
let LABELS = {};
let snaps = [];
const byIdx = new Map();
let summary = null;
let cur = null;
const filter = { status: "all", facet: null };
const view = { mode: "fit", s: 1, tx: 0, ty: 0, iw: 0, ih: 0, shown: null };
let mouse = null;
let drag = null;
let cropArmed = false;
let cropTarget = null;
let draft = null;
let sel = null;
let editing = null;

const pad = (i) => String(i).padStart(4, "0");
const css = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();
const facetColor = (f) => (f ? css(FACET_VAR[f]) : "#ffffff");
const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => (
  { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const dpr = () => window.devicePixelRatio || 1;

// ---- saving -----------------------------------------------------------------------------

let chain = Promise.resolve();
let pending = 0;
let flashTimer = null;

function saveState(state) {
  const el = $("save");
  el.className = "save " + (state || "");
  el.textContent = { pending: "Saving…", failed: "Not saved: server unreachable. Retrying.",
    flash: "Saved" }[state] || "Saved";
  if (state === "flash") {
    clearTimeout(flashTimer);
    flashTimer = setTimeout(() => { if (!pending) saveState(""); }, 900);
  }
}

function send(path, body) {
  pending++;
  saveState("pending");
  const p = chain.then(() => attempt(path, body));
  chain = p.catch(() => {});
  return p;
}

async function attempt(path, body) {
  for (;;) {
    let r;
    try {
      r = await fetch(path, { method: "POST", headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body || {}) });
    } catch (e) {
      saveState("failed");
      await sleep(2000);
      continue;
    }
    let j = {};
    try { j = await r.json(); } catch (e) { /* empty body */ }
    if (r.status >= 500) {
      saveState("failed");
      await sleep(2000);
      continue;
    }
    pending--;
    if (!r.ok) {
      toast(j.error || "Not saved.");
      saveState(pending ? "pending" : "");
      await reload();
      return null;
    }
    applyResponse(j);
    saveState(pending ? "pending" : "flash");
    return j;
  }
}

function applyResponse(j) {
  if (j.summary) summary = j.summary;
  if (j.snapshot) putSnap(j.snapshot);
  renderHeader();
  if (j.snapshot) {
    updateTile(j.snapshot.index);
    if (j.snapshot.index === cur) renderRail();
  }
}

function putSnap(s) {
  const old = byIdx.get(s.index);
  if (old) Object.keys(old).forEach((k) => delete old[k]);
  const target = old || s;
  Object.assign(target, s);
  if (!old) {
    snaps.push(target);
    snaps.sort((a, b) => a.index - b.index);
    byIdx.set(s.index, target);
    buildStrip();
  }
}

async function reload() {
  const r = await fetch("/api/run", { cache: "no-store" });
  const j = await r.json();
  document.title = j.name + " · Snapshot triage";
  $("runName").textContent = j.name;
  FACETS = j.facets;
  LABELS = j.facet_labels;
  snaps = j.snapshots;
  byIdx.clear();
  snaps.forEach((s) => byIdx.set(s.index, s));
  summary = j.summary;
  buildStrip();
  renderHeader();
  if (cur === null || !byIdx.has(cur)) {
    const v = visible();
    cur = v.length ? v[0].index : null;
  }
  show(cur, true);
}

// ---- filtering / navigation ---------------------------------------------------------------

function facetsOf(s) {
  const f = new Set(s.tags || []);
  (s.annotations || []).forEach((a) => a.facet && f.add(a.facet));
  return FACETS.filter((x) => f.has(x));
}

function matches(s) {
  if (filter.status === "all") {
    if (filter.facet && s.status === "deleted") return false;
  } else if (s.status !== filter.status) return false;
  return !filter.facet || facetsOf(s).includes(filter.facet);
}

function visible() {
  return snaps.filter((s) => matches(s) || s.index === cur);
}

function step(delta) {
  const v = visible();
  if (!v.length) return;
  let i = v.findIndex((s) => s.index === cur);
  i = i < 0 ? 0 : Math.max(0, Math.min(v.length - 1, i + delta));
  show(v[i].index);
}

function advanceFrom(idx) {
  const v = snaps.filter((s) => matches(s));
  const next = v.find((s) => s.index > idx) || [...v].reverse().find((s) => s.index < idx);
  show(next ? next.index : idx);
}

function setFilter(status, facet) {
  filter.status = status;
  filter.facet = facet;
  const v = snaps.filter(matches);
  if (!v.some((s) => s.index === cur)) cur = v.length ? v[0].index : null;
  renderHeader();
  refreshStripVisibility();
  show(cur, true);
}

// ---- header -------------------------------------------------------------------------------

function renderHeader() {
  if (!summary) return;
  const c = summary.counts;
  $("counts").innerHTML =
    `<span class="c-kept"><b>${c.kept}</b> kept</span>` +
    `<span class="c-nfr"><b>${c["not-for-review"]}</b> not for review</span>` +
    `<span class="c-del"><b>${c.deleted}</b> deleted</span>`;
  $("facetCounts").innerHTML = FACETS.map((f) => {
    const n = summary.facets[f];
    const on = filter.facet === f ? " on" : "";
    const zero = n ? "" : " zero";
    return `<button class="fcount${on}${zero}" data-facet="${f}" style="--fc:var(${FACET_VAR[f]})"` +
      ` title="${esc(FACET_TIP[f])}. Click to show only these."` +
      ` aria-pressed="${!!on}"><i class="dot" style="background:var(--fc)"></i>${esc(LABELS[f])} <b>${n}</b></button>`;
  }).join("");
  let label = "Showing: " + FILTER_LABEL[filter.status];
  if (filter.facet) label += " · " + LABELS[filter.facet];
  $("filterLabel").textContent = label;
}

$("facetCounts").addEventListener("click", (e) => {
  const b = e.target.closest(".fcount");
  if (!b) return;
  setFilter(filter.status, filter.facet === b.dataset.facet ? null : b.dataset.facet);
  b.blur();
});

// ---- filmstrip ------------------------------------------------------------------------------

function tileHtml(s) {
  const dots = facetsOf(s).map((f) => `<i style="background:var(${FACET_VAR[f]})" title="${esc(LABELS[f])}"></i>`).join("");
  const n = (s.annotations || []).length;
  return `<img loading="lazy" alt="" src="/thumb/${pad(s.index)}.jpg">` +
    `<span class="bar"></span><span class="lab"><span class="num">#${pad(s.index)}</span>` +
    `<span class="lvl">${esc(s.level || "")}</span><span class="dots">${dots}</span>` +
    (n ? `<span class="crops" title="${n} crop${n > 1 ? "s" : ""}">▭${n}</span>` : "") + `</span>`;
}

function tileClass(s) {
  return `tile ${s.status}${s.index === cur ? " cur" : ""}${matches(s) || s.index === cur ? "" : " filtered"}`;
}

function buildStrip() {
  const strip = $("strip");
  if (!snaps.length) {
    strip.innerHTML = '<span class="stripEmpty">No snapshots yet.</span>';
    return;
  }
  strip.innerHTML = snaps.map((s) =>
    `<button class="${tileClass(s)}" data-idx="${s.index}" aria-label="#${pad(s.index)} ${esc(s.level || "")}, ${STATUS_LABEL[s.status]}">${tileHtml(s)}</button>`).join("");
}

function updateTile(idx) {
  const t = document.querySelector(`.tile[data-idx="${idx}"]`);
  const s = byIdx.get(idx);
  if (!t || !s) return;
  t.className = tileClass(s);
  t.setAttribute("aria-label", `#${pad(idx)} ${s.level || ""}, ${STATUS_LABEL[s.status]}`);
  const img = t.querySelector("img");
  const lab = t.querySelector(".lab");
  const tmp = document.createElement("div");
  tmp.innerHTML = tileHtml(s);
  lab.replaceWith(tmp.querySelector(".lab"));
  if (!img) t.innerHTML = tileHtml(s);
}

function refreshStripVisibility() {
  document.querySelectorAll(".tile").forEach((t) => {
    const s = byIdx.get(+t.dataset.idx);
    if (s) t.className = tileClass(s);
  });
}

$("strip").addEventListener("click", (e) => {
  const t = e.target.closest(".tile");
  if (!t) return;
  show(+t.dataset.idx);
  t.blur();
});

// ---- showing a snapshot ---------------------------------------------------------------------

function show(idx, force) {
  const prev = cur;
  if (document.activeElement === $("note")) commitNote();
  cur = idx;
  if (prev !== idx) { sel = null; editing = null; draft = null; cancelCrop(); }
  refreshStripVisibility();
  const s = byIdx.get(idx);
  if (!s) {
    const empty = !snaps.length;
    stageMsg(empty
      ? "No snapshots in this run. Press the snapshot key in game; each press adds a PNG and its sidecar here."
      : `No snapshots match ${FILTER_LABEL[filter.status]}${filter.facet ? " · " + LABELS[filter.facet] : ""}. Press F to show another set, Esc to show all.`);
    $("img").removeAttribute("src");
    view.shown = null;
    drawOverlay();
    renderRail();
    return;
  }
  const t = document.querySelector(`.tile[data-idx="${idx}"]`);
  if (t) t.scrollIntoView({ inline: "center", block: "nearest" });
  $("stage").classList.toggle("deletedVeil", s.status === "deleted");
  renderRail();
  if (force || prev !== idx || view.shown !== idx) loadImage(s);
  else drawOverlay();
}

function loadImage(s) {
  const url = `/img/${pad(s.index)}.png`;
  const pre = new Image();
  pre.onload = () => {
    if (cur !== s.index) return;
    const img = $("img");
    const sizeChanged = pre.naturalWidth !== view.iw || pre.naturalHeight !== view.ih;
    img.src = url;
    view.iw = pre.naturalWidth;
    view.ih = pre.naturalHeight;
    img.style.width = view.iw + "px";
    img.style.height = view.ih + "px";
    view.shown = s.index;
    stageMsg(null);
    if (view.mode === "fit" || sizeChanged) fit(); else clampPan();
    applyView();
    preloadNeighbours();
  };
  pre.onerror = () => {
    if (cur !== s.index) return;
    $("img").removeAttribute("src");
    view.shown = null;
    stageMsg(`Image missing: ${pad(s.index)}.png`);
    drawOverlay();
  };
  pre.src = url;
}

function preloadNeighbours() {
  const v = visible();
  const i = v.findIndex((s) => s.index === cur);
  [v[i + 1], v[i - 1]].forEach((s) => { if (s) new Image().src = `/img/${pad(s.index)}.png`; });
}

function stageMsg(text) {
  const el = $("stageMsg");
  el.hidden = !text;
  el.textContent = text || "";
  $("layer").style.visibility = text ? "hidden" : "visible";
}

// ---- zoom and pan ---------------------------------------------------------------------------

function stageSize() {
  const r = $("stage").getBoundingClientRect();
  return [r.width, r.height];
}

function fit() {
  const [W, H] = stageSize();
  const p = 12;
  view.mode = "fit";
  view.s = Math.min((W - 2 * p) / view.iw, (H - 2 * p) / view.ih);
  view.tx = (W - view.iw * view.s) / 2;
  view.ty = (H - view.ih * view.s) / 2;
}

function oneToOne(cx, cy) {
  const [W, H] = stageSize();
  if (cx === undefined) { cx = W / 2; cy = H / 2; }
  const ix = (cx - view.tx) / view.s;
  const iy = (cy - view.ty) / view.s;
  view.mode = "one";
  view.s = 1 / dpr();
  view.tx = cx - ix * view.s;
  view.ty = cy - iy * view.s;
  clampPan();
}

function clampPan() {
  const [W, H] = stageSize();
  const w = view.iw * view.s;
  const h = view.ih * view.s;
  view.tx = w <= W ? (W - w) / 2 : Math.min(0, Math.max(W - w, view.tx));
  view.ty = h <= H ? (H - h) / 2 : Math.min(0, Math.max(H - h, view.ty));
  // whole device pixels, so 1:1 maps one image pixel to one screen pixel
  view.tx = Math.round(view.tx * dpr()) / dpr();
  view.ty = Math.round(view.ty * dpr()) / dpr();
}

function applyView() {
  $("layer").style.transform = `translate(${view.tx}px, ${view.ty}px) scale(${view.s})`;
  $("stage").classList.toggle("one", view.mode === "one");
  $("zoomTag").textContent = view.mode === "one" ? "1:1" : `Fit ${Math.round(view.s * dpr() * 100)}%`;
  drawOverlay();
}

function toggleZoom(cx, cy) {
  if (!view.iw) return;
  if (view.mode === "one") fit(); else oneToOne(cx, cy);
  applyView();
}

function toImage(e) {
  const r = $("stage").getBoundingClientRect();
  return [(e.clientX - r.left - view.tx) / view.s, (e.clientY - r.top - view.ty) / view.s];
}

window.addEventListener("resize", () => {
  if (!view.iw) return;
  if (view.mode === "fit") fit(); else clampPan();
  applyView();
});

// ---- overlay (annotations) ------------------------------------------------------------------

function drawOverlay() {
  const svg = $("overlay");
  const s = byIdx.get(cur);
  if (!s || view.shown !== cur) { svg.innerHTML = ""; return; }
  svg.setAttribute("width", view.iw);
  svg.setAttribute("height", view.ih);
  svg.setAttribute("viewBox", `0 0 ${view.iw} ${view.ih}`);
  const k = 1 / view.s;
  let out = "";
  (s.annotations || []).forEach((a, i) => {
    if (editing === a.id && cropArmed && cropTarget === a.id) return;
    const [x, y, w, h] = a.rect;
    const col = facetColor(a.facet);
    const wd = sel === a.id ? 3 : 1.5;
    out += `<g class="ann${sel === a.id ? " sel" : ""}" data-id="${esc(a.id)}">` +
      `<rect class="box" x="${x}" y="${y}" width="${w}" height="${h}" stroke="#000" stroke-width="${wd + 2}" vector-effect="non-scaling-stroke" stroke-opacity=".7"/>` +
      `<rect class="box" x="${x}" y="${y}" width="${w}" height="${h}" stroke="${col}" stroke-width="${wd}" vector-effect="non-scaling-stroke"/>` +
      `<g transform="translate(${x} ${y}) scale(${k})"><circle r="11" fill="${col}" stroke="#000" stroke-width="1.5"/>` +
      `<text text-anchor="middle" dy="4" font-size="12" font-weight="700" fill="#111" font-family="system-ui">${i + 1}</text></g></g>`;
  });
  const d = draft && draft.rect;
  if (d) {
    out += `<rect class="draft" x="${d[0]}" y="${d[1]}" width="${d[2]}" height="${d[3]}" stroke-width="1.5" vector-effect="non-scaling-stroke"/>`;
  }
  svg.innerHTML = out;
}

$("overlay").addEventListener("mousedown", (e) => {
  const g = e.target.closest(".ann");
  if (!g || cropArmed) return;
  e.stopPropagation();
  selectAnn(g.dataset.id);
});

$("overlay").addEventListener("mouseover", (e) => {
  const g = e.target.closest(".ann");
  document.querySelectorAll(".ann-row").forEach((r) => r.classList.toggle("hover", !!g && r.dataset.id === g.dataset.id));
});

function selectAnn(id) {
  sel = sel === id ? null : id;
  renderAnns();
  drawOverlay();
}

// ---- stage mouse: pan, double-click zoom, crop drawing --------------------------------------

const stage = $("stage");
stage.addEventListener("mousemove", (e) => {
  const r = stage.getBoundingClientRect();
  mouse = [e.clientX - r.left, e.clientY - r.top];
});
stage.addEventListener("mouseleave", () => { mouse = null; });

stage.addEventListener("mousedown", (e) => {
  if (e.button !== 0 || !view.iw || view.shown !== cur) return;
  e.preventDefault();
  if (cropArmed) {
    const p = clampToImage(toImage(e));
    draft = { start: p, rect: [p[0], p[1], 0, 0] };
    drag = { kind: "crop" };
  } else if (view.mode === "one") {
    drag = { kind: "pan", x: e.clientX, y: e.clientY, tx: view.tx, ty: view.ty };
    stage.classList.add("dragging");
  }
});

window.addEventListener("mousemove", (e) => {
  if (!drag) return;
  if (drag.kind === "pan") {
    view.tx = drag.tx + e.clientX - drag.x;
    view.ty = drag.ty + e.clientY - drag.y;
    clampPan();
    applyView();
  } else {
    const p = clampToImage(toImage(e));
    const [sx, sy] = draft.start;
    draft.rect = [Math.min(sx, p[0]), Math.min(sy, p[1]), Math.abs(p[0] - sx), Math.abs(p[1] - sy)];
    drawOverlay();
  }
});

window.addEventListener("mouseup", () => {
  if (!drag) return;
  const kind = drag.kind;
  drag = null;
  stage.classList.remove("dragging");
  if (kind !== "crop") return;
  const r = draft.rect.map(Math.round);
  if (r[2] < 4 || r[3] < 4) { draft = null; drawOverlay(); return; }
  if (cropTarget) {
    const id = cropTarget;
    draft = null;
    cancelCrop();
    editing = null;
    send(`/api/snap/${cur}/annotations/${id}`, { rect: r });
    return;
  }
  draft = { rect: r };
  cancelCrop(true);
  editing = "new";
  renderAnns();
  drawOverlay();
});

stage.addEventListener("dblclick", (e) => {
  if (cropArmed) return;
  const r = stage.getBoundingClientRect();
  toggleZoom(e.clientX - r.left, e.clientY - r.top);
});

function clampToImage([x, y]) {
  return [Math.max(0, Math.min(view.iw, x)), Math.max(0, Math.min(view.ih, y))];
}

function armCrop(target) {
  const s = byIdx.get(cur);
  if (!s || !view.iw) return;
  if (s.status === "deleted") { toast("Deleted. D restores it."); return; }
  cropArmed = true;
  cropTarget = target || null;
  stage.classList.add("crop");
  $("cropHint").hidden = false;
  $("cropHint").textContent = target ? "Drag the new rectangle. Esc keeps the old one." : "Drag over the problem. Esc cancels.";
  drawOverlay();
}

function cancelCrop(keepDraft) {
  cropArmed = false;
  cropTarget = null;
  stage.classList.remove("crop");
  $("cropHint").hidden = true;
  if (!keepDraft) draft = null;
}

// ---- rail -----------------------------------------------------------------------------------

function fmtNum(v, d = 1) {
  return typeof v === "number" ? (Number.isInteger(v) ? String(v) : v.toFixed(d)) : esc(v);
}

function fmtVec(v) {
  if (Array.isArray(v)) return v.map((x) => (Array.isArray(x) ? "[" + fmtVec(x) + "]" : fmtNum(x, 2))).join(", ");
  if (v && typeof v === "object") return Object.entries(v).map(([k, x]) => `${esc(k)} ${fmtNum(x, 2)}`).join(", ");
  return esc(v ?? "—");
}

function fmtHour(h) {
  if (typeof h !== "number") return h == null ? "" : esc(h);
  const hh = Math.floor(h) % 24;
  const mm = Math.round((h - Math.floor(h)) * 60);
  return `${String(hh).padStart(2, "0")}:${String(mm % 60).padStart(2, "0")}`;
}

function moodHtml(m) {
  if (m == null) return "—";
  const entries = Array.isArray(m) ? m.map((w, i) => [`slot ${i}`, w]) : Object.entries(m);
  if (!entries.length) return "—";
  return `<div class="mood">` + entries.map(([k, w]) => {
    const pct = typeof w === "number" ? Math.max(0, Math.min(1, w)) * 100 : 0;
    return `<span class="dim">${esc(k)}</span><span class="bar"><i style="width:${pct}%"></i></span><span>${fmtNum(w, 2)}</span>`;
  }).join("") + `</div>`;
}

function togglesHtml(t) {
  if (t == null) return "—";
  if (Array.isArray(t)) return t.length ? `<span class="on-list">On: ${t.map(esc).join(", ")}</span>` : "—";
  const on = [], off = [], other = [];
  Object.entries(t).forEach(([k, v]) => {
    if (v === true) on.push(k);
    else if (v === false) off.push(k);
    else other.push(`${k}=${typeof v === "object" ? JSON.stringify(v) : v}`);
  });
  let out = "";
  if (on.length) out += `<div class="on-list">On: ${on.map(esc).join(", ")}</div>`;
  if (off.length) out += `<div class="off-list">Off: ${off.map(esc).join(", ")}</div>`;
  if (other.length) out += `<div>${other.map(esc).join(", ")}</div>`;
  return out || "—";
}

function renderRail() {
  const s = byIdx.get(cur);
  const note = $("note");
  if (!s) {
    $("snapIndex").textContent = "#—";
    $("snapLevel").textContent = "";
    $("snapHour").textContent = "";
    $("pill").className = "pill";
    $("pill").textContent = "";
    $("pillHint").textContent = "";
    $("chips").innerHTML = "";
    $("anns").innerHTML = "";
    $("facts").innerHTML = "";
    note.value = "";
    note.disabled = true;
    return;
  }
  const deleted = s.status === "deleted";
  $("snapIndex").textContent = "#" + pad(s.index);
  $("snapLevel").textContent = s.level || "";
  $("snapHour").textContent = fmtHour(s.hour);
  $("pill").className = "pill " + s.status;
  $("pill").textContent = STATUS_LABEL[s.status];
  $("pillHint").textContent = deleted ? "Moved to deleted/; U or D restores it."
    : s.status === "not-for-review" ? "X keeps it" : "";
  $("chips").innerHTML = FACETS.map((f, i) => {
    const on = (s.tags || []).includes(f);
    return `<button class="chip${on ? " on" : ""}" data-facet="${f}" style="--fc:var(${FACET_VAR[f]})"` +
      ` aria-pressed="${on}" title="${esc(FACET_TIP[f])}"${deleted ? " disabled" : ""}><kbd>${i + 1}</kbd>${esc(LABELS[f])}</button>`;
  }).join("");
  if (document.activeElement !== note) note.value = s.note || "";
  note.disabled = deleted;
  renderAnns();
  renderFacts(s);
}

function renderFacts(s) {
  const rows = [
    ["Level", esc(s.level || "—")],
    ["Continue", esc(s.continue || "—")],
    ["Time of day", fmtHour(s.hour) || "—"],
    ["Captured", `<span class="mono">${esc(s.time_utc || "—")}</span>`],
    ["Toggles", togglesHtml(s.toggles)],
    ["Mood", moodHtml(s.mood_weights ?? s.mood)],
    ["Jak", `<span class="mono">${fmtVec(s.jak_pos)}</span>`],
    ["Camera", `<span class="mono">${fmtVec(s.cam_pos)}</span>`],
    ["FOV", s.fov == null ? "—" : fmtNum(s.fov, 1)],
    ["Cam rows", `<span class="mono">${Array.isArray(s.cam_rows) ? s.cam_rows.map((r) => fmtVec(r)).join("<br>") : fmtVec(s.cam_rows)}</span>`],
    ["Build", `<span class="mono" title="${esc(s.build_sha || "")}">${esc((s.build_sha || "—").slice(0, 10))}</span>`],
  ];
  Object.keys(s).filter((k) => !KNOWN.has(k)).forEach((k) => {
    const v = s[k];
    rows.push([esc(k), `<span class="mono">${typeof v === "object" ? fmtVec(v) : esc(v)}</span>`]);
  });
  $("facts").innerHTML = rows.map(([k, v]) => `<dt>${k}</dt><dd>${v}</dd>`).join("");
}

function renderAnns() {
  const s = byIdx.get(cur);
  const list = $("anns");
  if (!s) { list.innerHTML = ""; return; }
  const typed = list.querySelector(".ann-edit");
  const keep = typed && typed.closest(".ann-row").dataset.id === editing ? typed.value : null;
  const anns = s.annotations || [];
  let html = anns.map((a, i) => {
    const col = a.facet ? `var(${FACET_VAR[a.facet]})` : "#fff";
    const facet = a.facet ? esc(LABELS[a.facet]) : "No facet: select it, then 1–4";
    const body = editing === a.id
      ? `<textarea class="ann-edit" rows="2" placeholder="What is wrong in this crop?">${esc(a.text)}</textarea>` +
        `<div class="ann-actions"><span class="dim small">Enter saves · Esc cancels</span><button data-act="redraw">Redraw</button></div>`
      : `<div class="ann-text${a.text ? "" : " empty"}">${a.text ? esc(a.text) : "No comment"}</div>` +
        `<div class="ann-actions"><button data-act="edit">Edit</button><button data-act="delete">Delete</button>` +
        (sel === a.id ? `<span class="dim small">1–4 sets its facet · Esc deselects</span>` : "") + `</div>`;
    return `<li class="ann-row${sel === a.id ? " sel" : ""}" data-id="${esc(a.id)}" style="--ac:${col}">` +
      `<span class="ann-num">${i + 1}</span><span class="ann-facet">${facet}</span>${body}</li>`;
  }).join("");
  if (editing === "new" && draft) {
    html += `<li class="ann-row sel" data-id="new"><span class="ann-num">${anns.length + 1}</span>` +
      `<span class="ann-facet">New crop</span><textarea class="ann-edit" rows="2" placeholder="What is wrong in this crop?"></textarea>` +
      `<div class="ann-actions"><span class="dim small">Enter saves, then 1–4 sets its facet · Esc cancels</span></div></li>`;
  }
  if (!html) html = `<li class="ann-empty">No crops. Press C and drag over the problem.</li>`;
  list.innerHTML = html;
  const ed = list.querySelector(".ann-edit");
  if (ed && keep !== null) ed.value = keep;
  if (ed) { ed.focus(); ed.setSelectionRange(ed.value.length, ed.value.length); }
}

$("anns").addEventListener("click", (e) => {
  const row = e.target.closest(".ann-row");
  if (!row || row.dataset.id === "new") return;
  const id = row.dataset.id;
  const act = e.target.closest("button")?.dataset.act;
  if (act === "edit") { sel = id; editing = id; renderAnns(); drawOverlay(); return; }
  if (act === "delete") { deleteAnn(id); return; }
  if (act === "redraw") { sel = id; armCrop(id); return; }
  if (e.target.closest("textarea")) return;
  selectAnn(id);
});

$("anns").addEventListener("keydown", (e) => {
  if (!e.target.classList.contains("ann-edit")) return;
  if (e.key === "Enter" && !e.shiftKey) {
    e.preventDefault();
    saveAnnText(e.target.value);
  } else if (e.key === "Escape") {
    e.preventDefault();
    e.stopPropagation();
    cancelEdit();
  }
});

function saveAnnText(text) {
  const s = byIdx.get(cur);
  if (editing === "new") {
    const rect = draft.rect;
    const before = new Set((s.annotations || []).map((a) => a.id));
    draft = null;
    editing = null;
    send(`/api/snap/${cur}/annotations`, { rect, text }).then((j) => {
      const added = j && j.snapshot && j.snapshot.annotations.find((a) => !before.has(a.id));
      if (added && cur === j.snapshot.index) { sel = added.id; renderAnns(); drawOverlay(); }
    });
    renderAnns();
    return;
  }
  const id = editing;
  editing = null;
  const a = (s.annotations || []).find((x) => x.id === id);
  if (a) a.text = text.trim();
  renderAnns();
  drawOverlay();
  send(`/api/snap/${cur}/annotations/${id}`, { text });
}

function cancelEdit() {
  editing = null;
  draft = null;
  cancelCrop();
  renderAnns();
  drawOverlay();
}

function deleteAnn(id) {
  const s = byIdx.get(cur);
  s.annotations = (s.annotations || []).filter((a) => a.id !== id);
  if (sel === id) sel = null;
  renderAnns();
  drawOverlay();
  send(`/api/snap/${cur}/annotations/${id}/delete`);
  toast("Crop deleted. U restores it.");
}

// ---- actions --------------------------------------------------------------------------------

function current() { return byIdx.get(cur); }

function toggleFacet(i) {
  const s = current();
  const f = FACETS[i];
  if (!s || !f) return;
  if (s.status === "deleted") { toast("Deleted. D restores it."); return; }
  if (sel) {
    const a = (s.annotations || []).find((x) => x.id === sel);
    if (a) {
      a.facet = a.facet === f ? null : f;
      renderAnns();
      drawOverlay();
      send(`/api/snap/${cur}/annotations/${a.id}`, { facet: a.facet });
      return;
    }
  }
  const tags = new Set(s.tags || []);
  if (tags.has(f)) tags.delete(f); else tags.add(f);
  s.tags = FACETS.filter((x) => tags.has(x));
  renderRail();
  updateTile(cur);
  send(`/api/snap/${cur}`, { tags: s.tags });
}

$("chips").addEventListener("click", (e) => {
  const b = e.target.closest(".chip");
  if (!b) return;
  const i = FACETS.indexOf(b.dataset.facet);
  const keep = sel;
  sel = null;
  toggleFacet(i);
  sel = keep;
  b.blur();
});

function toggleNfr() {
  const s = current();
  if (!s) return;
  if (s.status === "deleted") { toast("Deleted. D restores it."); return; }
  const idx = s.index;
  s.status = s.status === "kept" ? "not-for-review" : "kept";
  updateTile(idx);
  send(`/api/snap/${idx}`, { status: s.status });
  if (s.status === "not-for-review") advanceFrom(idx); else renderRail();
}

function deleteOrRestore() {
  const s = current();
  if (!s) return;
  const idx = s.index;
  if (s.status === "deleted") {
    s.status = "kept";
    updateTile(idx);
    renderRail();
    $("stage").classList.remove("deletedVeil");
    send(`/api/snap/${idx}/restore`, { status: "kept" });
    return;
  }
  s.status = "deleted";
  updateTile(idx);
  send(`/api/snap/${idx}/delete`);
  toast(`#${pad(idx)} deleted. U restores it.`);
  advanceFrom(idx);
}

function undo() {
  send("/api/undo").then((j) => {
    if (!j) return;
    if (!j.snapshot) { toast("Nothing to undo."); return; }
    toast(`Restored #${pad(j.snapshot.index)}.`);
    if (!matches(j.snapshot)) { filter.status = "all"; filter.facet = null; renderHeader(); }
    show(j.snapshot.index, true);
  });
}

let noteBaseline = null;
$("note").addEventListener("focus", () => { noteBaseline = $("note").value; });
$("note").addEventListener("blur", () => commitNote());
$("note").addEventListener("keydown", (e) => {
  if (e.key === "Enter" && !e.shiftKey) {
    e.preventDefault();
    $("note").blur();
  } else if (e.key === "Escape") {
    e.preventDefault();
    e.stopPropagation();
    $("note").value = noteBaseline ?? "";
    $("note").blur();
  }
});

function commitNote() {
  const s = current();
  const v = $("note").value;
  if (!s || s.status === "deleted" || noteBaseline === null || v === noteBaseline) return;
  noteBaseline = v;
  s.note = v.trim() || null;
  send(`/api/snap/${s.index}`, { note: v });
}

$("jump").addEventListener("keydown", (e) => {
  if (e.key === "Enter") {
    e.preventDefault();
    const n = parseInt($("jump").value, 10);
    if (byIdx.has(n)) { $("jump").value = ""; $("jump").blur(); show(n); }
    else toast(Number.isNaN(n) ? "Type a snapshot number." : `No snapshot #${pad(n)}.`);
  } else if (e.key === "Escape") {
    e.preventDefault();
    e.stopPropagation();
    $("jump").value = "";
    $("jump").blur();
  }
});

// ---- toast ----------------------------------------------------------------------------------

let toastTimer = null;
function toast(text) {
  let el = $("toast");
  if (!el) {
    el = document.createElement("div");
    el.id = "toast";
    el.setAttribute("role", "status");
    el.style.cssText = "position:absolute;left:50%;bottom:12px;transform:translateX(-50%);" +
      "background:rgba(0,0,0,.75);color:#eee;padding:6px 12px;border-radius:6px;font-size:13px;pointer-events:none";
    stage.appendChild(el);
  }
  el.textContent = text;
  el.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.hidden = true; }, 2600);
}

// ---- keyboard -------------------------------------------------------------------------------

function typing(t) {
  if (!t || !t.tagName) return false;
  return t.tagName === "TEXTAREA" || t.isContentEditable ||
    (t.tagName === "INPUT" && !["checkbox", "radio", "button", "range"].includes((t.type || "text").toLowerCase()));
}

document.addEventListener("keydown", (e) => {
  if (e.ctrlKey || e.metaKey || e.altKey) return;
  if (typing(e.target)) return;
  const k = e.key;
  const handled = () => e.preventDefault();
  switch (k) {
    case "j": case "J": case "ArrowRight": case "ArrowDown": handled(); step(1); break;
    case "k": case "K": case "ArrowLeft": case "ArrowUp": handled(); step(-1); break;
    case "Home": handled(); { const v = visible(); if (v.length) show(v[0].index); } break;
    case "End": handled(); { const v = visible(); if (v.length) show(v[v.length - 1].index); } break;
    case "x": case "X": handled(); toggleNfr(); break;
    case "d": case "D": handled(); deleteOrRestore(); break;
    case "u": case "U": handled(); undo(); break;
    case "1": case "2": case "3": case "4": handled(); toggleFacet(+k - 1); break;
    case "n": case "N": {
      handled();
      const s = current();
      if (s && s.status !== "deleted") $("note").focus();
      else if (s) toast("Deleted. D restores it.");
      break;
    }
    case "c": case "C": handled(); if (!cropArmed) armCrop(); break;
    case "z": case "Z": handled(); toggleZoom(...(mouse || [])); break;
    case "f": case "F": handled(); setFilter(FILTERS[(FILTERS.indexOf(filter.status) + 1) % FILTERS.length], filter.facet); break;
    case "g": case "G": handled(); $("jump").focus(); $("jump").select(); break;
    case "Escape":
      handled();
      if (cropArmed || draft) cancelEdit();
      else if (editing) cancelEdit();
      else if (sel) { sel = null; renderAnns(); drawOverlay(); }
      else if (filter.status !== "all" || filter.facet) setFilter("all", null);
      break;
    default: break;
  }
});

reload().catch(() => {
  stageMsg("Could not reach the triage server. Start it with: sandover-triage <run folder>");
});
