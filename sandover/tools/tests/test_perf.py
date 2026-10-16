"""sandover-perf: PERF log parsing, window summaries, marginal costs, toggles, the digest and
compare, and the manifest."""

import json
import re

import pytest

from sandover.paths import root
from sandover.rig import perf as P
from sandover.rig.capture import load_manifest


def window(w: int, frame: float, gpu: float, ee: float, groups: dict[str, float]) -> list[str]:
    lines = [f"[12:00] [info] PERF-GPU w={w} name=frame in=none avg_ms={gpu:.4f} min_ms=1.0 max_ms=20.0 n=60/60"]
    lines += [f"[12:00] [info] PERF-GPU w={w} name={k} in=buckets avg_ms={v:.4f} min_ms=0.1 max_ms=9.0 n=60/60"
              for k, v in groups.items()]
    lines += [f"[12:00] [info] PERF-CPU w={w} name=frame avg_ms={frame:.4f} min_ms=9.0 max_ms=20.0 n=60",
              f"[12:00] [info] PERF-CPU w={w} name=ee avg_ms={ee:.4f} min_ms=1.0 max_ms=9.0 n=60",
              f"[12:00] [info] PERF-CPU w={w} name=render avg_ms=3.0000 min_ms=1.0 max_ms=9.0 n=60",
              f"[12:00] [info] PERF-END w={w} frames=60 gpu_frames=60 fb=3840x2160 msaa=4 present=hdr-dxgi"]
    return lines


def collect(lines: list[str]) -> list[dict]:
    wins, out = P.Windows(), []
    for ln in lines:
        w = wins.feed(ln)
        if w is not None:
            out.append(w)
    return out


def test_parse_line():
    kind, w, f = P.parse_line("[info] PERF-GPU w=7 name=fur in=merc avg_ms=0.2500 min_ms=0.1 max_ms=0.4 n=30/60")
    assert (kind, w) == ("GPU", 7)
    assert f == {"name": "fur", "in": "merc", "avg": 0.25, "min": 0.1, "max": 0.4, "n": 30, "frames": 60}
    kind, w, f = P.parse_line("PERF-END w=3 frames=60 gpu_frames=58 fb=3840x2054 msaa=4 present=gl-swap")
    assert kind == "END" and f["fb"] == "3840x2054" and f["gpu_frames"] == 58 and f["present"] == "gl-swap"
    assert P.parse_line("SSAO-TIMING name=ssao-ao avg_ms=1.0 min_ms=1.0 max_ms=1.0 n=240") is None


def test_windows_and_summary():
    wins = collect(window(1, 13.0, 12.5, 4.0, {"ssao": 1.2, "tfrag": 2.0})
                   + window(2, 10.0, 6.0, 9.5, {"ssao": 1.2}))
    assert len(wins) == 2
    s = P.summary(wins[0])
    assert s["fps"] == 76.9 and s["fb"] == "3840x2160" and s["gpu_ms"] == 12.5 and "bound" not in s
    m = P.mean_window(wins)
    assert m["gpu"]["tfrag"]["avg"] == 1.0 and m["cpu"]["frame"]["avg"] == 11.5


def test_aba_marginal_noise_and_markdown():
    b1, off, b2, voff = collect(window(1, 13.0, 12.5, 4.0, {"ssao": 1.2, "tfrag": 2.0})
                                + window(2, 11.0, 10.5, 4.0, {"tfrag": 2.0})
                                + window(3, 12.6, 12.1, 4.0, {"ssao": 1.2, "tfrag": 2.0})
                                + window(4, 12.5, 12.0, 4.0, {"ssao": 1.2, "tfrag": 2.0}))
    r = P.spot_result([b1, b2], {"ssao": off, "vista": voff})
    assert r["noise"] == {"windows": 2, "frame_ms": 0.4, "gpu_ms": 0.4,
                          "groups": {"frame": 0.4, "ssao": 0.0, "tfrag": 0.0}}
    m = r["toggles"]["ssao"]["marginal"]
    assert m["frame_ms"] == 1.8 and m["groups"]["ssao"] == 1.2 and m["within_noise"] is False
    v = r["toggles"]["vista"]
    assert v["bases"] == 1 and v["marginal"]["frame_ms"] == 0.1 and v["marginal"]["within_noise"]
    result = {"meta": {"started": "t", "sha": "abc", "bin": "b", "resolution": [3840, 2160], "msaa": 4,
                       "frames": 45, "wall_s": 600.0},
              "locations": [{"name": "jungle", **r}, {"name": "ogre", "error": "timeout"}]}
    md = P.markdown(result)
    assert "| jungle | 12.80 | 78.1 | 12.30 | 96% |" in md and "| 0.40 |" in md
    assert "| jungle | 1.80 (1.80) | ~0.10 (0.10) |" in md and "ogre" not in md
    assert "GPU time or submit time" in md and "DXGI flip-model" in md


def test_toggle_forms():
    assert P.toggle_form("bloom", False) == "(set! (-> *pc-settings* sandover bloom?) #f)"
    assert P.toggle_form("hdr-output", True) == "(set! (-> *pc-settings* sandover hdr-output) 1)"
    assert list(P.TOGGLES)[-1] == "vista" and P.NO_RESTORE == {"vista"}
    off, on = P.toggle_form("swamp", False), P.toggle_form("swamp", True)
    assert off.startswith("(pc-set-swamp") and "(pc-set-swamp 9 0)" in off
    assert all(f"(pc-set-swamp {t} 1000000)" in on for t in (0, 5, 9))
    assert P.toggle_form("shore", False) == "(pc-set-swamp 16 0)" and "shore" in P.OPT_IN
    assert P.OPT_IN <= set(P.TOGGLES)
    assert "target-fps) 1000" in P.display_forms(3840, 2160, 4)


def test_perf_manifest():
    _, shots = load_manifest(root() / "sandover" / "shots" / "perf.yaml")
    names = [s.name for s in shots]
    assert len(names) == len(set(names)) and len(shots) >= 12
    assert all(s.resolution == (3840, 2160) and s.msaa == 2 and s.wait_vista for s in shots)
    assert all(s.particles and s.effects for s in shots)


def test_perf_manifest_new_pass_spots():
    _, shots = load_manifest(root() / "sandover" / "shots" / "perf.yaml")
    by = {s.name: s for s in shots}
    # baseline spot names stay, so runs compare with the 2026-09-27 baseline
    for n in ("beach-shore", "open-sea-low", "village1-night", "jungle", "maincave", "darkcave",
              "swamp", "ogre", "finalboss"):
        assert n in by
    expect = {"village1-hut-lamps": ("village1-hut", 22.4819),
              "village2-night-lamps": ("village2-start", 3.1503),
              "maincave-crystals": ("maincave-to-robocave", 12.5508),
              "darkcave-0024": ("darkcave-start", 14.9664),
              "darkcave-0025": ("darkcave-start", 12.2589)}
    for n, (cont, hour) in expect.items():
        s = by[n]
        assert (s.continue_point, s.hour) == (cont, hour)
        # snapshot cameras: explicit pose and Jak placed where the session had him
        assert isinstance(s.camera, dict) and s.jak_pos is not None
        r = s.camera["rot"]
        rows = [r[0:3], r[3:6], r[6:9]]
        for i, a in enumerate(rows):
            assert abs(sum(x * x for x in a) - 1) < 1e-4
            for b in rows[i + 1:]:
                assert abs(sum(x * y for x, y in zip(a, b))) < 1e-4
    assert by["swamp-dock1"].continue_point == "swamp-dock1"
    assert by["swamp-dock1"].camera == "from_continue"


def test_perf_manifest_swamp_cameras_match_swamp_directions():
    _, perf = load_manifest(root() / "sandover" / "shots" / "perf.yaml")
    _, sd = load_manifest(root() / "sandover" / "shots" / "swamp-directions.yaml")
    p, s = {x.name: x for x in perf}, {x.name: x for x in sd}
    assert p["swamp"].camera == s["pond-se-B-swamp"].camera
    assert p["swamp"].hour == s["pond-se-B-swamp"].hour == p["swamp-dock1"].hour
    assert p["swamp-dock1"].continue_point == s["dock1-B-swamp"].continue_point


def test_parse_counters_named_scopes_and_mean_window():
    kind, w, f = P.parse_line("[info] PERF-CNT w=4 name=shadow-maps avg=2.50 min=1 max=3 n=45")
    assert (kind, w) == ("CNT", 4) and f == {"name": "shadow-maps", "avg": 2.5, "min": 1, "max": 3, "n": 45}
    lines = window(1, 13.0, 12.0, 4.0, {"vista": 3.0}) + window(2, 13.0, 12.0, 4.0, {"vista": 2.0})
    lines.insert(1, "[info] PERF-GPU w=1 name=vista:beach in=vista avg_ms=1.2500 min_ms=1.0 max_ms=2.0 n=60/60")
    lines.insert(1, "[info] PERF-CNT w=1 name=vista-levels avg=3.00 min=3 max=3 n=60")
    wins = collect(lines)
    assert wins[0]["gpu"]["vista:beach"]["in"] == "vista" and wins[0]["cnt"]["vista-levels"]["avg"] == 3.0
    m = P.mean_window(wins)
    assert m["gpu"]["vista:beach"] == {"avg": 0.625, "in": "vista"}
    assert m["gpu"]["vista"]["in"] == "buckets" and m["cnt"]["vista-levels"]["avg"] == 3.0
    nz = P.noise(wins)
    assert nz["groups"]["vista"] == 1.0 and nz["groups"]["vista:beach"] == 1.25


def test_resolve_toggles_sets_combos_custom():
    ts = P.resolve_toggles(["vista", "ssao+bloom", "bloom", "bloom"], [("opt", "(a 0)", "(a 1)")])
    assert [t.name for t in ts] == ["ssao+bloom", "bloom", "opt", "vista"]
    combo = ts[0]
    assert combo.off == ("(set! (-> *pc-settings* sandover ssao?) #f) "
                         "(set! (-> *pc-settings* sandover bloom?) #f)")
    assert combo.on.startswith("(set! (-> *pc-settings* sandover bloom?) #t)")
    assert (ts[2].off, ts[2].on, ts[2].restore) == ("(a 0)", "(a 1)", True)
    assert not ts[-1].restore and not P.resolve_toggles(["pbr+vista"])[0].restore
    with pytest.raises(ValueError, match="nope"):
        P.resolve_toggles(["ssao+nope"])
    with pytest.raises(ValueError, match="vista, pbr\\+vista"):
        P.resolve_toggles(["vista", "pbr+vista"])
    for s in P.TOGGLE_SETS.values():
        P.resolve_toggles(s)
    assert set(P.TOGGLE_SETS["default"]) == set(P.TOGGLES) - P.OPT_IN
    assert all(t in P.TOGGLES for s in P.TOGGLE_SETS.values() for t in s)


def test_probe_toggles_match_engine():
    gfx = (root() / "game/graphics/gfx.h").read_text(encoding="utf-8")
    opts = re.search(r"enum class WaterOption \{([^}]*)\}", gfx).group(1)
    assert "TESS_SEG_M = 93," in opts and "SUBDIV = 8," in opts
    assert "float water_tess_seg_m = 0.75f;" in gfx and "int water_subdiv = 16;" in gfx
    off, on = P.toggle_form("water-tess", False), P.toggle_form("water-tess", True)
    assert "(pc-set-water 93 1000000000)" in off and "(pc-set-water 8 1000000)" in off
    assert "(pc-set-water 93 750000)" in on and "(pc-set-water 8 16000000)" in on


def test_perf_timer_tables_match_enum():
    src = root() / "game/graphics/opengl_renderer"
    h = (src / "PerfTimers.h").read_text(encoding="utf-8")
    cpp = (src / "PerfTimers.cpp").read_text(encoding="utf-8")
    groups = re.findall(r"^\s+([A-Z][A-Z0-9_]*)[,\s]", re.search(r"enum class Group : u8 \{(.*?)\};", h, re.S).group(1), re.M)
    assert groups[-1] == "COUNT"
    table = re.search(r"kInfo = \{\{(.*?)\}\};", cpp, re.S).group(1)
    names = re.findall(r'\{"([\w-]+)", Group::(\w+)\}', table)
    assert len(names) == len(groups) - 1 and len({n for n, _ in names}) == len(names)
    for n in ("ssao-ao", "emlights-track", "ocean-mid-envmap", "shadow-merc", "swamp-mist",
              "pcrtc-post", "present-flip"):
        assert n in {x for x, _ in names}
    counters = re.search(r"enum class Counter : u8 \{(.*?)\};", h, re.S).group(1)
    n_counters = len(re.findall(r"^\s+[A-Z_]+,", counters, re.M))
    assert len(re.search(r"kCounterNames = \{(.*?)\};", cpp, re.S).group(1).split(",")) == n_counters


def _run(started: str, frame: tuple[float, float], groups: list[dict], marg: float | None = None,
         windows: int = 2) -> dict:
    """A perf.json with one spot `jungle`: base windows with the given frame ms and GPU groups."""
    bases = []
    for i in range(windows):
        f = frame[i % 2]
        bases += collect(window(i + 1, f, f - 0.5, 4.0, groups[i % len(groups)]))
    offs = {}
    if marg is not None:
        f = sum(frame) / 2 - marg
        offs["bloom"] = collect(window(9, f, f - 0.5, 4.0, groups[0]))[0]
    r = P.spot_result(bases, offs)
    return {"meta": {"started": started, "sha": "abc", "bin": "b", "resolution": [3840, 2160],
                     "msaa": 4, "frames": 45, "wall_s": 1.0},
            "locations": [{"name": "jungle", "continue": "jungle-start", "hour": 12.0, **r},
                          {"name": "only-" + started, "error": "timeout"}]}


def test_digest_is_flat_and_json_ready():
    d = P.digest(_run("a", (12.0, 12.2), [{"ssao": 1.0, "vista:beach": 2.0}], marg=1.0))
    assert d["schema"] == P.SUMMARY_SCHEMA
    s = d["spots"][0]
    assert s["frame_ms"] == 12.1 and s["noise"] == {"windows": 2, "frame_ms": 0.2, "gpu_ms": 0.2}
    assert s["groups"]["ssao"] == {"ms": 1.0, "in": "buckets", "noise": 0.0}
    assert s["marginals"]["bloom"]["frame_ms"] == 1.0 and s["cpu"]["ee"] == 4.0
    assert d["spots"][1] == {"name": "only-a", "continue": None, "hour": None, "error": "timeout"}
    json.dumps(d)


def test_compare_deltas_bands_and_verdicts():
    a = P.digest(_run("a", (12.0, 12.2), [{"ssao": 1.0, "tfrag": 2.0}], marg=1.0))
    b = P.digest(_run("b", (13.0, 13.1), [{"ssao": 2.0, "tfrag": 2.02, "swamp-mist": 0.5}], marg=1.1))
    c = P.compare(a, b)
    s = c["spots"][0]
    assert s["frame_ms"] == {"a": 12.1, "b": 13.05, "delta": 0.95, "band": 0.3, "verdict": "slower"}
    g = s["groups"]
    assert list(g)[0] == "ssao" and g["ssao"]["verdict"] == "slower" and g["ssao"]["band"] == 0.0
    assert g["tfrag"]["verdict"] == "~"  # 0.02 ms: under the floor
    assert g["swamp-mist"]["a"] == 0.0 and g["swamp-mist"]["verdict"] == "slower"
    assert s["marginals"]["bloom"]["delta"] == 0.1 and s["marginals"]["bloom"]["verdict"] == "~"
    assert c["only_a"] == ["only-a"] and c["only_b"] == ["only-b"]
    assert P.compare(b, a)["spots"][0]["frame_ms"]["verdict"] == "faster"
    txt = P.compare_text(c)
    assert "| jungle | 12.10 | 13.05 | +0.95 | 0.30 | slower |" in txt
    assert "| ssao | buckets | 1.00 | 2.00 | +1.00 | 0.00 | slower |" in txt and "| tfrag |" not in txt
    assert "| tfrag |" in P.compare_text(c, all_groups=True)


def test_compare_single_window_runs_have_no_band(tmp_path):
    old = _run("old", (12.0, 12.0), [{"ssao": 1.0}], windows=1)
    (tmp_path / "old").mkdir()
    (tmp_path / "old" / "perf.json").write_text(json.dumps(old), encoding="utf-8")
    new = P.digest(_run("new", (11.0, 11.2), [{"ssao": 1.0}]))
    (tmp_path / "summary.json").write_text(json.dumps(new), encoding="utf-8")
    c = P.compare(P.load_run(str(tmp_path / "old")), P.load_run(str(tmp_path / "summary.json")))
    s = c["spots"][0]
    assert s["frame_ms"]["band"] is None and s["frame_ms"]["verdict"] == "?"
    assert s["groups"]["ssao"]["verdict"] == "~"  # no change at all is not flagged
    assert P.main(["compare", str(tmp_path / "old"), str(tmp_path / "summary.json"),
                   "--json", str(tmp_path / "c.json")]) == 0
    assert json.loads((tmp_path / "c.json").read_text(encoding="utf-8"))["spots"][0]["name"] == "jungle"


def test_markdown_sub_scopes_and_counters():
    r = _run("a", (12.0, 12.2), [{"ssao": 3.0, "vista": 2.0}])
    base = r["locations"][0]["base"]
    base["gpu"]["ssao-ao"] = {"avg": 1.5, "in": "ssao"}
    base["gpu"]["vista:beach"] = {"avg": 1.25, "in": "vista"}
    base["cnt"] = {"shadow-maps": {"avg": 3.0}}
    md = P.markdown(r)
    groups_table = md.split("## GPU ms per group")[1].split("##")[0]
    assert "ssao-ao" not in groups_table and "vista:beach" not in groups_table
    assert "### ssao" in md and "| jungle | 3.00 | 1.50 |" in md
    assert "### vista" in md and "| jungle | 2.00 | 1.25 |" in md
    assert "shadow-maps" in md and "| jungle | 3.0 |" in md


def test_build_meta_records_bin_and_proj(tmp_path):
    b = tmp_path / "bin"
    b.mkdir()
    (b / "build.json").write_text(json.dumps({"engine_sha": "abc1234", "head": "def5678", "reason": "",
                                              "staged": "t"}), encoding="utf-8")
    proj = tmp_path / "perf-base-root"
    proj.mkdir()
    m = P.build_meta(b, proj)
    assert m == {"bin_build": {"engine_sha": "abc1234", "head": "def5678", "reason": ""},
                 "gk_proj": "perf-base-root", "gk_proj_sha": None}
    assert P.build_meta(tmp_path / "missing", None)["gk_proj"] is None


def test_perf_opt_toggles_and_masks():
    assert P.OPT_DEFAULT == 0xFFFFFFFF & ~sum(1 << b for b in P.OPT_FAILED)
    D = P.OPT_RUN  # opt toggles and clear: start from the run mask
    assert D == P.OPT_DEFAULT & ~sum(1 << b for b in P.OPT_STATIC_ONLY)
    assert list(P.TOGGLES)[-1] == "vista"
    assert P.toggle_form("opt-ssao", False) == f"(pc-set-perf-opt {D & ~0xF0})"
    assert P.toggle_form("opt-geometry", False) == f"(pc-set-perf-opt {D & ~0xF00000})"
    assert P.toggle_form("opt-all", False) == "(pc-set-perf-opt 0)"
    assert all(P.toggle_form(f"opt-{a}", True) == f"(pc-set-perf-opt {D})" for a in P.OPT_AREAS)
    assert set(P.TOGGLE_SETS["opt"]) == {f"opt-{a}" for a in P.OPT_AREAS} | {"opt-all"}
    assert not set(P.TOGGLE_SETS["opt"]) & set(P.TOGGLE_SETS["default"] + P.TOGGLE_SETS["all"])
    assert P.opt_bits("0,2,8-9") == 0b1100000101
    with pytest.raises(ValueError):
        P.opt_bits("30-32")
    ts = {t.name: t for t in P.resolve_toggles(["opt-bit5", "opt-bits12-13", "vista"],
                                               [("tl-async", "clear:0", "mask:0xffffffff")])}
    assert ts["opt-bit5"].off == f"(pc-set-perf-opt {D & ~(1 << 5)})" and ts["opt-bit5"].restore
    assert ts["opt-bit5"].on == f"(pc-set-perf-opt {D})"
    assert ts["opt-bits12-13"].off == f"(pc-set-perf-opt {D & ~0x3000})"
    assert ts["tl-async"].off == f"(pc-set-perf-opt {D & ~1})"
    # mask: is literal, so a failed bit can still be measured on purpose
    assert ts["tl-async"].on == "(pc-set-perf-opt 4294967295)"
    assert P.custom_forms("(foo 1)") == "(foo 1)"


def test_static_only_bits_are_off_in_every_window(tmp_path, monkeypatch):
    """Bit 30 keeps unchanged world map cascades, which only pays with the camera and the clock
    both still, as at every spot: the harness measures with it off and offers it as a toggle."""
    gfx = (root() / "game/graphics/gfx.h").read_text(encoding="utf-8")
    core = int(re.search(r"PERF_OPT_CORE = (\d+),", gfx).group(1))
    prepass = core + int(re.search(r"PERF_OPT_WORLD_PREPASS = PERF_OPT_CORE \+ (\d+);", gfx).group(1))
    assert P.OPT_STATIC_ONLY == [prepass] == [30]
    assert not P.OPT_RUN & (1 << 30) and P.OPT_DEFAULT & (1 << 30)
    assert P.toggle_form("static-cache", False) == f"(pc-set-perf-opt {P.OPT_DEFAULT})"
    assert P.toggle_form("static-cache", True) == f"(pc-set-perf-opt {P.OPT_RUN})"
    assert "static-cache" in P.OPT_IN and "static-cache" not in P.default_toggle_names()
    assert P.custom_forms("clear:4") == f"(pc-set-perf-opt {P.OPT_RUN & ~(1 << 4)})"
    sent: list[str] = []

    class G:
        def shoot(self, shot, path):
            return {}

        def send(self, form):
            sent.append(form)

    def stop(*a, **k):
        raise StopIteration

    monkeypatch.setattr(P, "measure", stop)
    with pytest.raises(StopIteration):
        P.profile_spot(G(), type("S", (), {"name": "spot"})(), tmp_path, (3840, 2160, 2), 45, [], {})
    assert f"(pc-set-perf-opt {P.OPT_RUN})" in sent


def test_failed_bits_match_engine_default():
    gfx = (root() / "game/graphics/gfx.h").read_text(encoding="utf-8")
    failed = re.search(r"constexpr u32 PERF_OPT_FAILED = ([^;]*);", gfx).group(1)
    assert failed == "0" or "1u <<" in failed
    bits = sorted(int(b) for b in re.findall(r"1u << (\d+)", failed))
    assert bits == sorted(P.OPT_FAILED)
    assert "u32 perf_opt_mask = PERF_OPT_DEFAULT;" in gfx


def test_perf_opt_areas_match_engine():
    gfx = (root() / "game/graphics/gfx.h").read_text(encoding="utf-8")
    body = re.search(r"enum PerfOptArea : int \{(.*?)\};", gfx, re.S).group(1)
    engine = {k: int(v) for k, v in re.findall(r"PERF_OPT_(\w+) = (\d+),", body)}
    names = {"TILE_LIGHTS": "tile-lights", "SSAO": "ssao", "VISTA": "vista", "WATER": "water",
             "POST": "post", "GEOMETRY": "geometry", "PRESENT2": "present2", "CORE": "core"}
    assert {names[k]: v for k, v in engine.items()} == P.OPT_AREAS
    defs = (root() / "goal_src/jak1/kernel-defs.gc").read_text(encoding="utf-8")
    assert "(define-extern pc-set-perf-opt (function int none))" in defs
    assert "(define-extern pc-set-perf-opt (function int none))" in P.EXTERNS


def test_quiet_builds_holds_every_lane(tmp_path, monkeypatch):
    from sandover import locks
    monkeypatch.setattr(locks, "lock_path", lambda name: tmp_path / f"{name}.lock")
    with P.quiet_builds(True, "perf test") as q:
        assert q["wait_s"] >= 0
        assert all(locks.holder(lane) and "perf test" in locks.holder(lane) for lane in P.BUILD_LANES)
        assert locks.try_acquire("build") is None and locks.try_acquire("build-b") is None
    assert all(locks.holder(lane) is None for lane in P.BUILD_LANES)
    with P.quiet_builds(False, "perf test") as q:
        assert q == {"wait_s": 0.0} and locks.holder("build") is None


def test_hitch_window():
    assert P.hitch({"gpu": {"frame": {"avg": 12.0, "max": 1006.2}}})
    assert not P.hitch({"gpu": {"frame": {"avg": 12.0, "max": 23.0}}})
    assert not P.hitch({"gpu": {}})


def test_measured_sha_names_the_staged_build():
    assert P.measured_sha({"bin_build": {"engine_sha": "2a5c5fccd", "head": "x"}}, "291805608") == "2a5c5fccd"
    assert P.measured_sha({"bin_build": {"head": "abc1234"}}, "291805608") == "abc1234"
    assert P.measured_sha({"bin_build": {}}, "291805608") == "291805608 (tools checkout)"
    a = {"started": "a", "sha": "291805608", "bin": "perf-base", "bin_build": {"engine_sha": "2a5c5fccd"}}
    txt = P.compare_text({"a": a, "b": a, "floor_ms": 0.05, "spots": [], "only_a": [], "only_b": []})
    assert "build `2a5c5fccd`" in txt and "291805608" not in txt


def test_antialias_toggles():
    assert P.toggle_form("aa-smaa", False) == "(set! (-> *pc-settings* sandover antialias) 1)"
    assert P.toggle_form("aa-msaa4", False) == ("(set! (-> *pc-settings* sandover smaa?) #f) "
                                                "(set! (-> *pc-settings* gfx-msaa) 4)")
    on = P.toggle_form("aa-msaa4", True)
    assert "(set! (-> *pc-settings* gfx-msaa) 4)" in P.with_msaa(on, 4)  # a --msaa 4 run stays at 4x
    assert "(set! (-> *pc-settings* gfx-msaa) 2)" in P.with_msaa(on, 2)
    assert P.toggle_form("aa-off", True) == "(set! (-> *pc-settings* sandover antialias) 0)"
    assert set(P.TOGGLE_SETS["aa"]) <= P.OPT_IN
    assert not set(P.TOGGLE_SETS["aa"]) & set(P.TOGGLE_SETS["default"])
    assert [t.name for t in P.resolve_toggles(P.TOGGLE_SETS["aa"])] == P.AA_TOGGLES


def test_render_scale_toggles():
    assert P.TOGGLE_SETS["scale"] == [f"rs-{p}-{u}" for p in ("77", "67", "50") for u in ("bilinear", "fsr")]
    off = P.toggle_form("rs-67-fsr", False)
    assert "(set! (-> *pc-settings* sandover render-scale) 2)" in off and "upscaler) 1)" in off
    assert "render-scale) 0)" in P.toggle_form("rs-50-bilinear", True)
    assert set(P.SCALE_TOGGLES) <= P.OPT_IN and not set(P.SCALE_TOGGLES) & set(P.TOGGLE_SETS["default"])


def _fake_run(tmp_path, monkeypatch, calls: list[str], boot_error: Exception | None = None):
    """Patch main's collaborators so a run touches no game: ``Game`` records the lane state at boot."""
    import contextlib
    from sandover import locks
    monkeypatch.setattr(locks, "lock_path", lambda name: tmp_path / f"{name}.lock")
    monkeypatch.setattr(P, "other_games_running", lambda: False)
    monkeypatch.setattr(P, "load_manifest", lambda m: (None, []))
    monkeypatch.setattr(P, "build_meta", lambda b, p: {})
    monkeypatch.setattr(P, "git_sha", lambda: "x")
    monkeypatch.setattr(P.engine, "staged_bin", lambda: tmp_path / "bin")

    def lanes_state() -> str:
        return "held" if all(locks.holder(lane) for lane in P.BUILD_LANES) else "free"

    @contextlib.contextmanager
    def slot(**kw):
        calls.append("slot " + lanes_state())
        try:
            yield object()
        finally:
            calls.append("slot exit")

    real_quiet = P.quiet_builds

    @contextlib.contextmanager
    def quiet_builds(*a, **kw):
        with real_quiet(*a, **kw) as q:
            calls.append("lanes " + lanes_state())
            yield q

    class Game:
        def __init__(self, s, bin_dir):
            calls.append("game")

        def __enter__(self):
            calls.append("boot " + lanes_state())
            if boot_error:
                raise boot_error
            return self

        def __exit__(self, *exc):
            calls.append("exit")

        def send(self, form):
            pass

        def alive(self):
            return True

    monkeypatch.setattr(P.locks, "slot", slot)
    monkeypatch.setattr(P, "Game", Game)
    monkeypatch.setattr(P, "quiet_builds", quiet_builds)
    return ["--no-toggles", "--resolution", "640", "360", "--msaa", "1", "--out-dir", str(tmp_path / "out")]


def test_main_holds_build_lane_before_boot(tmp_path, monkeypatch, capsys):
    from sandover import locks
    calls: list[str] = []
    argv = _fake_run(tmp_path, monkeypatch, calls)
    assert P.main(argv) == 0
    assert calls == ["slot free", "lanes held", "game", "boot held", "exit", "slot exit"]
    out = capsys.readouterr().out
    assert "holding build lane" in out
    assert all(locks.holder(lane) is None for lane in P.BUILD_LANES)


@pytest.mark.parametrize("err", [RuntimeError("boot failed"), KeyboardInterrupt()])
def test_main_releases_build_lane_on_error(tmp_path, monkeypatch, err):
    from sandover import locks
    calls: list[str] = []
    argv = _fake_run(tmp_path, monkeypatch, calls, boot_error=err)
    with pytest.raises(type(err)):
        P.main(argv)
    assert calls[-2:] == ["boot held", "slot exit"]
    assert all(locks.holder(lane) is None for lane in P.BUILD_LANES)


def test_main_lane_timeout_boots_nothing(tmp_path, monkeypatch, capsys):
    from sandover import locks
    calls: list[str] = []
    argv = _fake_run(tmp_path, monkeypatch, calls)
    with locks.hold("build-b", "other build (timer)"):
        assert P.main([*argv, "--lane-timeout", "0"]) == 3
    assert calls == ["slot free", "slot exit"]
    err = capsys.readouterr().err
    assert "waiting for build-b lock (waited 0m00s): other build (timer)" in err
    assert "giving up, no gk booted" in err and "--lane-timeout 0" in err
    assert all(locks.holder(lane) is None for lane in P.BUILD_LANES)


def test_hold_repeats_wait_line(tmp_path, monkeypatch, capsys):
    from sandover import locks
    monkeypatch.setattr(locks, "lock_path", lambda name: tmp_path / f"{name}.lock")
    with locks.hold("build", "other build (timer)"):
        with pytest.raises(TimeoutError):
            with locks.hold("build", "perf", timeout=0.25, poll=0.05, repeat=0.1):
                pass
    lines = [ln for ln in capsys.readouterr().err.splitlines() if ln.startswith("waiting for build lock")]
    assert 2 <= len(lines) <= 4 and all("other build (timer)" in ln for ln in lines)
