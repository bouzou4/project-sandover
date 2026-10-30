"""FLIP slider report: static, self-contained HTML, no reg-cli (ADR-0006).

One row per shot in a vanilla directory and a candidate directory (matched by filename
stem, like ``sandover-capture compare``): a before/after slider plus the FLIP error map as
a third pane, and a summary table sorted worst-first. All images are embedded as base64
data URIs, so the output is a single offline file with no external assets -- no CDN, no
fonts, no JS libraries.

Usage::

    uv run sandover-report --vanilla base --candidate base2 --out report.html
"""

from __future__ import annotations

import argparse
import base64
import io
import sys
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path

from jinja2 import Environment
from markupsafe import Markup

# No published FLIP threshold is trusted (ADR-0008): calibrate on this project's captures.
UNCALIBRATED_NOTE = "not calibrated -- run a no-op capture-twice pass before trusting bands"

# Wipe slider shared with sandover.compare: `--pos` on `.slider` sets the divider and the
# right-hand clip; `setupWipe` binds a `.drag` range to it.
SLIDER_CSS = """
.slider { --pos: 50%; max-width: 900px; }
.frame { position: relative; width: 100%; aspect-ratio: 16 / 9; overflow: hidden;
         background: #000; border: 1px solid #444; }
.frame img { position: absolute; inset: 0; width: 100%; height: 100%; object-fit: contain;
             background: #000; }
.right-wrap { position: absolute; inset: 0; clip-path: inset(0 0 0 var(--pos)); }
.divider { position: absolute; top: 0; bottom: 0; left: var(--pos); width: 2px;
           background: #fff; box-shadow: 0 0 4px #000; pointer-events: none; }
.controls { display: flex; align-items: center; gap: 12px; margin-top: 6px; }
.controls input[type=range] { flex: 1; }
.cycle-btn { background: #333; color: #ddd; border: 1px solid #555; border-radius: 4px;
             padding: 4px 10px; cursor: pointer; }
.mode-label { color: #999; min-width: 130px; }
"""

WIPE_JS_HELPERS = """
function setupWipe(sl) {
  var range = sl.querySelector(".drag");
  function setPos(v) { sl.style.setProperty("--pos", v + "%"); }
  range.addEventListener("input", function () { setPos(range.value); });
  setPos(range.value);
  return { range: range, setPos: setPos };
}
"""


@dataclass
class ShotResult:
    name: str
    flip_mean: float
    band: str  # "pass" | "near" | "fail" | "uncalibrated"
    vanilla_b64: str
    candidate_b64: str
    error_b64: str


def _image_to_b64(path: Path) -> str:
    from PIL import Image

    with Image.open(path) as im:
        buf = io.BytesIO()
        im.convert("RGB").save(buf, format="JPEG", quality=88)
        return base64.b64encode(buf.getvalue()).decode("ascii")


def _array_to_b64(arr) -> str:
    """A float [0,1] HxWx3 array (FLIP's magma-colored error map) -> base64 JPEG."""
    import numpy as np
    from PIL import Image

    pixels = (np.clip(np.asarray(arr, dtype="float32"), 0.0, 1.0) * 255).astype("uint8")
    buf = io.BytesIO()
    Image.fromarray(pixels, mode="RGB").save(buf, format="JPEG", quality=88)
    return base64.b64encode(buf.getvalue()).decode("ascii")


def band_for(mean: float, threshold: float | None, near_multiplier: float) -> str:
    if threshold is None:
        return "uncalibrated"
    if mean <= threshold:
        return "pass"
    if mean <= threshold * near_multiplier:
        return "near"
    return "fail"


def compare_shot(vanilla_png: Path, candidate_png: Path,
                  threshold: float | None, near_multiplier: float) -> ShotResult:
    import flip_evaluator as flip

    fmap, mean, _ = flip.evaluate(str(vanilla_png), str(candidate_png), "LDR", applyMagma=True)
    return ShotResult(
        name=vanilla_png.stem,
        flip_mean=float(mean),
        band=band_for(float(mean), threshold, near_multiplier),
        vanilla_b64=_image_to_b64(vanilla_png),
        candidate_b64=_image_to_b64(candidate_png),
        error_b64=_array_to_b64(fmap),
    )


def collect_shots(vanilla_dir: Path, candidate_dir: Path,
                   threshold: float | None, near_multiplier: float) -> list[ShotResult]:
    shots = []
    for vp in sorted(vanilla_dir.glob("*.png")):
        cp = candidate_dir / vp.name
        if not cp.exists():
            continue
        shots.append(compare_shot(vp, cp, threshold, near_multiplier))
    shots.sort(key=lambda s: s.flip_mean, reverse=True)
    return shots


TEMPLATE = r"""<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>FLIP report{{ ' -- ' + preset if preset else '' }}</title>
<style>
:root { color-scheme: dark; }
body { background: #1e1e1e; color: #ddd; font: 14px/1.4 -apple-system, "Segoe UI", sans-serif;
       margin: 0; padding: 24px; }
h1 { font-size: 20px; margin: 0 0 4px; }
.meta { color: #999; margin-bottom: 20px; }
.meta span { margin-right: 18px; }
table.summary { border-collapse: collapse; margin-bottom: 32px; width: 100%; max-width: 720px; }
table.summary th, table.summary td { border: 1px solid #444; padding: 6px 10px; text-align: left; }
table.summary a { color: #7ab8ff; }
.band-pass { color: #7fd77f; }
.band-near { color: #e8c15a; }
.band-fail { color: #e86a6a; }
.band-uncalibrated { color: #999; }
.shot { margin-bottom: 36px; }
.shot h2 { font-size: 16px; margin: 0 0 8px; }
{{ slider_css }}
</style>
</head>
<body>
<h1>FLIP report{{ ' -- ' + preset if preset else '' }}</h1>
<div class="meta">
<span>commit: {{ commit }}</span>
<span>threshold: {{ ('%.4f' % threshold) if threshold is not none else uncalibrated_note }}</span>
<span>noise floor: {{ ('%.4f' % noise_floor) if noise_floor is not none else 'not measured' }}</span>
<span>generated: {{ generated }}</span>
</div>
<table class="summary">
<tr><th>shot</th><th>FLIP mean</th><th>band</th><th>&nbsp;</th></tr>
{% for s in shots %}
<tr>
<td>{{ s.name }}</td>
<td>{{ '%.4f' % s.flip_mean }}</td>
<td class="band-{{ s.band }}">{{ s.band }}</td>
<td><a href="#shot-{{ s.name }}">view</a></td>
</tr>
{% endfor %}
</table>
{% for s in shots %}
<div class="shot" id="shot-{{ s.name }}">
<h2>{{ s.name }} -- mean FLIP {{ '%.4f' % s.flip_mean }} ({{ s.band }})</h2>
<div class="slider">
<div class="frame">
<img class="left" src="data:image/jpeg;base64,{{ s.vanilla_b64 }}" alt="vanilla">
<div class="right-wrap">
<img class="right" src="data:image/jpeg;base64,{{ s.candidate_b64 }}"
     data-error="data:image/jpeg;base64,{{ s.error_b64 }}" alt="candidate">
</div>
<div class="divider"></div>
</div>
<div class="controls">
<input type="range" class="drag" min="0" max="100" value="50">
<button class="cycle-btn" type="button">cycle right pane (E)</button>
<span class="mode-label">right: candidate</span>
</div>
</div>
</div>
{% endfor %}
<script>
{{ wipe_js }}
(function () {
  var modes = ["candidate", "error", "vanilla"];
  var active = null;
  document.querySelectorAll(".slider").forEach(function (sl) {
    var wipe = setupWipe(sl);
    var rightImg = sl.querySelector(".right");
    var label = sl.querySelector(".mode-label");
    var btn = sl.querySelector(".cycle-btn");
    var modeIdx = 0;
    // Each image is embedded once; the vanilla and candidate panes reuse existing srcs.
    var srcs = { candidate: rightImg.src, error: rightImg.dataset.error,
                 vanilla: sl.querySelector(".left").src };
    function cycle() {
      modeIdx = (modeIdx + 1) % modes.length;
      var m = modes[modeIdx];
      rightImg.src = srcs[m];
      label.textContent = "right: " + m;
    }
    btn.addEventListener("click", cycle);
    sl.addEventListener("mouseenter", function () { active = sl; });
  });
  document.addEventListener("keydown", function (e) {
    if (e.key.toLowerCase() === "e" && active) {
      active.querySelector(".cycle-btn").click();
    }
  });
})();
</script>
</body>
</html>
"""


def render_report(shots: list[ShotResult], *, preset: str | None, commit: str,
                   threshold: float | None, noise_floor: float | None) -> str:
    env = Environment(autoescape=True)
    template = env.from_string(TEMPLATE)
    return template.render(
        shots=shots,
        preset=preset,
        commit=commit,
        threshold=threshold,
        noise_floor=noise_floor,
        uncalibrated_note=UNCALIBRATED_NOTE,
        generated=datetime.now(UTC).isoformat(timespec="seconds"),
        slider_css=Markup(SLIDER_CSS),
        wipe_js=Markup(WIPE_JS_HELPERS),
    )


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="sandover-report", description=__doc__.splitlines()[0])
    ap.add_argument("--vanilla", type=Path, required=True, help="directory of vanilla PNGs")
    ap.add_argument("--candidate", type=Path, required=True, help="directory of candidate PNGs")
    ap.add_argument("--out", type=Path, required=True, help="output HTML path")
    ap.add_argument("--preset", help="preset label shown in the header")
    ap.add_argument("--commit", default="unknown")
    ap.add_argument("--threshold", type=float, help="calibrated FLIP pass ceiling")
    ap.add_argument("--near-multiplier", type=float, default=1.5,
                     help="near-band ceiling as a multiple of --threshold (default 1.5)")
    ap.add_argument("--noise-floor", type=float, help="mean FLIP of a no-op capture-twice pass")
    args = ap.parse_args(argv)

    shots = collect_shots(args.vanilla, args.candidate, args.threshold, args.near_multiplier)
    if not shots:
        print("no common PNGs between --vanilla and --candidate", file=sys.stderr)
        return 1

    html = render_report(shots, preset=args.preset, commit=args.commit,
                          threshold=args.threshold, noise_floor=args.noise_floor)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(html, encoding="utf-8")
    print(f"wrote {args.out} ({len(shots)} shots)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
