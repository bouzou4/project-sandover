"""Frame profile per location and per effect, from the engine's PERF-* timer windows.

Boots one instance, visits every spot of a manifest (default ``sandover/shots/perf.yaml``) with
the play look on (the game's Sandover defaults: every effect on, HDR target, toon tonemap, PQ
output), switches the window to the requested size and MSAA with the frame limiter and vsync
off, then measures A/B/A at the same spot: base, toggle 1 off, base, toggle 2 off, base, ... Each
measurement discards one ``--frames`` timer window and keeps the next. A toggle's marginal cost
is the mean of the base windows on either side minus its off window; the spread of all base
windows at the spot is its noise band, and marginals inside it are marked. Writes ``perf.json``
(every window), ``summary.json`` (one digest per spot, for diffing runs) and ``perf.md`` to the
output directory after every spot, with the PNG the rig took at each.

A toggle is an effect name, a named probe, ``a+b`` (switched off together as one measurement),
or ``--custom NAME OFF ON`` (any GOAL forms). ``--set NAME`` picks a list from TOGGLE_SETS.

Take the numbers solo: the command refuses to start while another gk is running.

Usage::

    sandover-perf                              # perf.yaml, every spot, every default toggle
    sandover-perf --only jungle hut-roof --toggles ssao bloom ssao+bloom
    sandover-perf --no-toggles --frames 120
    sandover-perf --only swamp --toggles swamp  # opt-in toggles run only when named
    sandover-perf --set probes --only open-sea-low
    sandover-perf --bin perf-base --proj <project root of that build>
    sandover-perf compare RUN_A RUN_B          # per-spot and per-scope deltas with noise bands
"""

from __future__ import annotations

import argparse
import contextlib
import dataclasses
import json
import os
import re
import subprocess
import sys
import time
from datetime import UTC, datetime
from pathlib import Path

from sandover import engine, locks
from sandover.paths import data_dir, root
from sandover.rig.capture import Game, Shot, git_sha, load_manifest
from sandover.rig.cost import other_games_running

SUMMARY_SCHEMA = "sandover-perf-summary/1"

#: effect -> (sandover-settings field, value when off, value when on), or (None, forms when off,
#: forms when on) for a toggle without a settings field. Order = run order; vista goes last
#: because switching it back on streams the neighbours in again, so it is not restored.
TOGGLES: dict[str, tuple[str | None, str, str]] = {
    "ssao": ("ssao?", "#f", "#t"),  # tile lights need SSAO, so this includes them
    "tile-lights": ("tile-lights?", "#f", "#t"),
    "bloom": ("bloom?", "#f", "#t"),
    "shadows": ("shadows?", "#f", "#t"),
    "pbr": ("pbr?", "#f", "#t"),
    "fur": ("fur?", "#f", "#t"),
    "water": ("water?", "#f", "#t"),
    "hdr-output": ("hdr-output", "2", "1"),
    "hdr-target": ("hdr-target?", "#f", "#t"),  # also turns the tonemap and HDR output off
    # the swamp look (water, wet, mist; engine default on): measured only when named
    "swamp": (None, " ".join(f"(pc-set-swamp {i} 0)" for i in (0, 5, 9)),
              " ".join(f"(pc-set-swamp {i} 1000000)" for i in (0, 5, 9))),
    # sunken caustics pattern (engine default on): measured only when named
    "caustics": (None, "(pc-set-caustics 0 0)", "(pc-set-caustics 0 1000000)"),
    # the wet shoreline (sea levels but the swamp; engine default on): measured only when named
    "shore": (None, "(pc-set-swamp 16 0)", "(pc-set-swamp 16 1000000)"),
    # the Misty Island sea mist (engine default on): measured only when named
    "misty-mist": (None, "(pc-set-swamp 25 0)", "(pc-set-swamp 25 1000000)"),
    # local-light highlights (engine default on; needs tile lights and material shading)
    "highlights": (None, "(pc-set-emissive-lights 20 0)", "(pc-set-emissive-lights 20 1000000)"),
    # probes, not looks: water tessellation off (1 segment per edge near and far; WaterOption
    # TESS_SEG_M 93, SUBDIV 8); local lights on characters and their spot maps (#297 option 7)
    "water-tess": (None, "(pc-set-water 93 1000000000) (pc-set-water 8 1000000)",
                   "(pc-set-water 93 750000) (pc-set-water 8 16000000)"),
    "local-lights": (None, "(pc-set-shadow-option 7 0)", "(pc-set-shadow-option 7 4000000)"),
    # antialiasing in place of the base's (the run's MSAA plus the SMAA row; 2x + SMAA by default):
    # the marginal is what the base costs over the mode, so negative when the mode is dearer.
    # aa-msaa4 is 4x without SMAA; it restores the run's MSAA (RUN_MSAA, filled in per spot).
    "aa-smaa": ("antialias", "1", "0"),
    "aa-msaa4": (None, "(set! (-> *pc-settings* sandover smaa?) #f) (set! (-> *pc-settings* gfx-msaa) 4)",
                 "(set! (-> *pc-settings* gfx-msaa) <run-msaa>) (set! (-> *pc-settings* sandover smaa?) #t)"),
    "aa-off": ("antialias", "3", "0"),
    # render scale (sandover-settings render-scale 0..3 = 100/77/67/50 %, upscaler 0 bilinear /
    # 1 FSR 1) in place of native: the output size stays the run's, only the scene shrinks
    **{f"rs-{pct}-{up}": (None, f"(set! (-> *pc-settings* sandover upscaler) {ui}) "
                                f"(set! (-> *pc-settings* sandover render-scale) {ri})",
                          "(set! (-> *pc-settings* sandover render-scale) 0) "
                          "(set! (-> *pc-settings* sandover upscaler) 1)")
       for pct, ri in (("77", 1), ("67", 2), ("50", 3)) for up, ui in (("bilinear", 0), ("fsr", 1))},
    "vista": ("vista?", "#f", "#t"),
}

#: placeholder in toggle forms for the run's MSAA sample count
RUN_MSAA = "<run-msaa>"


def with_msaa(forms: str, msaa: int) -> str:
    return forms.replace(RUN_MSAA, str(msaa))


#: pc-set-perf-opt (gfx.h PerfOptArea): four bits per area, all on by default. `opt-<area>`
#: switches the area's bits off, so its marginal is minus what the area's optimizations save.
OPT_ALL = 0xFFFFFFFF
#: bits that failed their gate and are off in the engine default (gfx.h PERF_OPT_FAILED)
OPT_FAILED: list[int] = []
OPT_DEFAULT = OPT_ALL & ~sum(1 << b for b in OPT_FAILED)
#: bits that only pay off with the camera and the game clock both still, which every spot is: the
#: harness measures with them off so a spot reads what play costs (30: world map cascade cache)
OPT_STATIC_ONLY: list[int] = [30]
#: the mask every window runs with; opt toggles and ``clear:`` start from it
OPT_RUN = OPT_DEFAULT & ~sum(1 << b for b in OPT_STATIC_ONLY)
OPT_AREAS = {"tile-lights": 0, "ssao": 4, "vista": 8, "water": 12, "post": 16, "geometry": 20,
             "present2": 24, "core": 28}


def opt_form(mask: int) -> str:
    return f"(pc-set-perf-opt {mask & OPT_ALL})"


def opt_bits(spec: str) -> int:
    """Mask of a bit list such as ``5``, ``4-7`` or ``0,2,8-9``."""
    mask = 0
    for part in spec.split(","):
        a, _, b = part.partition("-")
        lo, hi = int(a), int(b or a)
        if not 0 <= lo <= hi <= 31:
            raise ValueError(f"bad perf-opt bits {spec!r}")
        for i in range(lo, hi + 1):
            mask |= 1 << i
    return mask


for _area, _bit in OPT_AREAS.items():
    TOGGLES[f"opt-{_area}"] = (None, opt_form(OPT_RUN & ~(0xF << _bit)), opt_form(OPT_RUN))
TOGGLES["opt-all"] = (None, opt_form(0), opt_form(OPT_RUN))
# the static-only bits on (the engine default): its marginal is minus what they save at a still spot
TOGGLES["static-cache"] = (None, opt_form(OPT_DEFAULT), opt_form(OPT_RUN))
TOGGLES["vista"] = TOGGLES.pop("vista")  # stays last
NO_RESTORE = {"vista"}
#: not in the default run: they change nothing outside their level, or are probes
AA_TOGGLES = ["aa-smaa", "aa-msaa4", "aa-off"]
SCALE_TOGGLES = [t for t in TOGGLES if t.startswith("rs-")]
OPT_IN = {"swamp", "shore", "misty-mist", "highlights", "caustics", "water-tess", "local-lights",
          *AA_TOGGLES, *SCALE_TOGGLES, "static-cache",
          *(t for t in TOGGLES if t.startswith("opt-"))}
#: named toggle lists for --set
TOGGLE_SETS: dict[str, list[str]] = {
    "default": [t for t in TOGGLES if t not in OPT_IN],
    "probes": ["water-tess", "local-lights"],
    "new-passes": ["swamp", "shore", "misty-mist", "highlights", "local-lights", "water-tess",
                   "shadows", "tile-lights"],
    "opt": [t for t in TOGGLES if t.startswith("opt-")],
    "aa": list(AA_TOGGLES),
    "scale": list(SCALE_TOGGLES),
    "all": [t for t in TOGGLES if not t.startswith("opt-")],
}
#: forms defined before any toggle is sent (goalc may compile an out/ that predates them)
EXTERNS = ["(define-extern pc-set-perf-timing (function int none))",
           "(define-extern pc-set-swamp (function int int none))",
           "(define-extern pc-set-caustics (function int int none))",
           "(define-extern pc-set-perf-opt (function int none))"]

#: build lanes held for the whole run, taken after the slot and before gk boots: a compile on the same CPU shows up
#: as GPU time in submit-bound groups (merc at jungle 0.47 ms quiet, 0.59-0.90 during builds)
BUILD_LANES = ("build", "build-b")


class LaneTimeout(Exception):
    """A build lane stayed busy past the wait limit."""


@contextlib.contextmanager
def quiet_builds(enabled: bool, purpose: str, timeout: float = 3600.0):
    """Hold every build lane for the block, first waiting up to ``timeout`` seconds in all for
    running builds to finish (LaneTimeout past it). Yields a dict whose ``wait_s`` is the time
    spent waiting (0 when disabled)."""
    info = {"wait_s": 0.0}
    if not enabled:
        yield info
        return
    t0 = time.monotonic()
    with contextlib.ExitStack() as stack:
        for lane in BUILD_LANES:
            left = max(0.0, timeout - (time.monotonic() - t0))
            try:
                stack.enter_context(locks.hold(lane, purpose, left, repeat=60.0))
            except TimeoutError as exc:
                raise LaneTimeout(f"{exc} after {(time.monotonic() - t0) / 60:.0f} min") from None
        info["wait_s"] = round(time.monotonic() - t0, 1)
        yield info


#: after every level load: the play defaults, pushed again by the game's settings update
ALL_ON = ["(sandover-settings-reset! (-> *pc-settings* sandover))", "(sandover-settings-reapply)"]

LINE = re.compile(r"PERF-(GPU|CPU|CNT|END) w=(\d+) (.*)$")
KV = re.compile(r"(\w+)=(\S+)")
NUM = re.compile(r"-?\d+\.\d*")


def parse_line(line: str) -> tuple[str, int, dict] | None:
    """``("GPU"|"CPU"|"CNT"|"END", window, fields)`` of one PERF log line, else None."""
    m = LINE.search(line)
    if not m:
        return None
    f: dict = {}
    for k, v in KV.findall(m.group(3)):
        if k.endswith("_ms"):
            f[k[:-3]] = float(v)
        elif k == "n" and "/" in v:
            a, b = v.split("/")
            f["n"], f["frames"] = int(a), int(b)
        elif v.isdigit():
            f[k] = int(v)
        elif NUM.fullmatch(v):
            f[k] = float(v)
        else:
            f[k] = v
    return m.group(1), int(m.group(2)), f


class Windows:
    """Collects PERF lines into complete windows (a window is complete at its PERF-END line)."""

    def __init__(self) -> None:
        self.open: dict[int, dict] = {}

    def feed(self, line: str) -> dict | None:
        p = parse_line(line)
        if not p:
            return None
        kind, w, f = p
        win = self.open.setdefault(w, {"gpu": {}, "cpu": {}, "cnt": {}, "end": {}})
        if kind == "END":
            win["end"] = f
            return self.open.pop(w)
        name = f.pop("name")
        win[kind.lower()][name] = f
        return None


def mean_window(wins: list[dict]) -> dict:
    """Mean of the ``avg`` of every GPU, CPU and counter entry (a GPU group absent from a window
    counts 0). GPU entries keep their parent (``in``)."""
    if len(wins) == 1:
        return wins[0]
    out: dict = {"gpu": {}, "cpu": {}, "cnt": {}, "end": dict(wins[-1]["end"])}
    for side in ("gpu", "cpu", "cnt"):
        for n in {n for w in wins for n in w.get(side, {})}:
            vals = [w.get(side, {}).get(n, {}).get("avg") for w in wins]
            if side == "gpu":
                vals = [0.0 if v is None else v for v in vals]
            else:
                vals = [v for v in vals if v is not None]
            if vals:
                out[side][n] = {"avg": sum(vals) / len(vals)}
                parent = next((w[side][n].get("in") for w in wins if n in w.get(side, {})), None)
                if side == "gpu" and parent:
                    out[side][n]["in"] = parent
    return out


def summary(win: dict) -> dict:
    """Frame interval, GPU span, render thread and game (EE) times of a window."""
    cpu, gpu = win["cpu"], win["gpu"]
    frame = cpu.get("frame", {}).get("avg")
    return {"frame_ms": frame, "fps": round(1000 / frame, 1) if frame else None,
            "gpu_ms": gpu.get("frame", {}).get("avg"), "ee_ms": cpu.get("ee", {}).get("avg"),
            "render_ms": cpu.get("render", {}).get("avg"),
            "wait_dma_ms": cpu.get("wait-dma", {}).get("avg"),
            "present": win["end"].get("present"), "fb": win["end"].get("fb")}


def noise(bases: list[dict]) -> dict:
    """Spread of the base windows at one spot: max - min of frame and GPU span ms, and of every
    GPU group (``groups``; a group missing from a window counts 0)."""
    out: dict = {"windows": len(bases)}
    for k in ("frame_ms", "gpu_ms"):
        v = [x for x in (summary(b)[k] for b in bases) if x is not None]
        out[k] = round(max(v) - min(v), 4) if len(v) > 1 else None
    if len(bases) > 1:
        names = {n for b in bases for n in b["gpu"]}
        out["groups"] = {}
        for n in sorted(names):
            v = [b["gpu"].get(n, {}).get("avg", 0.0) for b in bases]
            out["groups"][n] = round(max(v) - min(v), 4)
    return out


def marginal(base: dict, off: dict, band: float | None = None) -> dict:
    """What a toggle costs: base minus the window with it off (positive = it costs).
    ``within_noise`` when the frame difference is inside ``band`` (the spot's base spread)."""
    sb, so = summary(base), summary(off)
    out: dict = {}
    for k in ("frame_ms", "gpu_ms", "ee_ms", "render_ms"):
        if sb[k] is not None and so[k] is not None:
            out[k] = round(sb[k] - so[k], 4)
    groups = {}
    for name in set(base["gpu"]) | set(off["gpu"]):
        a = base["gpu"].get(name, {}).get("avg", 0.0)
        b = off["gpu"].get(name, {}).get("avg", 0.0)
        if abs(a - b) >= 0.01:
            groups[name] = round(a - b, 4)
    out["groups"] = dict(sorted(groups.items(), key=lambda kv: -abs(kv[1])))
    if band is not None and "frame_ms" in out:
        out["within_noise"] = abs(out["frame_ms"]) <= band
    return out


def spot_result(bases: list[dict], offs: dict[str, dict], no_restore: set[str] | None = None) -> dict:
    """Toggle i's off window sits between ``bases[i]`` and ``bases[i + 1]``; a toggle in
    ``no_restore`` (default: those of NO_RESTORE) has no base after it."""
    no_restore = {t for t in offs if is_no_restore(t)} if no_restore is None else no_restore
    nz = noise(bases)
    toggles = {}
    i = 0
    for t, off in offs.items():
        around = bases[i:i + 1] if t in no_restore else bases[i:i + 2]
        toggles[t] = {"window": off, "bases": len(around),
                      "marginal": marginal(mean_window(around), off, nz["frame_ms"])}
        i += 0 if t in no_restore else 1
    return {"base": mean_window(bases), "base_windows": bases, "noise": nz, "toggles": toggles}


# --- toggles -------------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class Toggle:
    name: str
    off: str
    on: str
    restore: bool = True


def is_no_restore(name: str) -> bool:
    return any(p in NO_RESTORE for p in name.split("+"))


def _forms(name: str, on: bool) -> str:
    field, off_v, on_v = TOGGLES[name]
    if field is None:
        return on_v if on else off_v
    return f"(set! (-> *pc-settings* sandover {field}) {on_v if on else off_v})"


def toggle_form(name: str, on: bool) -> str:
    """Forms that switch a toggle (``a+b``: every part, restored in reverse order)."""
    parts = name.split("+")
    return " ".join(_forms(p, on) for p in (reversed(parts) if on else parts))


OPT_NAME = re.compile(r"opt-bits?(\d+(?:-\d+)?(?:,\d+(?:-\d+)?)*)$")


def custom_forms(v: str) -> str:
    """A --custom OFF/ON value: GOAL forms, or ``mask:<int|0x..>`` (that perf-opt mask) or
    ``clear:<bits>`` (the run mask minus those bits, e.g. ``clear:4-5``)."""
    if v.startswith("mask:"):
        return opt_form(int(v[5:], 0))
    if v.startswith("clear:"):
        return opt_form(OPT_RUN & ~opt_bits(v[6:]))
    return v


def resolve_toggles(names: list[str], custom: list[tuple[str, str, str]] = ()) -> list[Toggle]:
    """Toggles in run order: the given order, except that a toggle that is not restored goes
    last. ``opt-bit5`` / ``opt-bits4-6`` switch those perf-opt bits off. Raises ValueError on an
    unknown name."""
    extra = {n: Toggle(n, custom_forms(off), custom_forms(on)) for n, off, on in custom}
    for n in names:
        m = OPT_NAME.match(n)
        if m and n not in TOGGLES:
            extra[n] = Toggle(n, opt_form(OPT_RUN & ~opt_bits(m.group(1))), opt_form(OPT_RUN))
    out, seen = [], set()
    for n in names:
        if n in seen:
            continue
        seen.add(n)
        if n in extra:
            out.append(extra[n])
            continue
        bad = [p for p in n.split("+") if p not in TOGGLES]
        if bad:
            raise ValueError(f"unknown toggle {', '.join(bad)} (known: {', '.join(TOGGLES)})")
        out.append(Toggle(n, toggle_form(n, False), toggle_form(n, True), not is_no_restore(n)))
    for n in extra:
        if n not in seen and n not in names:
            out.append(extra[n])
    last = [t.name for t in out if not t.restore]
    if len(last) > 1:
        # the second would be measured against a base that still has the first one off
        raise ValueError(f"only one toggle may stay off (not restored): {', '.join(last)}")
    return [t for t in out if t.restore] + [t for t in out if not t.restore]


def default_toggle_names() -> list[str]:
    return list(TOGGLE_SETS["default"])


# --- digest and markdown -------------------------------------------------------------------


def _r(v: float | None, nd: int = 4) -> float | None:
    return None if v is None else round(v, nd)


def digest_spot(row: dict) -> dict:
    """One spot of ``perf.json`` as a flat record: frame numbers, GPU groups with their parent
    and noise, counters, CPU times, toggle marginals."""
    out: dict = {k: row.get(k) for k in ("name", "continue", "hour")}
    if "base" not in row:
        out["error"] = row.get("error", "no base window")
        return out
    base, nz = row["base"], row.get("noise") or {}
    s = summary(base)
    out.update({k: _r(v) if isinstance(v, float) else v for k, v in s.items()})
    out["noise"] = {"windows": nz.get("windows", 1), "frame_ms": nz.get("frame_ms"),
                    "gpu_ms": nz.get("gpu_ms")}
    gn = nz.get("groups") or {}
    out["groups"] = {n: {"ms": _r(g.get("avg")), "in": g.get("in"), "noise": gn.get(n)}
                     for n, g in sorted(base["gpu"].items())}
    out["cpu"] = {n: _r(g.get("avg")) for n, g in sorted(base["cpu"].items())}
    out["counters"] = {n: _r(g.get("avg"), 2) for n, g in sorted(base.get("cnt", {}).items())}
    out["marginals"] = {t: {k: v for k, v in r["marginal"].items()}
                        for t, r in row.get("toggles", {}).items()}
    return out


def digest(result: dict) -> dict:
    """``summary.json``: the run's meta and one :func:`digest_spot` record per spot."""
    return {"schema": SUMMARY_SCHEMA, "meta": result["meta"],
            "spots": [digest_spot(r) for r in result["locations"]]}


def _f(v: float | None, nd: int = 2) -> str:
    return "-" if v is None else f"{v:.{nd}f}"


def is_top(name: str, parent: str | None) -> bool:
    """A group of the main table: not a named per-level scope, and not nested below another
    group (``frame``, ``buckets`` and their direct children)."""
    return ":" not in name and parent in (None, "none", "buckets")


def top_groups(rows: list[dict], n: int = 18) -> list[str]:
    """Top-level groups by mean GPU time over all spots, largest first (frame and buckets
    excluded)."""
    tot: dict[str, float] = {}
    for r in rows:
        for name, st in r["base"]["gpu"].items():
            if name not in ("frame", "buckets") and is_top(name, st.get("in")):
                tot[name] = tot.get(name, 0.0) + st.get("avg", 0.0)
    return [k for k, _ in sorted(tot.items(), key=lambda kv: -kv[1])][:n]


def sub_tables(rows: list[dict]) -> list[str]:
    """One table per parent group with its sub-scopes (named per-level scopes included)."""
    kids: dict[str, set[str]] = {}
    for r in rows:
        for name, st in r["base"]["gpu"].items():
            parent = st.get("in")
            if parent and not is_top(name, parent):
                kids.setdefault(parent, set()).add(name)
            elif ":" in name:
                kids.setdefault(name.split(":")[0], set()).add(name)
    out: list[str] = []
    for parent in sorted(kids):
        names = sorted(kids[parent])
        out += ["", f"### {parent}", "", f"| spot | {parent} | " + " | ".join(names) + " |",
                "|---|---|" + "---|" * len(names)]
        for r in rows:
            g = r["base"]["gpu"]
            if not any(n in g for n in names):
                continue
            out.append(f"| {r['name']} | {_f(g.get(parent, {}).get('avg'))} | "
                       + " | ".join(_f(g.get(n, {}).get("avg")) for n in names) + " |")
    return out


def markdown(result: dict) -> str:
    rows = [r for r in result["locations"] if "base" in r]
    meta = result["meta"]
    proj = f", project root `{meta['gk_proj']}`" if meta.get("gk_proj") else ""
    out = [f"# Frame profile {meta['started']}", "",
           f"Build `{meta['sha']}` ({meta['bin']}{proj}), {meta['resolution'][0]}x"
           f"{meta['resolution'][1]} MSAA {meta['msaa']}, {meta['frames']}-frame windows (one "
           f"discarded before each kept), wall time {meta.get('wall_s', 0):.0f} s.", "",
           "Group ms are GL_TIMESTAMP spans summed per frame: GPU time or submit time, whichever is "
           "longer, since a span also covers the GPU waiting for the CPU to issue the group's "
           "commands. The tile lights read a candidate row back every frame (a full CPU-GPU sync), "
           "so the render thread's time is close to the frame interval whenever they are on. `fur` "
           "is inside `merc`. Base = mean of every base window at the spot; noise = max - min of "
           "those windows. Machine-readable digest: `summary.json`.", "",
           "## Frame", "",
           "| spot | frame ms | fps | GPU span ms | GPU/frame | EE ms | render ms | wait-dma ms "
           "| noise ms | present | fb |",
           "|---|---|---|---|---|---|---|---|---|---|---|"]
    for r in rows:
        s = summary(r["base"])
        share = f"{100 * s['gpu_ms'] / s['frame_ms']:.0f}%" if s["gpu_ms"] and s["frame_ms"] else "-"
        out.append(f"| {r['name']} | {_f(s['frame_ms'])} | {_f(s['fps'], 1)} | {_f(s['gpu_ms'])} | "
                   f"{share} | {_f(s['ee_ms'])} | {_f(s['render_ms'])} | {_f(s['wait_dma_ms'])} | "
                   f"{_f(r.get('noise', {}).get('frame_ms'))} | {s['present']} | {s['fb']} |")
    groups = top_groups(rows)
    out += ["", "## GPU ms per group", "", "| spot | " + " | ".join(groups) + " |",
            "|---|" + "---|" * len(groups)]
    for r in rows:
        g = r["base"]["gpu"]
        out.append(f"| {r['name']} | " + " | ".join(_f(g.get(k, {}).get("avg")) for k in groups) + " |")
    subs = sub_tables(rows)
    if subs:
        out += ["", "## Sub-scopes", "", "Each parent's sub-scopes; they need not add up to it "
                "(state changes and uncovered draws stay in the parent only)."] + subs
    cnt = sorted({n for r in rows for n in r["base"].get("cnt", {})})
    cpu_extra = [n for n in ("present",) if any(n in r["base"]["cpu"] for r in rows)]
    if cnt or cpu_extra:
        out += ["", "## Counters and CPU", "",
                "| spot | " + " | ".join(cnt + [f"cpu {n} ms" for n in cpu_extra]) + " |",
                "|---|" + "---|" * (len(cnt) + len(cpu_extra))]
        for r in rows:
            b = r["base"]
            out.append(f"| {r['name']} | " + " | ".join(
                [_f(b.get("cnt", {}).get(n, {}).get("avg"), 1) for n in cnt]
                + [_f(b["cpu"].get(n, {}).get("avg")) for n in cpu_extra]) + " |")
    names: list[str] = []
    for r in rows:
        names += [t for t in r.get("toggles", {}) if t not in names]
    if names:
        out += ["", "## Marginal cost", "",
                "Frame ms saved with the toggle off, GPU span ms in brackets; `~` = inside the "
                "spot's noise band. Each is compared with the mean of the base windows before and "
                "after it, except `vista`, which is not switched back on (base before only). "
                "`hdr-output` compares the DXGI flip-model present with the GL swap (the `present` "
                "group); the PQ encode stays in `pcrtc` either way.", "",
                "| spot | " + " | ".join(names) + " |", "|---|" + "---|" * len(names)]
        sums: dict[str, list[float]] = {t: [] for t in names}
        for r in rows:
            cells = []
            for t in names:
                m = r.get("toggles", {}).get(t, {}).get("marginal")
                if not m or "frame_ms" not in m:
                    cells.append("-")
                    continue
                sums[t].append(m["frame_ms"])
                mark = "~" if m.get("within_noise") else ""
                cells.append(f"{mark}{m['frame_ms']:.2f} ({_f(m.get('gpu_ms'))})")
            out.append(f"| {r['name']} | " + " | ".join(cells) + " |")
        out.append("| **mean** | "
                   + " | ".join(_f(sum(v) / len(v) if v else None) for v in sums.values()) + " |")
    return "\n".join(out) + "\n"


# --- compare -------------------------------------------------------------------------------


def load_run(arg: str) -> dict:
    """A run's digest from a run directory, its ``perf.json``/``summary.json``, or a directory
    name under ``$SANDOVER_DATA/perf``."""
    p = Path(arg)
    if not p.exists():
        p = data_dir() / "perf" / arg
    if p.is_dir():
        p = p / "perf.json"
    doc = json.loads(p.read_text(encoding="utf-8"))
    if doc.get("schema") == SUMMARY_SCHEMA:
        return doc
    return digest(doc)


def _band(a: float | None, b: float | None) -> float | None:
    """Noise band of a difference between two runs: the sum of both spreads (None if either
    run has a single base window)."""
    return None if a is None or b is None else round(a + b, 4)


def _verdict(d: float, band: float | None, floor: float) -> str:
    if band is None:
        return "?" if abs(d) >= floor else "~"
    return "~" if abs(d) <= max(band, floor) else ("slower" if d > 0 else "faster")


def compare(a: dict, b: dict, floor: float = 0.05) -> dict:
    """Per-spot frame, GPU span and group deltas (B - A, positive = B slower), each with the
    noise band of the two runs and a verdict: ``faster``/``slower`` outside the band, ``~``
    inside it, ``?`` when a run has no band (one base window). ``floor`` (ms) is the smallest
    delta ever called a change."""
    sa = {s["name"]: s for s in a["spots"]}
    out: dict = {"a": a["meta"], "b": b["meta"], "floor_ms": floor, "spots": [],
                 "only_a": [n for n in sa if n not in {s["name"] for s in b["spots"]}],
                 "only_b": [s["name"] for s in b["spots"] if s["name"] not in sa]}
    for sb in b["spots"]:
        s0 = sa.get(sb["name"])
        if s0 is None:
            continue
        rec: dict = {"name": sb["name"]}
        if "error" in s0 or "error" in sb:
            rec["error"] = s0.get("error") or sb.get("error")
            out["spots"].append(rec)
            continue
        for k in ("frame_ms", "gpu_ms"):
            if s0.get(k) is None or sb.get(k) is None:
                continue
            d = round(sb[k] - s0[k], 4)
            band = _band(s0["noise"].get(k), sb["noise"].get(k))
            rec[k] = {"a": s0[k], "b": sb[k], "delta": d, "band": band,
                      "verdict": _verdict(d, band, floor)}
        def group_noise(spot: dict, g: dict) -> float | None:
            # a group a multi-window run never saw has no spread; a one-window run has none known
            if g:
                return g.get("noise")
            return 0.0 if spot["noise"].get("frame_ms") is not None else None

        groups = {}
        for n in sorted(set(s0["groups"]) | set(sb["groups"])):
            ga, gb = s0["groups"].get(n, {}), sb["groups"].get(n, {})
            va, vb = ga.get("ms") or 0.0, gb.get("ms") or 0.0
            d = round(vb - va, 4)
            band = _band(group_noise(s0, ga), group_noise(sb, gb))
            groups[n] = {"a": va, "b": vb, "delta": d, "band": band,
                         "in": gb.get("in") or ga.get("in"), "verdict": _verdict(d, band, floor)}
        rec["groups"] = dict(sorted(groups.items(), key=lambda kv: -abs(kv[1]["delta"])))
        marg = {}
        for t in sorted(set(s0.get("marginals", {})) & set(sb.get("marginals", {}))):
            ma, mb = s0["marginals"][t], sb["marginals"][t]
            if "frame_ms" in ma and "frame_ms" in mb:
                d = round(mb["frame_ms"] - ma["frame_ms"], 4)
                band = _band(s0["noise"].get("frame_ms"), sb["noise"].get("frame_ms"))
                marg[t] = {"a": ma["frame_ms"], "b": mb["frame_ms"], "delta": d, "band": band,
                           "verdict": _verdict(d, band, floor)}
        rec["marginals"] = marg
        out["spots"].append(rec)
    return out


def compare_text(c: dict, all_groups: bool = False) -> str:
    """The comparison as markdown: a spot table, then per spot every group outside its band
    (every group with ``all_groups``), then the toggle marginals both runs measured."""
    a, b = c["a"], c["b"]
    def build(m: dict) -> str:
        # runs before measured_sha recorded the tools checkout as sha; prefer build.json then
        bb = m.get("bin_build") or {}
        return bb.get("engine_sha") or bb.get("head") or m.get("sha")

    lines = ["# sandover-perf compare", "",
             f"A: {a.get('started')} build `{build(a)}` ({a.get('bin')})  ",
             f"B: {b.get('started')} build `{build(b)}` ({b.get('bin')})", "",
             "Delta = B - A in ms (positive: B slower). Band = sum of the two runs' base-window "
             f"spreads; `~` inside it (or under {c['floor_ms']} ms), `?` when a run has one base "
             "window per spot.", "",
             "| spot | frame A | frame B | delta | band | | GPU A | GPU B | delta | band | |",
             "|---|---|---|---|---|---|---|---|---|---|---|"]
    for s in c["spots"]:
        if "error" in s:
            lines.append(f"| {s['name']} | error: {s['error']} |" + " |" * 9)
            continue
        cells = []
        for k in ("frame_ms", "gpu_ms"):
            v = s.get(k)
            cells += ([_f(v["a"]), _f(v["b"]), f"{v['delta']:+.2f}", _f(v["band"]), v["verdict"]]
                      if v else ["-"] * 5)
        lines.append(f"| {s['name']} | " + " | ".join(cells) + " |")
    for s in c["spots"]:
        if "error" in s:
            continue
        gs = [(n, g) for n, g in s["groups"].items()
              if all_groups or g["verdict"] in ("faster", "slower", "?")]
        if gs:
            lines += ["", f"## {s['name']}", "", "| group | in | A | B | delta | band | |",
                      "|---|---|---|---|---|---|---|"]
            for n, g in gs:
                lines.append(f"| {n} | {g['in'] or '-'} | {_f(g['a'])} | {_f(g['b'])} | "
                             f"{g['delta']:+.2f} | {_f(g['band'])} | {g['verdict']} |")
        if s["marginals"]:
            lines += ["", f"marginals ({s['name']}): " + ", ".join(
                f"{t} {m['a']:.2f} -> {m['b']:.2f} ({m['verdict']})" for t, m in s["marginals"].items())]
    if c["only_a"] or c["only_b"]:
        lines += ["", f"only in A: {', '.join(c['only_a']) or '-'}; "
                      f"only in B: {', '.join(c['only_b']) or '-'}"]
    return "\n".join(lines) + "\n"


def compare_main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="sandover-perf compare",
                                 description="per-spot and per-scope deltas of two perf runs")
    ap.add_argument("run_a", help="run directory, perf.json/summary.json, or name under $SANDOVER_DATA/perf")
    ap.add_argument("run_b")
    ap.add_argument("--all", action="store_true", help="every group, not only those outside the band")
    ap.add_argument("--floor", type=float, default=0.05, help="smallest delta called a change (ms)")
    ap.add_argument("--json", type=Path, help="also write the comparison as JSON here")
    args = ap.parse_args(argv)
    c = compare(load_run(args.run_a), load_run(args.run_b), args.floor)
    sys.stdout.write(compare_text(c, args.all))
    if args.json:
        args.json.write_text(json.dumps(c, indent=2) + "\n", encoding="utf-8")
    return 0


# --- run -----------------------------------------------------------------------------------


#: a window holding a frame this long (ms) has a hitch (1.0-1.3 s single frames were seen in
#: base and toggle windows alike); it is replaced by the next window, up to HITCH_RETRIES times
HITCH_MS = 250.0
HITCH_RETRIES = 2


def hitch(win: dict) -> bool:
    return (win.get("gpu", {}).get("frame", {}).get("max") or 0.0) > HITCH_MS


def measure(game: Game, frames: int, timeout: float = 60.0) -> dict:
    """The second complete timer window after now, or a later one when it holds a hitch
    (``hitch_skipped`` counts the windows passed over)."""
    game.gk_log.skip_to_end()
    wins = Windows()
    done = 0
    skipped = 0
    deadline = time.monotonic() + (timeout + frames / 10) * (1 + HITCH_RETRIES)
    while True:
        left = deadline - time.monotonic()
        if left <= 0:
            raise TimeoutError("no PERF-END line; is gk built with pc-set-perf-timing?")
        m = game.gk_log.wait_for(r"PERF-(GPU|CPU|CNT|END) ", left, alive=game.alive)
        win = wins.feed(m.string)
        if win is not None:
            done += 1
            if done < 2:
                continue
            if hitch(win) and skipped < HITCH_RETRIES:
                skipped += 1
                print(f"PERF hitch: frame max {win['gpu']['frame']['max']:.0f} ms, next window",
                      flush=True)
                continue
            if skipped:
                win["hitch_skipped"] = skipped
            return win


def display_forms(w: int, h: int, msaa: int) -> str:
    """Window size and MSAA as the cost tools set them, frame limiter and vsync off."""
    return (f"(begin (set! (-> *pc-settings* gfx-msaa) {msaa}) "
            f"(set! (-> *pc-settings* width) {w}) (set! (-> *pc-settings* height) {h}) "
            f"(set-window-size! *pc-settings* {w} {h}) "
            "(set! (-> *pc-settings* vsync?) #f) (set! (-> *pc-settings* target-fps) 1000) 0)")


def line(name: str, s: dict, nz: dict | None = None) -> str:
    band = f"  noise {_f(nz.get('frame_ms'))}" if nz else ""
    return (f"PERF {name:16s} frame {_f(s['frame_ms'])} ms ({_f(s['fps'], 1)} fps)  "
            f"gpu {_f(s['gpu_ms'])}  ee {_f(s['ee_ms'])}  render {_f(s['render_ms'])}{band}  "
            f"present={s['present']} fb={s['fb']}")


def write_results(out_dir: Path, meta: dict, rows: list[dict], t0: float) -> None:
    meta["wall_s"] = round(time.monotonic() - t0, 1)
    result = {"meta": meta, "locations": rows}
    (out_dir / "perf.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    (out_dir / "summary.json").write_text(json.dumps(digest(result), indent=2) + "\n",
                                          encoding="utf-8")
    (out_dir / "perf.md").write_text(markdown(result), encoding="utf-8")


def profile_spot(game: Game, shot: Shot, out_dir: Path, size: tuple[int, int, int], frames: int,
                 toggles: list[Toggle], row: dict) -> None:
    """Shoot the spot, then base / off / base ... into ``row`` (``size`` = w, h, msaa)."""
    side = game.shoot(shot, out_dir / f"{shot.name}.png")
    row["camera_trans"] = side.get("camera_trans")
    row["wait"] = side.get("post_load", {}).get("wait")
    game.send(display_forms(*size))
    game.send(opt_form(OPT_RUN))
    game.send(f"(pc-set-perf-timing {frames})")
    bases = [measure(game, frames)]
    print(line(shot.name, summary(bases[0])), flush=True)
    offs: dict[str, dict] = {}
    for t in toggles:
        game.send(with_msaa(t.off, size[2]))
        offs[t.name] = measure(game, frames)
        if not t.restore:
            m = marginal(bases[-1], offs[t.name])
        else:
            game.send(with_msaa(t.on, size[2]))
            bases.append(measure(game, frames))
            m = marginal(mean_window(bases[-2:]), offs[t.name])
        top = ", ".join(f"{k} {v:+.2f}" for k, v in list(m["groups"].items())[:4])
        print(f"PERF {shot.name:16s} -{t.name:12s} saves frame {_f(m.get('frame_ms'))} ms, "
              f"gpu {_f(m.get('gpu_ms'))} ms  [{top}]", flush=True)
    row.update(spot_result(bases, offs, {t.name for t in toggles if not t.restore}))
    print(line(shot.name, summary(row["base"]), row["noise"]), flush=True)


def _git_head(path: Path) -> str | None:
    """HEAD of the checkout rooted at ``path``; None if ``path`` is not a checkout's top (a
    project root under $SANDOVER_DATA would otherwise report the enclosing checkout)."""
    try:
        out = subprocess.run(["git", "-C", str(path), "rev-parse", "--show-toplevel", "--short", "HEAD"],
                             capture_output=True, text=True, check=True).stdout.split()
    except (OSError, subprocess.CalledProcessError):
        return None
    if len(out) != 2 or Path(out[0]).resolve() != path.resolve():
        return None
    return out[1]


def build_meta(bin_dir: Path, proj: Path | None) -> dict:
    """What the numbers were measured on: the staged copy's build.json and the project root gk
    ran from (its directory name and git HEAD, when it is a checkout)."""
    info: dict = {}
    try:
        info = json.loads((bin_dir / engine.BUILD_INFO).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        pass
    return {"bin_build": {k: info.get(k) for k in ("engine_sha", "head", "reason") if k in info},
            "gk_proj": proj.name if proj else None,
            "gk_proj_sha": _git_head(proj) if proj else None}


def measured_sha(build: dict, tools_sha: str) -> str:
    """The engine commit of the measured build: the staged copy's build.json engine_sha, else
    its head, else (a copy without build.json) the tools checkout's HEAD, marked as such."""
    bb = build.get("bin_build") or {}
    return bb.get("engine_sha") or bb.get("head") or f"{tools_sha} (tools checkout)"


def main(argv: list[str] | None = None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    if argv and argv[0] == "compare":
        return compare_main(argv[1:])
    ap = argparse.ArgumentParser(prog="sandover-perf", description=__doc__.splitlines()[0])
    ap.add_argument("manifest", type=Path, nargs="?", default=None,
                    help="default sandover/shots/perf.yaml")
    ap.add_argument("--only", nargs="+", help="spot names, manifest order kept")
    ap.add_argument("--toggles", nargs="+", metavar="TOGGLE",
                    help=f"toggles to switch off, `a+b` for several at once (known: "
                         f"{', '.join(TOGGLES)}; default all but {', '.join(sorted(OPT_IN))})")
    ap.add_argument("--set", dest="toggle_set", choices=list(TOGGLE_SETS),
                    help="a named toggle list (TOGGLE_SETS), before any --toggles")
    ap.add_argument("--custom", nargs=3, action="append", default=[], metavar=("NAME", "OFF", "ON"),
                    help="a toggle given as GOAL forms (repeatable), e.g. a branch-only knob")
    ap.add_argument("--no-toggles", action="store_true", help="base frame only")
    ap.add_argument("--frames", type=int, default=45, help="frames per timer window")
    ap.add_argument("--resolution", nargs=2, type=int, help="override the manifest's")
    ap.add_argument("--msaa", type=int, help="override the manifest's")
    ap.add_argument("--bin", help="staged binary copy under $SANDOVER_DATA/bin")
    ap.add_argument("--proj", type=Path,
                    help="project root gk runs from (sets SANDOVER_GK_PROJ_PATH; default: that "
                         "variable, else the checkout)")
    ap.add_argument("--out-dir", type=Path)
    ap.add_argument("--allow-builds", action="store_true",
                    help="measure while builds run (default: hold both build lanes for the run, "
                         "taken before gk boots, so no compile shares the CPU with a measurement)")
    ap.add_argument("--lane-timeout", type=float, default=20.0, metavar="MIN",
                    help="give up when the build lanes stay busy this many minutes (default 20)")
    args = ap.parse_args(argv)
    if args.no_toggles:
        names: list[str] = []
    else:
        names = (TOGGLE_SETS[args.toggle_set] if args.toggle_set else []) + (args.toggles or [])
        if not names and not args.custom:
            names = default_toggle_names()
    try:
        toggles = [] if args.no_toggles else resolve_toggles(names, [tuple(c) for c in args.custom])
    except ValueError as exc:
        ap.error(str(exc))
    if args.proj:
        if not args.proj.is_dir():
            ap.error(f"--proj: no such directory: {args.proj}")
        os.environ["SANDOVER_GK_PROJ_PATH"] = str(args.proj.resolve())
    proj_env = os.environ.get("SANDOVER_GK_PROJ_PATH")
    proj = Path(proj_env) if proj_env else None
    if other_games_running():
        print("another gk is running; GPU costs must be measured solo", file=sys.stderr)
        return 2
    manifest = args.manifest or root() / "sandover" / "shots" / "perf.yaml"
    _, shots = load_manifest(manifest)
    if args.only:
        missing = set(args.only) - {s.name for s in shots}
        if missing:
            ap.error(f"not in {manifest.name}: {', '.join(sorted(missing))}")
        shots = [s for s in shots if s.name in args.only]
    started = datetime.now(UTC)
    out_dir = args.out_dir or data_dir() / "perf" / started.strftime("%Y%m%d-%H%M")
    out_dir.mkdir(parents=True, exist_ok=True)
    bin_dir = data_dir() / "bin" / args.bin if args.bin else engine.staged_bin()
    w, h = args.resolution or shots[0].resolution
    msaa = args.msaa or shots[0].msaa
    t0 = time.monotonic()
    rows: list[dict] = []
    build = build_meta(bin_dir, proj)
    meta = {"started": started.isoformat(timespec="seconds"),
            "sha": measured_sha(build, git_sha()), "tools_sha": git_sha(), "bin": bin_dir.name,
            "manifest": manifest.name, "resolution": [w, h], "msaa": msaa, "frames": args.frames,
            "toggles": [t.name for t in toggles],
            "custom": {n: [off, on] for n, off, on in args.custom}, "quiet_builds": not args.allow_builds,
            **build}
    n = 1 + sum(2 if t.restore else 1 for t in toggles)
    est = len(shots) * (30 + n * (2 * args.frames * 0.014 + 0.4))
    print(f"PERF: {len(shots)} spots x {n} measurements, estimated {est / 60:.0f} min plus boot; "
          f"results in {out_dir}", flush=True)
    purpose = f"sandover-perf bin {bin_dir.name}"
    with contextlib.ExitStack() as stack:
        # a slot is a reservation with no gk up: wait for it with the lanes free
        s = stack.enter_context(locks.slot(purpose=purpose))
        try:
            q = stack.enter_context(quiet_builds(not args.allow_builds, purpose,
                                                 args.lane_timeout * 60))
        except LaneTimeout as exc:
            print(f"PERF: giving up, no gk booted: {exc} (--lane-timeout {args.lane_timeout:g})",
                  file=sys.stderr, flush=True)
            return 3
        meta["build_wait_s"] = q["wait_s"]
        if not args.allow_builds:
            print(f"PERF: holding build lane ({', '.join(BUILD_LANES)}; waited "
                  f"{q['wait_s']:.0f} s)", flush=True)
        held = stack.pop_all()
    try:
        with held, Game(s, bin_dir) as game:
            for form in EXTERNS:
                game.send(form)
            for shot in shots:
                shot = dataclasses.replace(shot, resolution=(w, h), msaa=msaa,
                                           goals_after_load=(*shot.goals_after_load, *ALL_ON))
                row: dict = {"name": shot.name, "continue": shot.continue_point, "hour": shot.hour}
                rows.append(row)
                if not game.alive():
                    row["error"] = "game not running"
                    break
                try:
                    game.send("(pc-set-perf-timing 0)")
                    profile_spot(game, shot, out_dir, (w, h, msaa), args.frames, toggles, row)
                except Exception as exc:  # noqa: BLE001
                    # one spot's failure (a rig timeout, a malformed log line) never ends the run
                    print(f"PERF {shot.name}: failed: {type(exc).__name__}: {exc}", flush=True)
                    row["error"] = f"{type(exc).__name__}: {exc}"
                write_results(out_dir, meta, rows, t0)
            if game.alive():
                try:
                    game.send("(pc-set-perf-timing 0)")
                except OSError:
                    pass
    finally:
        write_results(out_dir, meta, rows, t0)
    ok = [r for r in rows if "base" in r]
    print(f"PERF: {len(ok)}/{len(shots)} spots in {meta['wall_s']:.0f} s; {out_dir / 'perf.md'}",
          flush=True)
    return 0 if len(ok) == len(shots) else 1


if __name__ == "__main__":
    sys.exit(main())
