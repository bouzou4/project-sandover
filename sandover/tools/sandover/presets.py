"""Preset TOML tooling: resolve a preset to engine calls, generate headers.

The TOML under ``sandover/presets/`` is the source of truth. The engine never parses it:
rig runs send ``pc-set-*`` forms built here, and the ImGui preset list is compiled from a
header this module generates.

Usage::

    sandover-preset list ssao
    sandover-preset apply ssao contact        # print the GOAL forms
    sandover-preset apply ssao round2/deep    # an open round's candidate ([round2.presets.deep])
    sandover-preset gen-header ssao           # rewrite the engine's preset table
    sandover-preset gen-header ssao --check   # exit 1 if the header is stale
"""

from __future__ import annotations

import os

import argparse
import sys
import tomllib
from pathlib import Path

from sandover.paths import root

# pc-set-ssao-param index per preset key, in SsaoParams field order; must match gfx.h
SSAO_PARAMS = {"radius": 1, "intensity": 2, "merc_weight": 6, "luma_floor": 7, "cave_scale": 8,
               "gtao_slices": 19, "gtao_steps": 20, "gtao_full_res": 21, "gtao_thin": 22}
SSAO_INT_KEYS = {"gtao_slices", "gtao_steps", "gtao_full_res"}
SSAO_ENABLE, SSAO_VIEW, SSAO_TIMING = 0, 10, 11
SCALE = 1_000_000  # pc-set-ssao-param takes millionths
# pc-set-fur indices; must match FurOption in game/graphics/gfx.h
FUR_PARAMS = ["shells", "length_m", "density", "gravity", "curl", "root_dark"]
# optional size rule: index, engine default when a preset leaves it out
FUR_OPTIONAL = {"size_exp": (8, 1.75), "size_ref_m": (9, 0.3)}
FUR_ENABLE = 0
# pc-set-water indices; must match WaterOption in game/graphics/gfx.h
WATER_PARAMS = ["wave", "glint", "glint_exp", "fresnel", "sparkle"]
WATER_ENABLE = 0
# later params: index, value when a preset leaves them out (no waves)
WATER_OPTIONAL = {"displace": (7, 0.0), "subdiv": (8, 16), "range_full_m": (9, 300.0),
                  "range_zero_m": (19, 1200.0),
                  "crest_foam": (10, 0.0), "crest_j": (11, 0.7), "shore_foam": (12, 0.0),
                  "shore_m": (13, 0.6), "shallow": (14, 0.0), "shallow_m": (15, 2.5),
                  "sss": (16, 0.0), "crest_light": (17, 0.0), "absorb": (18, 0.0),
                  # revision 3; left out, the revision-2 look (fresnel toward the fog colour, no noise)
                  "sky_refl": (45, 0.0), "sky_haze_m": (46, 400.0), "amp_noise": (47, 0.0),
                  "phase_noise": (48, 0.0), "noise_m": (49, 180.0),
                  # left out: no opaque depth and no distance fade
                  "shallow_depth_m": (50, 10000.0), "shore_fade_near_m": (51, 10000.0),
                  "shore_fade_far_m": (52, 20000.0),
                  # left out: waves keep full height over shallow ground
                  "shoal_m": (53, 0.0),
                  # left out: single-pixel tint depth, floats not stilled
                  "shoal_occ": (91, 0.0), "depth_blur_m": (92, 0.0),
                  # left out: the transparency pick (2026-10-22)
                  "opaque_m": (102, 12.0), "absorb_r": (103, 1.6), "absorb_g": (104, 0.9),
                  "absorb_b": (105, 0.5)}
# [presets.<name>.tiling] keys: index, value when left out (one texture repeat, no crest warp)
WATER_TILING = {"hex": (54, 0.0), "hex_m": (55, 56.0), "hex_rot_deg": (56, 180.0),
                "warp_m": (57, 0.0), "warp_cell_m": (58, 180.0)}
# [presets.<name>.aa] keys: index, value when left out (no detail fade, hard foam edge)
WATER_AA = {"lod_start": (85, 99.0), "lod_end": (86, 100.0), "foam_edge_px": (87, 0.0),
            "wave_px_full": (88, 0.0), "wave_px_zero": (89, 0.0)}
# [[presets.<name>.waves]] fields, index 20 + 5 * wave + field; waves left out are flat. The
# fifth wave is the long swell. [[wave_sets.lake.waves]] go to 60 + 5 * wave + field.
WATER_WAVE_FIELDS = ["dir_deg", "amp_m", "length_m", "speed", "steep"]
WATER_WAVES, WATER_WAVE0, WATER_LAKE_WAVE0 = 5, 20, 60
WATER_BODIES = ("sea", "lake")
# pc-set-tonemap indices; must match TonemapOption in game/graphics/gfx.h (3 is paper white)
TONEMAP_PARAMS = {"mode": 0, "exposure": 1, "knee": 2, "toon_knee": 4, "toon_range": 5,
                  "white_start": 6, "white_full": 7, "halo_start": 8, "halo_full": 9}
TONEMAP_EXTERNS = ["(define-extern pc-set-hdr-target (function symbol none))",
                   "(define-extern pc-set-tonemap (function int int none))"]
# pc-set-inland-water indices; must match InlandWaterOption in game/graphics/gfx.h. Per part: the
# toggle index, then each [presets.<name>.<part>] key and its index
INLAND_PARTS = {
    "water": (0, {"depth_m": 1, "refl": 2, "refl_cap": 3, "glint": 4}),
    "mud": (5, {"dark": 6, "refl_cap": 7, "rough": 8, "glint": 9}),
}
INLAND_EXTERN = "(define-extern pc-set-inland-water (function int int none))"
INLAND_PICKED = 11  # 1: no shading, only the envmap layer dropped on sky levels
HEADER = Path("game/graphics/opengl_renderer/SsaoPresets.inc")
# pc-set-swamp indices; must match SwampOption in game/graphics/gfx.h. Per direction: the
# toggle index, then each [presets.<name>.<direction>] key and its index
SWAMP_DIRECTIONS = {
    "water": (0, {"dark": 1, "refl": 2, "ripple": 3, "glint": 4}),
    "wet": (5, {"dark": 6, "coat": 7, "puddle": 8}),
    "mist": (9, {"density": 10, "height_m": 11, "start_m": 12, "max": 13, "sun": 14,
                 "bright": 15}),
}
SWAMP_EXTERN = "(define-extern pc-set-swamp (function int int none))"
# the wet shoreline (shore.toml) rides on pc-set-swamp too, options 16-20 (SwampOption SHORE*)
SHORE_DIRECTIONS = {"wet": (16, {"band_m": 17, "dark": 18, "film": 19, "cell_m": 20})}
# the Misty Island sea mist (misty.toml), options 25-30 and 33 (SwampOption MISTY_MIST*)
MISTY_DIRECTIONS = {"mist": (25, {"density": 26, "height_m": 27, "start_m": 28, "max": 29,
                                  "bright": 30, "strength": 33})}
# wetness through the PBR roughness (wetpbr.toml), options 40-46 (SwampOption WET_PBR*)
WETPBR_DIRECTIONS = {"wet": (40, {"gloss": 41, "rough": 42, "f0": 43, "dark": 44,
                                   "splash_full_m": 45, "splash_top_m": 46})}
SWAMP_TABLES = {"swamp": SWAMP_DIRECTIONS, "shore": SHORE_DIRECTIONS, "wetpbr": WETPBR_DIRECTIONS,
                "misty": MISTY_DIRECTIONS}
# pc-set-lava indices; must match LavaOption in game/graphics/gfx.h (10 debug view, 14 clock pin)
LAVA_DIRECTIONS = {
    "glow": (0, {"gain": 1, "mask": 2, "key_lo": 3, "key_hi": 4, "core": 11, "crust": 12}),
    "under": (5, {"gain": 6, "reach_m": 7, "refl": 8, "bake": 9, "char": 13}),
}
LAVA_EXTERN = "(define-extern pc-set-lava (function int int none))"
# pc-set-caustics indices; must match CausticOption in game/graphics/gfx.h
CAUSTIC_PARAMS = {"mode": 0, "cell_scale": 1, "loop_hz": 2, "drift_mps": 3, "amp": 4, "web": 5, "web_mean": 6}
CAUSTIC_EXTERN = "(define-extern pc-set-caustics (function int int none))"
# goal_src/jak1/pc/features/bug-report.gc; BUG_REPORT_FORMS save a report at the rig's shot
BUG_REPORT_EXTERN = "(define-extern sandover-bug-report (function none))"
BUG_REPORT_FORMS = [BUG_REPORT_EXTERN, "(set! *rig-stage-hook* (lambda () (sandover-bug-report) (none)))"]
# pc-set-shadow-option indices; must match ShadowOption SKY_HUE* in game/graphics/gfx.h
SKY_HUE_TOGGLE, SKY_HUE_PARAMS = 30, {"strength": 31}
# pc-set-water indices of the sun glint; must match WaterOption SUN_GLINT* in game/graphics/gfx.h
WATER_GLINT_TOGGLE, WATER_GLINT_RESET = 94, 98
WATER_GLINT_PARAMS = {"strength": 95, "rough": 96, "peak": 97}
# pc-set-sun-option indices; must match SunOption DISC* in game/graphics/gfx.h
SUN_DISC_TOGGLE, SUN_DISC_RESET = 10, 19
SUN_DISC_PARAMS = {"size_deg": 11, "glare": 12, "core": 13, "rim": 14, "sdr_core": 15,
                   "sdr_rim": 16, "fade": 17}
# pc-set-sun-option indices; must match SunOption SKY* in game/graphics/gfx.h
SKY_BODIES_RESET = 29
SKY_BODIES_PARAMS = {"style": 20, "burst": 21, "wash": 22, "glow": 23, "moon": 24,
                     "moon_sdr": 25, "stars": 26, "moon_deg": 27, "glow_sdr": 28}


def preset_file(phase: str) -> Path:
    """``$SANDOVER_PRESETS/<phase>.toml`` when set (a branch's presets with the main root's
    goalc), else ``$SANDOVER_ROOT/sandover/presets``."""
    d = os.environ.get("SANDOVER_PRESETS")
    return (Path(d) if d else root() / "sandover" / "presets") / f"{phase}.toml"


def load(phase: str) -> dict:
    return tomllib.loads(preset_file(phase).read_text(encoding="utf-8"))


def presets(phase: str) -> dict[str, dict]:
    """Presets in file order (anchor first by convention)."""
    return load(phase)["presets"]


def ssao_param_forms(params: dict) -> list[str]:
    missing = [k for k in SSAO_PARAMS if k not in params]
    if missing:
        raise ValueError(f"ssao preset lacks {missing}")
    return [f"(pc-set-ssao-param {i} {round(float(params[k]) * SCALE)})"
            for k, i in SSAO_PARAMS.items()]


def resolve(phase: str, name: str) -> dict:
    """Params of ``name``, or of ``round<N>/<name>`` from a ``[round<N>.presets]`` table,
    whose entries are a ``base`` preset plus the keys they override."""
    data = load(phase)
    table, key = data["presets"], name
    if "/" in name:
        rnd, _, key = name.partition("/")
        table = data.get(rnd, {}).get("presets", {})
    if key not in table:
        raise ValueError(f"{phase}: unknown preset {name!r}; have {list(table)}")
    entry = table[key]
    if "base" not in entry:
        return entry
    return {**data["presets"][entry["base"]], **{k: v for k, v in entry.items() if k != "base"}}


VISTA_NB_FOG_EXTERN = "(define-extern pc-set-vista-neighbour-fog (function string int int int none))"
VISTA_HAZE_EXTERNS = ["(define-extern pc-set-vista-haze (function int int int int none))",
                      "(define-extern pc-set-vista-neighbour-haze (function string int none))"]


def _pack_tint(tint) -> str:
    r, g, b = (max(0, min(255, round(float(c) * 128))) for c in tint)
    return "#x%06x" % ((r << 16) | (g << 8) | b)


def vista_forms(params: dict) -> list[str]:
    """Neighbour mode with unified fog, the neighbour haze and the per-neighbour overrides of a
    vista preset. The externs let an older ``out/`` compile the calls (the kernel symbols are
    the gk's). A preset without ``haze_density`` turns the haze off."""
    forms = [f"(pc-set-vista-fog \"unified\" {round(float(params['fog_start_m']))} "
             f"{round(float(params['fog_density']) * 1000)} {round(float(params['fog_height_m']))})",
             VISTA_NB_FOG_EXTERN, '(pc-set-vista-neighbour-fog "clear" -1 -1 -1)',
             *VISTA_HAZE_EXTERNS,
             f"(pc-set-vista-haze {round(float(params.get('haze_density', 0.0)) * 1000)} "
             f"{round(float(params.get('haze_start_m', 200)))} "
             f"{_pack_tint(params.get('haze_tint', [1.0, 1.0, 1.0]))} "
             f"{round(float(params.get('haze_max', 1.0)) * 1000)})"]
    for level, o in params.get("neighbour_fog", {}).items():
        forms.append(f'(pc-set-vista-neighbour-fog "{level}" '
                     f"{round(float(o.get('density_scale', 1.0)) * 1000)} "
                     f"{_pack_tint(o['tint']) if 'tint' in o else -1} "
                     f"{round(float(o['max_fog']) * 1000) if 'max_fog' in o else -1})")
        if "haze_scale" in o:
            forms.append(f'(pc-set-vista-neighbour-haze "{level}" '
                         f"{round(float(o['haze_scale']) * 1000)})")
    forms += ["(pc-set-vista-option 5 (the int (&-> *time-of-day-context* time)))",
              f"(pc-set-vista-option 12 {1 if params.get('standin_levels') else 0})",
              '(pc-set-vista "neighbours")']
    return forms


def fur_param_forms(params: dict) -> list[str]:
    missing = [k for k in FUR_PARAMS if k not in params]
    if missing:
        raise ValueError(f"fur preset lacks {missing}")
    return ([f"(pc-set-fur {i + 1} {round(float(params[k]) * SCALE)})" for i, k in enumerate(FUR_PARAMS)]
            + [f"(pc-set-fur {i} {round(float(params.get(k, d)) * SCALE)})"
               for k, (i, d) in FUR_OPTIONAL.items()])


def water_param_forms(params: dict, lake_waves: list | None = None) -> list[str]:
    """pc-set-water forms of a preset; ``waves`` is the sea set, ``lake_waves`` the lake set
    (flat when None)."""
    lake_waves = lake_waves or []
    missing = [k for k in WATER_PARAMS if k not in params]
    if missing:
        raise ValueError(f"water preset lacks {missing}")
    forms = [f"(pc-set-water {i + 1} {round(float(params[k]) * SCALE)})"
             for i, k in enumerate(WATER_PARAMS)]
    forms += [f"(pc-set-water {i} {round(float(params.get(k, dflt)) * SCALE)})"
              for k, (i, dflt) in WATER_OPTIONAL.items()]
    tiling = params.get("tiling", {})
    unknown = set(tiling) - set(WATER_TILING)
    if unknown:
        raise ValueError(f"water preset tiling has unknown keys {sorted(unknown)}")
    forms += [f"(pc-set-water {i} {round(float(tiling.get(k, dflt)) * SCALE)})"
              for k, (i, dflt) in WATER_TILING.items()]
    aa = params.get("aa", {})
    unknown = set(aa) - set(WATER_AA)
    if unknown:
        raise ValueError(f"water preset aa has unknown keys {sorted(unknown)}")
    forms += [f"(pc-set-water {i} {round(float(aa.get(k, dflt)) * SCALE)})"
              for k, (i, dflt) in WATER_AA.items()]
    forms += _water_wave_forms(params.get("waves", []), WATER_WAVE0)
    forms += _water_wave_forms(lake_waves, WATER_LAKE_WAVE0)
    return forms


def _water_wave_forms(waves: list, base: int) -> list[str]:
    if len(waves) > WATER_WAVES:
        raise ValueError(f"water wave set has {len(waves)} waves; the engine takes {WATER_WAVES}")
    forms = []
    for w in range(WATER_WAVES):
        wave = waves[w] if w < len(waves) else {"amp_m": 0.0, "length_m": 1.0, "speed": 1.0}
        for f, key in enumerate(WATER_WAVE_FIELDS):
            forms.append(f"(pc-set-water {base + 5 * w + f} "
                         f"{round(float(wave.get(key, 0.0)) * SCALE)})")
    return forms


def water_bodies() -> dict[str, str]:
    """Level name -> wave set of ``[bodies]`` in water.toml (mirrored by the engine)."""
    bodies = load("water").get("bodies", {})
    bad = {k: v for k, v in bodies.items() if v not in WATER_BODIES}
    if bad:
        raise ValueError(f"water.toml [bodies]: unknown wave sets {bad}")
    return bodies


def vista_outdoor_levels() -> list[str]:
    """``[outdoor] levels`` of vista.toml: viewer levels that draw neighbours (mirrored by
    ``kOutdoor`` in Neighbours.cpp)."""
    levels = load("vista").get("outdoor", {}).get("levels", [])
    if len(set(levels)) != len(levels):
        raise ValueError("vista.toml [outdoor]: duplicate levels")
    return levels


def vista_neighbours(level: str, adjacency: list[tuple[str, str]]) -> set[str]:
    """Levels neighbour mode draws around viewer ``level`` (Neighbours.cpp ``adjacent_to``):
    none from an indoor viewer, and only outdoor levels as neighbours."""
    outdoor = set(vista_outdoor_levels())
    if level not in outdoor:
        return set()
    return {b if a == level else a for a, b in adjacency
            if level in (a, b) and (b if a == level else a) in outdoor}


def vista_keep_out() -> list[dict]:
    """``[[keep_out]]`` of vista.toml: neighbours not drawn while the camera is inside another
    level's box (mirrored by ``kKeepOut`` in Neighbours.cpp)."""
    return list(load("vista").get("keep_out", []))


def vista_kept_out(level: str, cam_m: tuple[float, float, float]) -> bool:
    """True if neighbour ``level`` is not drawn with the camera at ``cam_m`` (metres), as
    ``neighbours::kept_out``."""
    return any(k["level"] == level and all(lo <= c <= hi for c, lo, hi in
                                           zip(cam_m, k["box_min"], k["box_max"]))
               for k in vista_keep_out())


def water_body(level: str) -> str:
    """Wave set of ``level``; levels without an entry take ``sea``, as the engine does."""
    return water_bodies().get(level, "sea")
# pc-set-emissive-lights indices 1..4, keys of [emissive_lights] in emissive.toml
EMISSIVE_LIGHT_PARAMS = ["intensity", "radius_m", "falloff", "threshold"]
# world light list, indices 5..12; optional (sent only when the table has them)
WORLD_LIGHT_PARAMS = ["world_count", "world_strong", "world_weak", "world_arc", "world_particle",
                      "world_radius_scale", "world_radius_max_m", "world_lamp"]
# temporal stability, indices 13..16; optional like the world keys
TEMPORAL_LIGHT_PARAMS = ["temporal_blend", "temporal_frames", "fade_frames", "world_suppress"]
# indices past the burst (17); optional like the world keys
EXTRA_LIGHT_PARAMS = {"world_glow": 18, "highlight_strength": 21, "highlight_max": 22,
                      "highlight_damp": 23, "highlights": 20,
                      "eco_strength": 28}
EMISSIVE_LIGHTS_EXTERN = "(define-extern pc-set-emissive-lights (function int int none))"


def emissive_light_params(name: str) -> dict:
    """``[emissive_lights]`` defaults, overridden by ``[emissive_lights.presets.<name>]``
    unless ``name`` is ``default``."""
    cfg = load("emissive")["emissive_lights"]
    params = {k: cfg[k] for k in EMISSIVE_LIGHT_PARAMS}
    params.update({k: cfg[k] for k in [*WORLD_LIGHT_PARAMS, *TEMPORAL_LIGHT_PARAMS, *EXTRA_LIGHT_PARAMS]
                   if k in cfg})
    if name != "default":
        table = cfg.get("presets", {})
        if name not in table:
            raise ValueError(f"emissive: unknown preset {name!r}; have {['default', *table]}")
        params.update(table[name])
    return params


def emissive_light_forms(name: str) -> list[str]:
    """Tile lights on with preset ``name`` (``off`` disables). The extern lets an older
    ``out/`` compile the call (the kernel symbol is the gk's)."""
    if name == "off":
        return [EMISSIVE_LIGHTS_EXTERN, "(pc-set-emissive-lights 0 0)"]
    p = emissive_light_params(name)
    keys = EMISSIVE_LIGHT_PARAMS + WORLD_LIGHT_PARAMS + TEMPORAL_LIGHT_PARAMS
    return ([EMISSIVE_LIGHTS_EXTERN]
            + [f"(pc-set-emissive-lights {i + 1} {round(float(p[k]) * SCALE)})"
               for i, k in enumerate(keys) if k in p]
            + [f"(pc-set-emissive-lights {i} {round(float(p[k]) * SCALE)})"
               for k, i in EXTRA_LIGHT_PARAMS.items() if k in p]
            + [f"(pc-set-emissive-lights 0 {SCALE})"])


def tonemap_forms(params: dict) -> list[str]:
    """FP16 target on and every tonemap term of a preset, so candidates never inherit state."""
    missing = [k for k in TONEMAP_PARAMS if k not in params]
    if missing:
        raise ValueError(f"tonemap preset lacks {missing}")
    return ([*TONEMAP_EXTERNS, "(pc-set-hdr-target #t)"]
            + [f"(pc-set-tonemap {i} {round(float(params[k]) * SCALE)})"
               for k, i in TONEMAP_PARAMS.items()])


def inland_water_forms(name: str) -> list[str]:
    """The preset's parts (water, mud) on with their parameters, the rest off; ``off``: all off."""
    params = {} if name == "off" else resolve("inland-water", name)
    forms = [INLAND_EXTERN,
             f"(pc-set-inland-water {INLAND_PICKED} {SCALE if params.get('picked') else 0})"]
    for part, (toggle, keys) in INLAND_PARTS.items():
        values = params.get(part)
        if values is None:
            forms.append(f"(pc-set-inland-water {toggle} 0)")
            continue
        missing = [k for k in keys if k not in values]
        if missing:
            raise ValueError(f"inland-water preset {name!r} lacks {part} {missing}")
        forms += [f"(pc-set-inland-water {i} {round(float(values[k]) * SCALE)})"
                  for k, i in keys.items()]
        forms.append(f"(pc-set-inland-water {toggle} {SCALE})")
    return forms


def swamp_forms(name: str, phase: str = "swamp") -> list[str]:
    """The preset's parts (``SWAMP_TABLES``: swamp water, wet, mist; shore wet; wetpbr wet;
    misty mist) on with their parameters, the rest off; ``off``: all off."""
    params = {} if name == "off" else resolve(phase, name)
    forms = [SWAMP_EXTERN]
    table = SWAMP_TABLES[phase]
    for direction, (toggle, keys) in table.items():
        values = params.get(direction)
        if values is None:
            forms.append(f"(pc-set-swamp {toggle} 0)")
            continue
        missing = [k for k in keys if k not in values]
        if missing:
            raise ValueError(f"{phase} preset {name!r} lacks {direction} {missing}")
        forms += [f"(pc-set-swamp {i} {round(float(values[k]) * SCALE)})" for k, i in keys.items()]
        forms.append(f"(pc-set-swamp {toggle} {SCALE})")
    return forms


def lava_forms(name: str) -> list[str]:
    """The preset's parts (glow, under) on with their parameters, the rest off; ``off``: both off."""
    params = {} if name == "off" else resolve("lava", name)
    forms = [LAVA_EXTERN]
    for direction, (toggle, keys) in LAVA_DIRECTIONS.items():
        values = params.get(direction)
        if values is None:
            forms.append(f"(pc-set-lava {toggle} 0)")
            continue
        missing = [k for k in keys if k not in values]
        if missing:
            raise ValueError(f"lava preset {name!r} lacks {direction} {missing}")
        forms += [f"(pc-set-lava {i} {round(float(values[k]) * SCALE)})" for k, i in keys.items()]
        forms.append(f"(pc-set-lava {toggle} {SCALE})")
    return forms


def caustic_forms(name: str) -> list[str]:
    """Every pattern parameter of the preset, then its mode; ``off`` (mode 0) sends the mode only."""
    params = resolve("caustics", name)
    if not params.get("mode", 0):
        return [CAUSTIC_EXTERN, "(pc-set-caustics 0 0)"]
    missing = [k for k in CAUSTIC_PARAMS if k not in params]
    if missing:
        raise ValueError(f"caustics preset {name!r} lacks {missing}")
    return [CAUSTIC_EXTERN] + [f"(pc-set-caustics {i} {round(float(params[k]) * SCALE)})"
                               for k, i in CAUSTIC_PARAMS.items() if k != "mode"] + [
        f"(pc-set-caustics 0 {SCALE})"]


def sky_hue_forms(name: str) -> list[str]:
    """The preset's strength, then the toggle on; ``off`` sends the toggle off only."""
    if name == "off":
        return [f"(pc-set-shadow-option {SKY_HUE_TOGGLE} 0)"]
    params = resolve("sky-hue", name)
    return [f"(pc-set-shadow-option {i} {round(float(params[k]) * SCALE)})"
            for k, i in SKY_HUE_PARAMS.items()] + [f"(pc-set-shadow-option {SKY_HUE_TOGGLE} {SCALE})"]


def water_glint_forms(name: str) -> list[str]:
    """The preset's glint parameters, then the toggle on; ``off`` sends the toggle off only."""
    if name == "off":
        return [f"(pc-set-water {WATER_GLINT_TOGGLE} 0)"]
    params = resolve("water-glint", name)
    missing = [k for k in WATER_GLINT_PARAMS if k not in params]
    if missing:
        raise ValueError(f"water-glint preset {name!r} lacks {missing}")
    return [f"(pc-set-water {i} {round(float(params[k]) * SCALE)})"
            for k, i in WATER_GLINT_PARAMS.items()] + [f"(pc-set-water {WATER_GLINT_TOGGLE} {SCALE})"]


def sun_disc_forms(name: str) -> list[str]:
    """The preset's look over the engine defaults (reset first), then the disc on; ``off`` sends
    the toggle off only."""
    if name == "off":
        return [f"(pc-set-sun-option {SUN_DISC_TOGGLE} 0)"]
    params = resolve("sun-disc", name)
    return ([f"(pc-set-sun-option {SUN_DISC_RESET} 1)"]
            + [f"(pc-set-sun-option {i} {round(float(params[k]) * SCALE)})"
               for k, i in SUN_DISC_PARAMS.items() if k in params]
            + [f"(pc-set-sun-option {SUN_DISC_TOGGLE} {SCALE})"])


def sky_bodies_forms(name: str) -> list[str]:
    """The preset's sky bodies over the engine defaults (reset first), then the sun switch on;
    ``off`` sends the switch off only (no sun, moon or stars)."""
    if name == "off":
        return [f"(pc-set-sun-option {SUN_DISC_TOGGLE} 0)"]
    params = resolve("sky-bodies", name)
    return ([f"(pc-set-sun-option {SKY_BODIES_RESET} 1)"]
            + [f"(pc-set-sun-option {i} {round(float(params[k]) * SCALE)})"
               for k, i in SKY_BODIES_PARAMS.items() if k in params]
            + [f"(pc-set-sun-option {SUN_DISC_TOGGLE} {SCALE})"])


def goal_forms(phase: str, name: str) -> list[str]:
    """GOAL forms that turn the feature on with preset ``name`` (``off`` turns it off)."""
    if phase == "emissive":
        return emissive_light_forms(name)
    if phase == "inland-water":
        return inland_water_forms(name)
    if phase == "vista":
        return ['(pc-set-vista "none")'] if name == "off" else vista_forms(resolve(phase, name))
    if phase == "tonemap":
        if name == "off":
            return [TONEMAP_EXTERNS[0], "(pc-set-hdr-target #f)"]
        return tonemap_forms(resolve(phase, name))
    if phase in SWAMP_TABLES:
        return swamp_forms(name, phase)
    if phase == "lava":
        return lava_forms(name)
    if phase == "caustics":
        return caustic_forms(name)
    if phase == "sky-hue":
        return sky_hue_forms(name)
    if phase == "water-glint":
        return water_glint_forms(name)
    if phase == "sun-disc":
        return sun_disc_forms(name)
    if phase == "sky-bodies":
        return sky_bodies_forms(name)
    if phase == "fur":
        if name == "off":
            return [f"(pc-set-fur {FUR_ENABLE} 0)"]
        return fur_param_forms(resolve(phase, name)) + [f"(pc-set-fur {FUR_ENABLE} {SCALE})"]
    if phase == "water":
        if name == "off":
            return [f"(pc-set-water {WATER_ENABLE} 0)"]
        lake = load(phase).get("wave_sets", {}).get("lake", {}).get("waves", [])
        return (water_param_forms(resolve(phase, name), lake)
                + [f"(pc-set-water {WATER_ENABLE} {SCALE})"])
    if phase != "ssao":
        raise ValueError(f"no engine mapping for phase {phase!r} yet")
    if name == "off":
        return [f"(pc-set-ssao-param {SSAO_ENABLE} 0)"]
    return ssao_param_forms(resolve(phase, name)) + [f"(pc-set-ssao-param {SSAO_ENABLE} {SCALE})"]


def ssao_header() -> str:
    lines = ["// Generated from sandover/presets/ssao.toml by `sandover-preset gen-header ssao`.",
             "// Do not edit; edit the TOML and regenerate.",
             "// name, label, then SsaoParams fields in declaration order"]
    for name, p in presets("ssao").items():
        vals = ", ".join(f"{int(p[k])}" if k in SSAO_INT_KEYS else f"{float(p[k])!r}f"
                         for k in SSAO_PARAMS)
        lines.append(f'{{"{name}", "{p.get("label", name)}", {{{vals}}}}},')
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="sandover-preset", description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    ls = sub.add_parser("list", help="preset names in file order")
    ls.add_argument("phase")
    a = sub.add_parser("apply", help="print the GOAL forms for a preset")
    a.add_argument("phase")
    a.add_argument("name")
    g = sub.add_parser("gen-header", help="write the engine's built-in preset table")
    g.add_argument("phase")
    g.add_argument("--check", action="store_true", help="exit 1 if the header is out of date")
    args = ap.parse_args(argv)

    if args.cmd == "list":
        for name, p in presets(args.phase).items():
            print(f"{name}\t{p.get('role', '')}\t{p.get('label', '')}")
        return 0
    if args.cmd == "apply":
        print("\n".join(goal_forms(args.phase, args.name)))
        return 0
    if args.phase != "ssao":
        ap.error("only ssao has an engine header")
    path = root() / HEADER
    text = ssao_header()
    if args.check:
        ok = path.exists() and path.read_text(encoding="utf-8") == text
        print("up to date" if ok else f"stale: {HEADER}")
        return 0 if ok else 1
    path.write_bytes(text.encode("utf-8"))
    print(f"wrote {HEADER}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
