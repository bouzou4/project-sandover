---
title: Pillow vs ImageMagick montage for contact sheets
date: 2026-09-23
issue: "#15"
workstream: tooling evaluation
status: verified
sha: 76e8eec
---
## Answer (<=5 lines)
Use Pillow for contact-sheet generation. It is MIT-CMU licensed, ships Windows wheels for current CPython, is already a transitive dependency (matplotlib/gradio/fiftyone), and covers grid compositing plus caption text with no new system dependency. ImageMagick's `magick montage` is a capable escape hatch for ad hoc/CLI batch work but adds a non-Python system install to manage on the target Windows workstation and via uv, and IM7 changed the command name from `montage` to `magick montage`. Keep Pillow as the default; do not add ImageMagick unless a specific batch/CLI need arises.

## Picks
| item | license (SPDX) | Windows | last release / last commit | maintenance | verdict | gotcha |
|---|---|---|---|---|---|---|
| Pillow (python-pillow/Pillow) | MIT-CMU | Windows wheels (win_amd64) published for CPython 3.12/3.13 on PyPI | release 12.3.0 (2026-07-01); repo commits as of 2026-09-23 | active: 13.8k stars, 137 open issues, commits same day as of writing | keep | `ImageFont.load_default()` is a fixed bitmap font pre-10.1.0; since Pillow 10.1.0 it loads a bundled scalable Aileron Regular TTF via FreeType with a limited (mostly Latin/ASCII) glyph set — fine for preset names/scores but ship a real TTF (e.g. `ImageFont.truetype("arial.ttf", size)`) for guaranteed glyph coverage and consistent metrics across machines |
| ImageMagick (`montage`/`magick montage`) | ImageMagick (Apache-2.0-derivative, distinct SPDX id `ImageMagick`) | Ready-to-run and portable ZIP installers for Windows | latest tag 7.1.2-31 (published 2026-09-03); repo commits as of 2026-09-23 | active: 17.5k stars, 162 open issues, commits same day as of writing | defer | IM7 folded `montage.exe` into the `magick` command (`magick montage ...`); IM6 scripts using bare `montage`/`convert` break unless legacy utilities are installed. `-label`/`caption:` text rendering needs the FreeType delegate (bundled in official Windows builds); Ghostscript is only needed for PostScript/PDF input, not for PNG contact sheets, so it is not a blocker here |

## Evidence
- Pillow license is the SPDX id `MIT-CMU`, changed from the historical HPND identifier: https://github.com/python-pillow/Pillow/issues/7942 and confirmed on PyPI JSON metadata (`license_expression: MIT-CMU`) at https://pypi.org/pypi/pillow/json
- SPDX registry entry for MIT-CMU: https://spdx.org/licenses/MIT-CMU.html
- Pillow 12.3.0 is the current PyPI release with win_amd64 wheels for cp312/cp313: https://pypi.org/pypi/pillow/json
- Pillow GitHub repo activity (stars, open issues, last push) as of 2026-09-23: `gh api repos/python-pillow/Pillow` (pushed_at 2026-09-23T03:41:29Z, 13835 stars, 137 open issues)
- `ImageFont.load_default()` is a "better than nothing" font; since Pillow 10.1.0 it loads a bundled Aileron Regular TTF via FreeType, scalable via a `size` argument, with a reduced character set: https://pillow.readthedocs.io/en/stable/releasenotes/10.1.0.html and https://pillow.readthedocs.io/en/stable/reference/ImageFont.html
- `ImageFont.truetype()` resolves a bare filename like `"arial.ttf"` on Windows by searching `%WINDIR%\fonts` when the file isn't found relative to the working directory: documented behavior per https://pillow.readthedocs.io/en/stable/reference/ImageFont.html and Pillow source (`PIL/ImageFont.py`) referenced from https://github.com/python-pillow/Pillow/blob/main/src/PIL/ImageFont.py
- `Image.MAX_IMAGE_PIXELS` defaults to ~128 Mpx (0.5 GB for RGBA); exceeding 2x that raises `DecompressionBombError`, 1x raises a warning: https://pillow.readthedocs.io/en/stable/handbook/security.html
- ImageMagick's project license is a derivative of Apache-2.0 with its own SPDX id `ImageMagick` (not `Apache-2.0` directly): https://spdx.org/licenses/ImageMagick.html and discussion at https://github.com/ImageMagick/ImageMagick/discussions/2758
- ImageMagick GitHub repo activity as of 2026-09-23: `gh api repos/ImageMagick/ImageMagick` (pushed_at 2026-09-23T01:58:01Z, 17494 stars, 162 open issues); latest release tag `7.1.2-31` published 2026-09-03T21:26:23Z via `gh api repos/ImageMagick/ImageMagick/releases`
- IM7 requires `magick montage` in place of the standalone `montage` binary unless legacy utilities are installed at setup: https://legacy.imagemagick.org/discourse-server/viewtopic.php?t=32800 and https://imagemagick.org/montage/
- Windows distribution options (installer, portable ZIP, winget) documented at https://imagemagick.org/download/
- Ghostscript is recommended for PostScript/PDF rendering, not required for raster label/caption text, which depends on the FreeType delegate: https://collectionbuilder.github.io/cb-docs/docs/software/optional/ and delegate-troubleshooting notes at https://github.com/ImageMagick/ImageMagick/discussions/7996
- opencv-python packaging license is MIT for the wrapper (underlying OpenCV is Apache-2.0); wheels run in the multi-MB to tens-of-MB range: https://pypi.org/project/opencv-python/ and https://opencv.org/license/, repo metadata via `gh api repos/opencv/opencv-python` (license spdx_id `MIT`, pushed_at 2026-09-04T08:16:50Z)

## Implications for the design
- Build the contact sheet with `Image.new` + `Image.paste` per cell and `ImageDraw.text` for the caption strip (preset name, metric score); no new dependency since Pillow is already transitive via matplotlib/gradio/fiftyone.
- Ship a bundled TTF (or resolve a known-good Windows system font by full path) instead of relying on `ImageFont.load_default()`, to control caption size/legibility and avoid depending on `%WINDIR%\fonts` contents matching between dev and target machines.
- Memory: 25 source frames at 2560x1440 RGB is roughly 25 x 2560 x 1440 x 3 bytes ≈ 265 MB fully decoded, plus the destination canvas; this is well within a 12 GB GPU's system RAM budget on the target workstation but should still downscale each source image to its target cell size with `Image.thumbnail()` (in-place, preserves aspect, releases the larger buffer) before pasting, rather than pasting full-resolution frames into a smaller canvas. `Image.draft()` only helps for JPEG decode-time downscaling, not PNG.
- Leave `Image.MAX_IMAGE_PIXELS` at its default; 2560x1440 sources and a 5x5 grid output stay far under the ~128 Mpx decompression-bomb threshold, so no override is needed.
- Do not add ImageMagick to the uv-managed environment; it is a system-level binary outside uv's dependency graph, needs its own installer/version pinning and PATH management on the target workstation, and duplicates functionality Pillow already covers. Keep it only as a manual escape hatch for one-off CLI batch composition if Pillow-based generation ever proves insufficient.

## Follow-ups
- If the caption strip ever needs bold/complex text styling (multiple weights, non-Latin scripts), verify glyph coverage of the chosen bundled TTF; the Aileron default font's character set is limited.
- If contact sheets are later expected to be interactive (e.g., clickable per-cell zoom or metadata), evaluate an HTML contact sheet instead of a raster PNG/JPEG.
- Alternatives noted but not adopted: matplotlib figure grids (already a dependency, but heavier per-image overhead and harder pixel-exact layout for photographic grids); `opencv-python` (Apache-2.0 core with MIT wrapper license, adds a large wheel primarily for compositing tasks Pillow already does); an HTML contact sheet (CSS grid + `<img>` tags) avoids raster compositing entirely and is worth considering if the review pass becomes interactive rather than a single static image for vision review.
