"""sandover-package: what goes in the zip, and that nothing from the disc does."""

import json
import subprocess
import zipfile

from sandover import engine, package


def _fake(tmp_path, monkeypatch):
    repo, data, bins = tmp_path / "repo", tmp_path / "data", tmp_path / "build"
    for rel in ("goal_src/jak1/game.gp", "goal_src/goal-lib.gc", "decompiler/config/jak1/jak1_config.jsonc",
                "game/graphics/opengl_renderer/shaders/a.frag", "sandover/presets/pbr.toml",
                ".github/scripts/releases/error-code-metadata.json", "goal_src/jak2/x.gc",
                "goal_src/user/README.md", "LICENSE"):
        (repo / rel).parent.mkdir(parents=True, exist_ok=True)
        (repo / rel).write_text("x", encoding="utf-8")
    (repo / "goal_src/jak1/scratch.gc").write_text("untracked", encoding="utf-8")
    subprocess.run(["git", "init", "-q", str(repo)], check=True)
    subprocess.run(["git", "-C", str(repo), "add", "goal_src/jak1/game.gp", "goal_src/goal-lib.gc",
                    "decompiler", "game", "sandover", ".github", "goal_src/jak2", "goal_src/user"], check=True)
    fr3 = repo / "out/jak1/fr3"
    fr3.mkdir(parents=True)
    (fr3 / "beach.fr3").write_bytes(b"disc-derived")
    (fr3 / "beach.unbake").write_bytes(b"sidecar")
    (repo / "iso_data/jak1").mkdir(parents=True)
    (repo / "iso_data/jak1/GAME.CGO").write_bytes(b"disc")
    (data / "materials/pbr-maps/common").mkdir(parents=True)
    (data / "materials/materials.csv").write_text("combo_id\n", encoding="utf-8")
    (data / "materials/emissive-actors.csv").write_text("name\n", encoding="utf-8")
    (data / "materials/pbr-maps/common/a.normal.png").write_bytes(b"png")
    (data / "sky/jak3").mkdir(parents=True)
    (data / "sky/jak3/full-moon.png").write_bytes(b"png")
    (data / "lighting/canopy").mkdir(parents=True)
    (data / "lighting/beach.canopy").write_bytes(b"c")
    (data / "lighting/beach-sidecar.json").write_text("{}", encoding="utf-8")
    (data / "lighting/canopy/beach.canopy").write_bytes(b"v")
    staged = data / "bin" / "20261008-000000-abcdef0123"
    staged.mkdir(parents=True)
    for f in ("gk.exe", "goalc.exe", "fmt.dll", engine.DONE):
        (staged / f).write_bytes(b"x")
    (staged / engine.BUILD_INFO).write_text(json.dumps({"engine_sha": "abc1234"}), encoding="utf-8")
    bins.mkdir()
    (bins / "extractor.exe").write_bytes(b"x")
    (bins / "fmt.dll").write_bytes(b"x")
    monkeypatch.setattr(engine, "build_bin", lambda: bins)
    monkeypatch.setattr(package, "crt_dir", lambda: None)
    return repo, data, staged


def test_plan_ships_runtime_data_and_never_disc_files(tmp_path, monkeypatch):
    repo, data, staged = _fake(tmp_path, monkeypatch)
    arcs = {e.arc for e in package.plan(staged, repo, data).entries}
    assert {"gk.exe", "goalc.exe", "fmt.dll", "extractor.exe", "install.ps1", "Install.cmd", "LICENSE"} <= arcs
    assert "data/goal_src/jak1/game.gp" in arcs
    assert "data/goal_src/user/README.md" in arcs  # gk aborts at boot without goal_src/user
    assert "data/sandover/presets/pbr.toml" in arcs
    assert "data/launcher/error-code-metadata.json" in arcs
    assert "data/sandover/data/materials/pbr-maps/common/a.normal.png" in arcs
    assert "data/sandover/data/sky/jak3/full-moon.png" in arcs  # the sky bodies' sprites
    assert "data/sandover/data/lighting/beach.canopy" in arcs
    assert "data/sandover/data/lighting/canopy/beach.canopy" in arcs
    # gk's own fallback folder, so a launcher install (no install.ps1) still gets the lighting
    assert "data/sandover/data/lighting/beach.unbake" in arcs
    assert not any(a.startswith("data/sandover/fr3/") for a in arcs)
    assert not any(a.endswith((".fr3", ".CGO")) or "iso_data" in a for a in arcs)
    assert "data/goal_src/jak1/scratch.gc" not in arcs  # untracked
    assert "data/goal_src/jak2/x.gc" not in arcs
    assert "data/sandover/data/lighting/beach-sidecar.json" not in arcs


def test_plan_warns_when_sky_sprites_are_missing(tmp_path, monkeypatch):
    repo, data, staged = _fake(tmp_path, monkeypatch)
    warns = package.plan(staged, repo, data).warnings
    assert not any("full-moon.png missing" in w for w in warns)
    assert any("glow.png missing" in w and "sun disc" in w for w in warns)


def test_zip_layout(tmp_path, monkeypatch):
    repo, data, staged = _fake(tmp_path, monkeypatch)
    p = package.plan(staged, repo, data)
    out = package.write_zip(p, tmp_path / "out" / "s.zip", "sandover-abc", {"engine_sha": "abc"})
    names = zipfile.ZipFile(out).namelist()
    assert all(n.startswith("sandover-abc/") for n in names)
    assert "sandover-abc/build.json" in names and "sandover-abc/data/log/.keep" in names
    assert len(names) == len(p.entries) + 2


def test_skew_is_refused(tmp_path, monkeypatch):
    repo, data, staged = _fake(tmp_path, monkeypatch)
    subprocess.run(["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@t", "commit",
                    "-qm", "x"], check=True)
    assert "built from abc1234" in package.check_skew(staged, repo)
    head = subprocess.run(["git", "-C", str(repo), "rev-parse", "--short", "HEAD"], capture_output=True,
                          text=True).stdout.strip()
    (staged / engine.BUILD_INFO).write_text(json.dumps({"engine_sha": head}), encoding="utf-8")
    assert package.check_skew(staged, repo) is None


def test_extractor_must_share_the_staged_libraries(tmp_path, monkeypatch):
    repo, data, staged = _fake(tmp_path, monkeypatch)
    assert package.check_extractor(staged, repo) is None
    (engine.build_bin() / "fmt.dll").write_bytes(b"rebuilt")
    assert "fmt.dll" in package.check_extractor(staged, repo)


def test_dirty_shipped_source_is_reported(tmp_path, monkeypatch):
    repo, data, staged = _fake(tmp_path, monkeypatch)
    subprocess.run(["git", "-C", str(repo), "-c", "user.name=t", "-c", "user.email=t@t", "commit",
                    "-qm", "x"], check=True)
    assert package.dirty_source(repo) == []
    (repo / "goal_src/jak1/game.gp").write_text("edited", encoding="utf-8")
    assert package.dirty_source(repo) == ["goal_src/jak1/game.gp"]


def test_release_zip_is_flat(tmp_path, monkeypatch):
    # the launcher extracts the asset as is and runs extractor.exe from the mod folder
    repo, data, staged = _fake(tmp_path, monkeypatch)
    p = package.plan(staged, repo, data)
    out = package.write_zip(p, tmp_path / "out" / "r.zip", "", {"version": "0.9.0"})
    names = zipfile.ZipFile(out).namelist()
    assert {"gk.exe", "extractor.exe", "build.json", "data/log/.keep"} <= set(names)
    assert not any(n.startswith("/") for n in names)


def test_build_tree_ships_its_own_extractor_and_notices(tmp_path, monkeypatch):
    repo, data, staged = _fake(tmp_path, monkeypatch)
    tree = tmp_path / "static" / "bin"
    tree.mkdir(parents=True)
    for f in ("gk.exe", "goalc.exe", "extractor.exe", "goalc-test.exe", "OpenAL32.dll",
              "OpenAL-Soft-COPYING.txt", "sandover-alsoft-objects.ini"):
        (tree / f).write_bytes(b"static")
    arcs = {e.arc: e.src for e in package.plan(tree, repo, data).entries}
    assert arcs["extractor.exe"] == tree / "extractor.exe"
    assert {"OpenAL32.dll", "OpenAL-Soft-COPYING.txt", "sandover-alsoft-objects.ini"} <= set(arcs)
    assert "goalc-test.exe" not in arcs and "fmt.dll" not in arcs
    assert package.check_extractor(tree, repo) is None


def test_dlss_runtime_never_ships(tmp_path, monkeypatch):
    # the player's own file, like the disc image: neither the DLL nor its licence goes in the zip
    repo, data, staged = _fake(tmp_path, monkeypatch)
    for d in (staged, tmp_path / "static" / "bin"):
        d.mkdir(parents=True, exist_ok=True)
        for f in ("gk.exe", "goalc.exe", "extractor.exe", "nvngx_dlss.dll", "NVIDIA-DLSS-LICENSE.txt"):
            (d / f).write_bytes(b"x")
        arcs = {e.arc for e in package.plan(d, repo, data).entries}
        assert "nvngx_dlss.dll" not in arcs and "NVIDIA-DLSS-LICENSE.txt" not in arcs


def test_fsr4_runtime_ships_with_its_notice(tmp_path, monkeypatch):
    repo, data, staged = _fake(tmp_path, monkeypatch)
    lic = repo / "third-party" / "amd-fsr4" / "LICENSE.txt"
    lic.parent.mkdir(parents=True, exist_ok=True)
    lic.write_text("MIT")
    staged.mkdir(parents=True, exist_ok=True)
    for f in ("gk.exe", "goalc.exe", "extractor.exe", *package.FSR4_DLLS):
        (staged / f).write_bytes(b"x")
    arcs = {e.arc: e.src for e in package.plan(staged, repo, data).entries}
    assert package.FSR4_DLLS <= set(arcs)
    assert arcs[package.FSR4_LICENSE] == lic


def test_crt_ships_only_for_a_shared_runtime(tmp_path, monkeypatch):
    repo, data, staged = _fake(tmp_path, monkeypatch)
    crt = tmp_path / "crt"
    crt.mkdir()
    for name in package.CRT:
        (crt / name).write_bytes(b"crt")
    monkeypatch.setattr(package, "crt_dir", lambda: crt)
    arcs = {e.arc for e in package.plan(staged, repo, data).entries}
    assert not arcs & set(package.CRT)
    (staged / "gk.exe").write_bytes(b"MZ...VCRUNTIME140.dll...")
    arcs = {e.arc for e in package.plan(staged, repo, data).entries}
    assert set(package.CRT) <= arcs


def test_release_version_is_checked():
    assert package.release_version("0.9.0") == "0.9.0"
    assert package.release_version("v0.9.0") == "0.9.0"
    for bad in ("0.9", "beta", "v0.9.0/x"):
        try:
            package.release_version(bad)
        except SystemExit:
            continue
        raise AssertionError(bad)
