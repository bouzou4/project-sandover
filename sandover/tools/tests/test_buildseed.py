"""Configure seed (sandover.buildseed) and the worktree build command (engine.configure_cmd)."""

import json
import re
from pathlib import Path

from sandover import buildseed, engine

REPO = Path(__file__).resolve().parents[3]

LOG = """
---
events:
  -
    kind: "try_compile-v1"
    backtrace:
      - "C:/cmake/Modules/CheckSymbolExists.cmake:171 (try_compile)"
      - "third-party/lib/CMakeLists.txt:12 (check_symbol_exists)"
    checks:
      - "Looking for ceil"
    buildResult:
      variable: "LIBC_HAS_CEIL"
      cached: true
      exitCode: 0
  -
    kind: "try_compile-v1"
    backtrace:
      - "C:/cmake/Modules/CheckTypeSize.cmake:93 (try_compile)"
      - "third-party/lib/cmake/checks.cmake:3 (check_type_size)"
      - "third-party/lib/CMakeLists.txt:20 (include)"
    buildResult:
      variable: "HAVE_SIZEOF_LONG"
      cached: true
  -
    kind: "try_compile-v1"
    backtrace:
      - "CMakeLists.txt:5 (try_compile)"
    buildResult:
      variable: "RAW_RESULT"
      cached: false
  -
    kind: "find-v1"
    backtrace:
      - "CMakeLists.txt:7 (find_program)"
...
"""


def _fake_build(tmp_path):
    src = tmp_path / "src"
    (src / "third-party" / "lib" / "cmake").mkdir(parents=True)
    (src / "third-party" / "lib" / "CMakeLists.txt").write_text("project(lib)\n")
    for gate in buildseed.GATE_FILES:
        (src / gate).write_text("{}\n")
    (src / "third-party" / "lib" / "cmake" / "checks.cmake").write_text("check_type_size(long SIZEOF_LONG)\n")
    build = src / "out" / "build" / "Release"
    (build / "CMakeFiles").mkdir(parents=True)
    (build / "CMakeFiles" / "CMakeConfigureLog.yaml").write_text(LOG)
    (build / "CMakeCache.txt").write_text(
        f"CMAKE_HOME_DIRECTORY:INTERNAL={src.as_posix()}\n"
        "CMAKE_BUILD_TYPE:STRING=Release\nCMAKE_C_COMPILER:STRING=C:/llvm/bin/clang.exe\n"
        "LIBC_HAS_CEIL:INTERNAL=1\nHAVE_SIZEOF_LONG:INTERNAL=TRUE\nSIZEOF_LONG:INTERNAL=4\n"
        "RAW_RESULT:INTERNAL=1\nSOME_PATH:FILEPATH=C:/x/y\n"
        "CMAKE_CACHE_MAJOR_VERSION:INTERNAL=4\nCMAKE_CACHE_MINOR_VERSION:INTERNAL=4\n"
        "CMAKE_CACHE_PATCH_VERSION:INTERNAL=3\n")
    return src, build


def test_collect_groups_cached_checks_by_defining_files(tmp_path):
    src, build = _fake_build(tmp_path)
    groups, _ = buildseed.collect(build, src)
    assert groups == {
        ("third-party/lib/CMakeLists.txt",): {"LIBC_HAS_CEIL": "1"},
        ("third-party/lib/CMakeLists.txt", "third-party/lib/cmake/checks.cmake"):
            {"HAVE_SIZEOF_LONG": "TRUE", "SIZEOF_LONG": "4"},
    }  # uncached raw try_compile results are never seeded


def test_rendered_seed_holds_no_paths(tmp_path, monkeypatch):
    src, build = _fake_build(tmp_path)
    monkeypatch.setattr(buildseed, "clang_version", lambda cc="clang": "clang version 1.2.3")
    monkeypatch.setattr(buildseed, "toolset", lambda cc="clang": "msvc 1.0 sdk 2.0")
    text = buildseed.harvest(build, src, tmp_path / "seed.cmake").read_text()
    assert str(tmp_path.as_posix()) not in text and not re.search(r"(?<![A-Za-z])[A-Za-z]:[/\\]", text)
    assert 'set(SIZEOF_LONG "4" CACHE INTERNAL "")' in text
    assert buildseed.file_hash(src / "third-party/lib/CMakeLists.txt") in text


def test_file_hash_ignores_line_endings(tmp_path):
    (tmp_path / "a").write_bytes(b"x\r\ny\r\n")
    (tmp_path / "b").write_bytes(b"x\ny\n")
    assert buildseed.file_hash(tmp_path / "a") == buildseed.file_hash(tmp_path / "b")


def test_committed_seed_matches_preset_and_checkout():
    seed = buildseed.seed_path(REPO)
    text = seed.read_text(encoding="utf-8")
    assert not re.search(r"(?<![A-Za-z])[A-Za-z]:[/\\]", text), "seed must not embed machine paths"
    presets = json.loads((REPO / "CMakePresets.json").read_text(encoding="utf-8"))
    names = {p["name"] for p in presets["configurePresets"]}
    assert buildseed.PRESET in names
    groups = [re.findall(r'"([^"]+)" ([0-9a-f]{64})', m.group(1))
              for m in re.finditer(r"^_seed_group\((.*)\)$", text, re.M)]
    # whole-seed gate first: the top level sets flags and options every check sees
    assert [f for f, _ in groups[0]] == list(buildseed.GATE_FILES)
    keyed = {f for g in groups for f, _ in g}
    assert "third-party/curl/CMake/CurlTests.c" in keyed  # try_compile sources are keyed too
    stale = sorted({rel for g in groups for rel, h in g
                    if not (REPO / rel).exists() or buildseed.file_hash(REPO / rel) != h})
    assert not stale, f"configure seed is stale for {stale}; re-run `python -m sandover.buildseed harvest`"


def test_configure_cmd_uses_seed_and_resolved_tools(tmp_path, monkeypatch):
    seed = buildseed.seed_path(tmp_path)
    seed.parent.mkdir(parents=True)
    seed.write_text("")
    monkeypatch.setattr(engine, "real_exe", lambda n: f"/tools/{n}.exe")
    cmd = engine.configure_cmd(tmp_path)
    assert cmd[:3] == ["cmake", "-C", str(seed)]
    assert f"--preset={buildseed.PRESET}" in cmd
    assert "-DCMAKE_CXX_COMPILER_LAUNCHER=/tools/ccache.exe" in cmd
    assert "-DCMAKE_MAKE_PROGRAM=/tools/ninja.exe" in cmd
    assert not any(a == "-j" or a.startswith("-j") for a in cmd)
    cache = tmp_path / "out" / "build" / "Release" / "CMakeCache.txt"
    cache.parent.mkdir(parents=True)
    cache.write_text("")
    assert "-C" not in engine.configure_cmd(tmp_path)  # never re-seed an existing tree


def test_configure_cmd_without_seed_or_ccache(tmp_path, monkeypatch):
    monkeypatch.setattr(engine, "real_exe", lambda n: None)
    assert engine.configure_cmd(tmp_path) == ["cmake", f"--preset={buildseed.PRESET}"]


def _shim(tmp_path, monkeypatch, extra=""):
    real = tmp_path / "apps" / "ninja.exe"
    real.parent.mkdir()
    real.write_bytes(b"")
    shim = tmp_path / "shims" / "ninja.exe"
    shim.parent.mkdir()
    shim.write_bytes(b"")
    shim.with_suffix(".shim").write_text(f'path = "{real}"\n{extra}')
    monkeypatch.setattr(engine.shutil, "which", lambda n: str(shim))
    return real, shim


def test_scoop_shim_resolves_to_its_exe(tmp_path, monkeypatch):
    real, _ = _shim(tmp_path, monkeypatch)
    assert engine.real_exe("ninja") == str(real)


def test_scoop_shim_with_args_is_kept(tmp_path, monkeypatch):
    _, shim = _shim(tmp_path, monkeypatch, 'args = "--flag"\n')
    assert engine.real_exe("ninja") == str(shim)


def test_jobs_computed_at_run_time(monkeypatch):
    monkeypatch.setattr(engine.os, "cpu_count", lambda: 16)
    assert engine.build_jobs() == 14
    monkeypatch.setattr(engine.os, "cpu_count", lambda: 8)
    assert engine.build_jobs() == 6


def test_hold_log_records_each_hold(tmp_path, monkeypatch):
    from sandover import locks
    monkeypatch.setenv("SANDOVER_DATA", str(tmp_path))
    with locks.hold("build-test", "configure + ninja gk\tsecond line"):
        pass
    with open(tmp_path / "locks" / locks.HOLD_LOG, "a", encoding="utf-8") as f:
        f.write("2026-10-15T00:00:00+00:00\tbuild\t12")  # cut short by a crash
    [e] = locks.hold_log()
    assert e["lock"] == "build-test" and e["purpose"] == "configure + ninja gk second line"
    assert e["seconds"] >= 0 and locks.hold_log(min_s=3600) == []


# --- the seed itself, run by CMake in script mode against a copy of the files it keys on ---

def _cmake_or_skip():
    import shutil

    import pytest
    if shutil.which("cmake") is None or shutil.which("clang") is None:
        pytest.skip("cmake/clang not on PATH")


def _seed_tree(tmp_path):
    """tmp_path holding the committed seed and a copy of every file it hashes."""
    import shutil
    text = buildseed.seed_path(REPO).read_text(encoding="utf-8")
    for rel in set(re.findall(r'"([^"]+)" [0-9a-f]{64}', text)):
        (tmp_path / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(REPO / rel, tmp_path / rel)
    seed = buildseed.seed_path(tmp_path)
    seed.parent.mkdir(parents=True, exist_ok=True)
    seed.write_text(text, encoding="utf-8")
    return seed


def _run_seed(seed, *defs):
    import subprocess

    import pytest
    r = subprocess.run(["cmake", *defs, "-P", str(seed)], capture_output=True, text=True,
                       cwd=seed.parent)
    out = r.stdout + r.stderr
    m = re.search(r"configure seed: (CMake|clang|toolset)[^;]*; running all checks", out)
    if m and not defs:
        pytest.skip(f"seed made for another toolchain: {m.group(0)}")
    return out


def test_seed_applies_in_this_checkout():
    _cmake_or_skip()
    out = _run_seed(buildseed.seed_path(REPO))
    assert re.search(r"configure seed: [1-9]\d* cached checks applied, 0 left to run", out), out


def test_seed_skips_other_build_types_and_compilers(tmp_path):
    _cmake_or_skip()
    seed = buildseed.seed_path(REPO)
    assert "build type Debug is not Release" in _run_seed(seed, "-DCMAKE_BUILD_TYPE=Debug")
    assert "C compiler cl is not clang" in _run_seed(seed, "-DCMAKE_C_COMPILER=cl")


def test_seed_runs_all_checks_when_top_level_changes(tmp_path):
    _cmake_or_skip()
    for gate in buildseed.GATE_FILES:
        seed = _seed_tree(tmp_path / gate)
        assert "cached checks applied, 0 left" in _run_seed(seed)
        with open(seed.parents[2] / gate, "a", encoding="utf-8") as f:
            f.write("\n")
        out = _run_seed(seed)
        assert "top-level CMakeLists.txt or CMakePresets.json changed" in out, out
        assert "cached checks applied" not in out


def test_seed_reruns_only_the_group_whose_check_source_changed(tmp_path):
    _cmake_or_skip()
    seed = _seed_tree(tmp_path)
    with open(tmp_path / "third-party/curl/CMake/CurlTests.c", "a", encoding="utf-8") as f:
        f.write("\n")
    out = _run_seed(seed)
    m = re.search(r"configure seed: (\d+) cached checks applied, (\d+) left to run", out)
    assert m and int(m.group(1)) > 0 and int(m.group(2)) > 0, out
    assert "re-run `python -m sandover.buildseed harvest`" in out
