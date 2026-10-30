# Snapshot triage: design spec

Tool: `sandover-triage <run_dir>`. A full run is played with every
effect on, presses the snapshot key whenever something looks wrong, then sits down with this
page to throw away the accidents and say what is wrong with the rest. The result
(`triage.csv`, the crops, the sidecars) turns each item into a fix task.

The job on this page is judging renders and writing short verdicts, fast, for maybe 50-300
snapshots. Everything below serves that.

## 1. Critique of the existing reviewer and an earlier review UI

Source: `materials/review.py`, an earlier review UI and the screenshots under
`$SANDOVER_DATA/materials/reviewer-ui/`.

**First impression.** The eye lands on the full-width button rows at the bottom (orange and
red blocks), not on the texture being judged. The image is one of many equally weighted
panels; the page reads as a form.

| Finding | Severity | Decision for triage |
|---|---|---|
| Images are shown scaled into Gradio panels; no way to see pixels 1:1 | Critical | Full-resolution image is the page. `Z` toggles fit / 1:1 (device pixels, not CSS pixels); drag pans. |
| Keys and buttons duplicate each other: 15+ large buttons plus a legend paragraph | Moderate | No button rows. Every action is a key; the same actions are clickable where the thing lives (a facet chip toggles its facet, a filmstrip tile selects it). One compact legend, always visible. |
| Legend is a run-on sentence (`keys: Up brighter ... · Down dimmer ...`) | Moderate | Legend is a two-column grid: key cap, verb. Grouped: move, judge, write, view. |
| "Not saved yet" / separate "Save" button; saving is a user chore | Moderate | Autosave on every action. A save indicator in the header: Saved / Saving / Not saved (retrying). |
| Progress (`610/670 (0 placed)`) is a line of text among others | Minor | Summary header with counts and open items per facet, clickable as filters. |
| Warm grey `#222226` background with saturated orange accents next to the image | Moderate | Neutral grey (no hue) around the image so the render's colours are judged against nothing. Accents only in small marks (status bar, facet dots), never large fills. |
| Gradio adds spinners, reflows and latency on each key press | Moderate | Plain page served by a stdlib server; state changes are local and optimistic, then saved. |
| Tabs hide related work (Review / Browse / Emissive) | Moderate | One page, no tabs, no menus, no accordions. |

**Keep:** the dark theme; keyboard-first with keys ignored while typing; loopback-only server
with no CDN; `<kbd>` key caps; undo for destructive flags; every action saved at once; the
context strip (here: the filmstrip) under the main view.

## 2. Copy

Terms (used identically in UI, JSON, CSV and docs):

| Thing | Label | Never |
|---|---|---|
| status `kept` | Kept | "OK", "valid" |
| status `not-for-review` | Not for review | "flagged", "skipped", "ignored" |
| status `deleted` | Deleted | "removed", "trashed" |
| an issue on a snapshot | open item (a kept snapshot with a facet, or an annotation) | "ticket", "bug" |
| facet 1 | Performance | |
| facet 2 | Artistic integrity | |
| facet 3 | Remaster fanatic | "DF", "Digital Foundry" |
| facet 4 | Game developer | "bugs" |
| rectangle + comment | annotation (UI: "Crop") | "region", "box" |

Facet tooltips (one line, from the issue): Performance: "Frame time, VRAM, hitching".
Artistic integrity: "Does it respect the game's art?". Remaster fanatic: "Is it enough of a
modern remaster?". Game developer: "Clipping, rendering bugs, seams, popping".

Strings:

- Title: run folder name, e.g. `20260925-1412-full-run`; subtitle "Snapshot triage".
- Summary: `41 kept · 6 not for review · 3 deleted` then facet chips `Performance 2`.
- Save indicator: `Saved` · `Saving…` · `Not saved: server unreachable. Retrying.`
- Note placeholder: `What is wrong here? (N to write, Enter to save)`.
- Annotation placeholder: `What is wrong in this crop?`
- Filter label: `Showing: All` / `Kept` / `Not for review`; plus `· Game developer` when a
  facet chip filters.
- Jump box: `Go to #` (G, type a number, Enter).
- Empty run: "No snapshots in this run. Press the snapshot key in game; each press adds a
  PNG and its sidecar here."
- Empty filter: "No snapshots match {filter}. Press F to show another set, Esc to show all."
- No annotations: "No crops. Press C and drag over the problem."
- Deleted snapshot on screen: "Deleted. Moved to deleted/; U or D restores it."
- Load error for one image: "Image missing: NNNN.png"

## 3. Layout

```
+----------------------------------------------------------------------------------+
| run name · Snapshot triage | 41 kept · 6 not for review · 3 deleted | facet chips  | Saved |
+-----------------------------------------------------------+----------------------+
|                                                           | #0007  beach  12:30  |
|                                                           | [Kept]               |
|                 image stage (fit or 1:1)                  | 1 2 3 4 facet chips  |
|                 annotations drawn over it                 | Note                 |
|                                                           | Crops (list)         |
|                                                           | Facts                |
|                                                           | Keys (legend)        |
+-----------------------------------------------------------+----------------------+
| filmstrip: [0001][0002][0003] ...  (status bar, index, level, facet dots)         |
+----------------------------------------------------------------------------------+
```

- Grid: header 48 px; stage fills; rail 360 px fixed; filmstrip 112 px (thumb 160×90 + label).
- Stage background is `--stage` (pure neutral), no border, no padding in 1:1, 12 px in fit.
- Rail scrolls on its own; the legend is the last rail section and is pinned (sticky bottom)
  so it is always visible.
- Filmstrip scrolls horizontally and keeps the current tile centred.

### Tokens

| Token | Value | Use |
|---|---|---|
| `--bg` | `#161616` | page |
| `--stage` | `#0f0f0f` | behind the image (neutral, no hue) |
| `--panel` | `#1e1e1e` | rail, header, filmstrip |
| `--line` | `#2c2c2c` | separators |
| `--text` | `#e6e6e6` | body |
| `--dim` | `#9a9a9a` | labels, secondary (contrast 6.9:1 on panel) |
| `--kept` | `#5fb37a` | status bar/pill |
| `--nfr` | `#d9a441` | not for review |
| `--del` | `#d8625a` | deleted |
| `--f1..f4` | `#6aa9ff` `#c792ea` `#4fd1c5` `#ff8a65` | Performance, Artistic, Remaster, Game dev |
| `--focus` | `#ffffff` | current tile outline, focus rings |
| font | `system-ui` 13 px / 1.4; mono `ui-monospace` 12 px | no web fonts |
| spacing | 4 / 8 / 12 / 16 px | `--s1..s4` |

## 4. Keyboard map

Keys are ignored while a text field has focus, except Enter and Esc. No modifiers.

| Key | Action |
|---|---|
| `J` / `→` / `↓` | next snapshot (within the filter) |
| `K` / `←` / `↑` | previous snapshot |
| `Home` / `End` | first / last |
| `G` | go to # (focus the jump box; Enter jumps, Esc closes) |
| `X` | toggle Not for review / Kept (marking it moves on to the next) |
| `D` | delete (moves to `deleted/`, then moves on); on a deleted snapshot, restore |
| `U` | undo the last delete or crop delete (and go back to it) |
| `1`-`4` | toggle facet on the snapshot; while an annotation is selected, set its facet |
| `N` | focus the note |
| `Enter` | in the note: save and leave the field (`Shift+Enter` new line) |
| `C` | new crop: drag a rectangle on the image, then type its comment, Enter saves |
| `Z` | fit / 1:1 (1:1 centres on the mouse position if over the image) |
| `F` | cycle filter: All → Kept → Not for review |
| `Esc` | leave a field / cancel a crop / deselect / clear filters, in that order |

Mouse: click a filmstrip tile to select; click a facet chip to toggle; click a header facet
count to filter by that facet; drag the image to pan in 1:1; double-click the image to toggle
1:1 at that point; click a rectangle or list row to select an annotation.

## 5. States

| Element | State | Behaviour |
|---|---|---|
| Filmstrip tile | kept / not for review / deleted | 4 px bottom bar in `--kept` / `--nfr` / `--del`; deleted tiles are 35% opacity |
| Filmstrip tile | current | 2 px `--focus` outline; auto-scrolled into view |
| Filmstrip tile | has facets / crops | one 6 px dot per facet; crop count `▭2` |
| Status pill | per status | text + colour; Deleted adds the restore hint |
| Facet chip | off / on | outline in facet colour / filled at 25% with facet-coloured text; key cap shown |
| Note | idle / editing / saved | border `--line` / `--focus`; "Saved" flashes in the header |
| Image | loading | previous image stays until the new one decodes (no flash); thumb used as placeholder |
| Image | fit / 1:1 | cursor `zoom-in` / `grab` (`grabbing` while dragging); zoom label "Fit" / "1:1" in the stage corner |
| Crop mode | armed | cursor `crosshair`; stage hint "Drag over the problem. Esc cancels." |
| Annotation | idle / hover / selected / editing | 1.5 px facet-colour (or white) outline with a number badge / 2 px + list row highlight / 3 px + handles-free redraw via "Redraw" / inline textarea |
| Save indicator | ok / pending / failed | "Saved" dim / "Saving…" / "Not saved" in `--del`, retried every 2 s, never drops a change |
| Server | unreachable at load | full-page message with the start command |

## 6. Annotations (crops)

- Stored in native image pixels: `{id, rect: [x, y, w, h], text, facet: null|"performance"|…,
  created_utc}` in the snapshot JSON's `annotations` list. `id` is `a1`, `a2`… per snapshot.
- Drawn over the image at any zoom; the number badge is the list number (1-based order).
- List under the facts: `1  [Game developer]  black pixels at the vista seam  Edit · Delete`.
  Edit turns the text into a textarea and offers "Redraw" (drag a new rect). Delete removes
  it (U restores).
- On every save the tool writes `<run>/crops/NNNN-<id>.png`: the rect plus a 10% margin each
  side, clamped to the frame, native resolution. Crops of removed annotations move to
  `crops/removed/` (nothing is ever unlinked).

## 7. Data contract (shared with the capture side; do not change)

Run folder `$SANDOVER_DATA/runs/<YYYYMMDD-HHMM>-<slug>/`, `NNNN.png` + `NNNN.json`. Triage
writes `status`, `note`, `tags`, `annotations` back into the same JSON with an atomic replace,
copies the original to `NNNN.json.bak` before its first write in a session, and moves a
deleted snapshot's files to `<run>/deleted/` (never unlinks). `<run>/triage.csv` is
rewritten on every change: one row per kept snapshot and one per annotation on a kept
snapshot; columns `index, annotation, level, tags, note, png, crop` (paths relative to the
run; `annotation`, `crop` empty on snapshot rows; `tags` is the facet on annotation rows).

## 8. Responsive

| Width | Change |
|---|---|
| ≥ 1280 px | layout above |
| 900-1279 px | rail 300 px; legend collapses to one column |
| < 900 px | rail moves under the stage (stage 60vh); filmstrip stays at the bottom |

The tool is used at a desk on a large monitor; small widths only need to stay usable.

## 9. Accessibility

Focus order: header jump box → facet chips → note → crop list → filmstrip. Chips are
`button`s with `aria-pressed`; tiles are `button`s with `aria-label="#0007 beach, kept"`;
the save indicator is `aria-live="polite"`. Every colour mark also has text (status pill,
facet names), so nothing relies on colour alone.

## 10. Run it

```
uv run --no-sync python -m sandover.triage $SANDOVER_DATA/runs/<run> --port 7895
```

Tests: `tests/test_triage.py` (store, CSV, crops, API) runs with the rest. The browser
walk-through `tests/test_triage_ui.py` needs Playwright:
`uv run --no-sync --with playwright python -m pytest tests/test_triage_ui.py`; set
`SANDOVER_TRIAGE_RUN` to walk a copy of a real run and `SANDOVER_TRIAGE_SHOTS` to keep the
screenshots (copies live in `$SANDOVER_DATA/materials/reviewer-ui/`).
