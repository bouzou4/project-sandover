import json
import re
import zipfile
from pathlib import Path

import pytest

from sandover import bug_report
from sandover.rig import snapshot
from sandover.triage.store import Run

SANDOVER_GC = "(sandover 3\n  (ssao? #t)\n  (fur? #f)\n  (render-scale 1)\n  )\n"


def sample_report(**over) -> dict:
    rep = {
        "schema": "sandover-bug-report",
        "schema_version": 1,
        "id": "20261009-153000",
        "source": "key",
        "index": 1,
        "time_utc": "2026-10-09T19:30:00Z",
        "level": "village1",
        "continue": "village1-hut",
        "jak_pos": [-560712.0, 30924.5, 781004.1],
        "jak_quat": [0.0, 0.7, 0.0, 0.7],
        "cam_pos": [-548102.3, 51077.0, 760018.9],
        "cam_rows": [[-0.99, 0.0, -0.12], [0.001, 0.99, -0.011], [0.12, -0.011, -0.99]],
        "fov": 64.0,
        "hour": 12.43,
        "mood_weights": [1.0, 0, 0, 0, 0, 0, 0, 0],
        "toggles": [],
        "build_sha": "abc1234",
        "hdr": False,
        "status": "kept",
        "note": None,
        "tags": [],
        "annotations": [],
        "build": {"id": "20261009-120000-0123456789", "commit": "abc1234", "branch": "master",
                  "built": "2026-10-09T12:00:00", "compiled_revision": "abc1234"},
        "system": {"os": "Windows 11 24H2 (10.0.26100.1)", "cpu_threads": 16,
                   "gpu": {"vendor": "Example Vendor", "renderer": "Example GPU", "driver": "4.6.0 1.0"}},
        "display": {"mode": "borderless", "window": [2560, 1440], "refresh_hz": 144,
                    "game_resolution": [2560, 1440], "screenshot": [2560, 1440], "render_scale": 1.0,
                    "upscaler": "fsr1", "msaa": 4, "aa_mode": 0, "vsync": True,
                    "hdr": {"fp16_target": True, "output_requested": True, "format": "pq",
                            "presenting": False, "peak_nits": 800.0, "paper_white_nits": 200.0,
                            "black_nits": 0.0, "display_peak_nits": -1.0, "display_black_nits": -1.0,
                            "desktop_white_nits": -1.0}},
        "settings": {"sandover": {"version": 3, "values": {"ssao?": True, "fur?": False, "render-scale": 1},
                                  "text": SANDOVER_GC, "source": "live"},
                     "pc": {"version": 1, "values": {"fps": 60}}},
        "files": {"screenshot": "screenshot.png", "preview": "preview.png"},
        "log_tail": ["[12:00] [info] loaded <path>/village1.fr3"] * 3,
    }
    rep.update(over)
    return rep


def write_zip(path: Path, report: dict, png: bytes = b"\x89PNG fake", preview: bool = True) -> Path:
    top = f"sandover-bug-{report.get('id', 'x')}"
    with zipfile.ZipFile(path, "w") as z:
        z.writestr(f"{top}/report.json", json.dumps(report))
        z.writestr(f"{top}/screenshot.png", png)
        if preview:
            z.writestr(f"{top}/preview.png", b"\x89PNG small")
    return path


def test_schema_file_is_valid_json_and_sample_passes():
    sch = bug_report.schema()
    assert sch["properties"]["schema"]["const"] == "sandover-bug-report"
    assert bug_report.validate(sample_report(), sch) == []


def test_schema_is_a_superset_of_the_snapshot_sidecar():
    st = {"index": 1, "level": "a", "continue": "b", "jak_pos": [0, 0, 0], "cam_pos": [0, 0, 0],
          "cam_rows": [[1, 0, 0]] * 3, "fov": 64.0, "hour": 1.0, "mood_weights": [0] * 8}
    snap_keys = set(snapshot.sidecar(st, [], "sha"))
    assert snap_keys <= set(bug_report.schema()["properties"])


def test_validate_reports_missing_and_wrong_fields():
    rep = sample_report()
    del rep["cam_rows"]
    del rep["system"]["gpu"]
    rep["hour"] = 30
    rep["mood_weights"] = [0.0] * 7
    rep["source"] = "mouse"
    rep["display"]["hdr"]["presenting"] = "yes"
    probs = bug_report.validate(rep)
    assert "cam_rows: missing" in probs
    assert "system.gpu: missing" in probs
    assert any(p.startswith("hour:") and "above 24" in p for p in probs)
    assert any(p.startswith("mood_weights:") and "at least 8" in p for p in probs)
    assert any(p.startswith("source:") for p in probs)
    assert any(p.startswith("display.hdr.presenting: expected boolean") for p in probs)


def test_private_paths_are_flagged(tmp_path):
    rep = sample_report(log_tail=["loading C:/Users/someone/x.gc", "<config>/OpenGOAL/jak1 ok",
                                  "url http://example.com/a"])
    assert bug_report.private_paths(rep) == ["loading C:/Users/someone/x.gc"]
    probs = bug_report.load(write_zip(tmp_path / "b.zip", rep)).problems
    assert any(p.startswith("absolute path left in") for p in probs)


def test_import_zip_makes_a_triage_run(tmp_path):
    rep = sample_report()
    z = write_zip(tmp_path / "sandover-bug-20261009-153000.zip", rep, png=b"\x89PNG frame")
    run, b = bug_report.import_bundle(z, tmp_path / "runs")
    assert run == tmp_path / "runs" / "bug-20261009-153000"
    assert b.problems == []
    assert (run / "0001.png").read_bytes() == b"\x89PNG frame"
    assert (run / "bundle" / "preview.png").exists()
    assert (run / "bundle" / "sandover.gc").read_text(encoding="utf-8") == SANDOVER_GC
    side = json.loads((run / "0001.json").read_text(encoding="utf-8"))
    assert side["index"] == 1 and side["annotations"] == [] and side["status"] == "kept"
    assert side["level"] == "village1" and side["cam_rows"] == rep["cam_rows"]
    gc = (run / "bundle" / "sandover.gc").resolve().as_posix()
    assert side["toggles"] == ["(sandover-settings-reset! (-> *pc-settings* sandover))",
                               f'(sandover-settings-read (-> *pc-settings* sandover) "{gc}")',
                               "(sandover-settings-reapply)", "(sandover-settings-update)"]
    snaps = Run(run).snapshots()
    assert [s["index"] for s in snaps] == [1] and snaps[0]["annotations"] == []


def test_imported_sidecar_feeds_from_snapshot(tmp_path):
    pytest.importorskip("PIL")
    from PIL import Image

    rep = sample_report()
    folder = tmp_path / "20261009-153000"
    folder.mkdir()
    (folder / "report.json").write_text(json.dumps(rep), encoding="utf-8")
    Image.new("RGB", (64, 36)).save(folder / "screenshot.png")
    run, _ = bug_report.import_bundle(folder, tmp_path / "runs")
    shot, forms = snapshot.shot_from_snapshot(run / "0001.json")
    assert shot.continue_point == "village1-hut" and shot.resolution == (64, 36)
    assert shot.camera["pos"] == rep["cam_pos"] and shot.hour == pytest.approx(12.43)
    assert forms[1].startswith("(sandover-settings-read")


def test_quoted_list_values_are_unquoted(tmp_path):
    old = '(sandover 3\n  (ssao? #t)\n  (material-look "bold")\n  (upscaler "bilinear")\n  )\n'
    new = "(sandover 3\n  (ssao? #t)\n  (material-look bold)\n  (upscaler bilinear)\n  )\n"
    assert bug_report.unquote_settings(old) == new
    rep = sample_report()
    rep["settings"]["sandover"]["text"] = old
    run, _ = bug_report.import_bundle(write_zip(tmp_path / "b.zip", rep), tmp_path / "runs")
    assert '"' not in (run / "bundle" / "sandover.gc").read_text(encoding="utf-8")


def test_import_never_overwrites(tmp_path):
    z = write_zip(tmp_path / "b.zip", sample_report())
    first, _ = bug_report.import_bundle(z, tmp_path / "runs")
    second, _ = bug_report.import_bundle(z, tmp_path / "runs")
    assert first.name == "bug-20261009-153000" and second.name == "bug-20261009-153000-2"


def test_import_refuses_without_camera(tmp_path):
    rep = sample_report()
    del rep["cam_pos"]
    del rep["level"]
    with pytest.raises(ValueError, match="level, cam_pos"):
        bug_report.import_bundle(write_zip(tmp_path / "b.zip", rep), tmp_path / "runs")
    assert not (tmp_path / "runs").exists()


def test_import_without_settings_text_keeps_toggles(tmp_path):
    rep = sample_report(settings={"sandover": None, "pc": None})
    run, b = bug_report.import_bundle(write_zip(tmp_path / "b.zip", rep, preview=False), tmp_path / "runs")
    side = json.loads((run / "0001.json").read_text(encoding="utf-8"))
    assert side["toggles"] == [] and not (run / "bundle" / "sandover.gc").exists()


def test_cli_validate_and_import(tmp_path, capsys):
    z = write_zip(tmp_path / "b.zip", sample_report())
    assert bug_report.main(["validate", str(z)]) == 0
    assert "ok" in capsys.readouterr().out
    assert bug_report.main(["import", str(z), "--runs-dir", str(tmp_path / "runs")]) == 0
    out = capsys.readouterr().out
    assert "imported to" in out and "from-snapshot" in out
    bad = sample_report()
    del bad["build"]
    assert bug_report.main(["validate", str(write_zip(tmp_path / "c.zip", bad))]) == 1
    assert "build: missing" in capsys.readouterr().out


CPP = Path(__file__).resolve().parents[3] / "game" / "graphics" / "bug_report.cpp"
ENV = {"USERPROFILE": r"C:\Users\jdoe.CORP", "USERNAME": "jdoe", "COMPUTERNAME": "JDOE-PC",
       "USERDOMAIN": "CORP", "APPDATA": r"C:\Users\jdoe.CORP\AppData\Roaming",
       "TEMP": r"C:\Users\jdoe.CORP\AppData\Local\Temp"}
FOLDERS = [(r"D:\Games\Sandover", "<install>"), (r"C:\Users\jdoe.CORP\AppData\Roaming\Sandover", "<config>")]
# (text, what gk writes); every output must pass privacy_problems
SAMPLES = [
    (r"loading D:\My Games\Jak ISO\jak.iso", "loading <path>/jak.iso"),
    (r'"D:\My Games\Jak ISO\jak.iso" not found', '"<path>/jak.iso" not found'),
    (r"open 'E:\Some Folder\x y.txt' failed", "open '<path>/x y.txt' failed"),
    (r"cannot open D:\My Games\a.iso: error 2", "cannot open <path>/a.iso: error 2"),
    (r"D:\Jo's Games\Jak\x.iso", "<path>/x.iso"),
    (r"C:\Program Files (x86)\Steam\x.dll", "<path>/x.dll"),
    (r"\\fileserver\share\Jak ISO\a.iso", "<path>/a.iso"),
    ("C:/Users/jdoe.CORP/OneDrive - Contoso Ltd/Documents/a.txt", "<home>/OneDrive - <org>/Documents/a.txt"),
    (r"C:\Users\other user\Desktop\x.gc", "<path>/x.gc"),
    (r"D:\Games\Sandover\gk.exe", r"<install>\gk.exe"),
    (r"C:\Users\jdoe.CORP\AppData\Roaming\Sandover\settings", r"<config>\settings"),
    (r"C:\Users\JDOE.corp\Saved Games", r"<home>\Saved Games"),
    ("user jdoe on JDOE-PC in CORP, profile jdoe.CORP", "user <user> on <host> in <domain>, profile <user>"),
    ("/home/alice/.config/x and /Users/bob/y", "/home/<user>/.config/x and /Users/<user>/y"),
    ("OneDrive - Contoso Ltd", "OneDrive - <org>"),
    ("a\nE:\\Jak ISO\\b.iso\nc", "a\n<path>/b.iso\nc"),
    ("fps 60, ratio 16:9, url http://example.com/a, jdoes", "fps 60, ratio 16:9, url http://example.com/a, jdoes"),
]


def test_sanitize_patterns_are_the_cpp_ones():
    src = CPP.read_text(encoding="utf-8")
    found = re.findall(r'std::regex (k[A-Za-z]+)\(\s*R"re\((.*?)\)re"\)', src, re.S)
    assert dict(found) == {"kAbsPath": bug_report.ABS_PATH.pattern, "kUnixHome": bug_report.UNIX_HOME.pattern,
                           "kOrgFolder": bug_report.ORG_FOLDER.pattern}


@pytest.mark.parametrize("text,want", SAMPLES)
def test_sanitize_sample_set(text, want):
    got = bug_report.sanitize(text, bug_report.sanitize_rules(ENV, FOLDERS))
    assert got == want
    assert bug_report.privacy_problems(got, ENV) == []


def test_redirected_profile_with_onedrive_set():
    env = dict(ENV, OneDriveCommercial=r"C:\Users\jdoe.CORP\OneDrive - Contoso Ltd")
    got = bug_report.sanitize(r"C:\Users\jdoe.CORP\OneDrive - Contoso Ltd\Documents\a.txt",
                              bug_report.sanitize_rules(env))
    assert got == r"<onedrive>\Documents\a.txt"


def test_profile_outside_users_scrubs_its_parents():
    env = {"USERPROFILE": r"D:\Profiles\Contoso\jdoe", "USERNAME": "jdoe"}
    got = bug_report.sanitize("roaming share Contoso, D:/Profiles/Contoso/jdoe/x", bug_report.sanitize_rules(env))
    assert got == "roaming share <profile>, <home>/x"


def test_old_regex_leaked_what_the_new_one_removes():
    old = re.compile(r"""(^|[^A-Za-z0-9_])((?:[A-Za-z]:|\\\\[^\\/\s]+)[\\/][^\s"'<>|*?]*)""")
    text = r"loading D:\My Games\Jak ISO\jak.iso"
    assert "Games" in old.sub(lambda m: m.group(1) + "<path>/", text)
    assert "Games" not in bug_report.sanitize(text, [])


def test_rig_extern_matches_the_goal_function():
    from sandover import presets
    gc = (CPP.parents[2] / "goal_src" / "jak1" / "pc" / "features" / "bug-report.gc").read_text(encoding="utf-8")
    assert "(defun sandover-bug-report ()" in gc
    assert presets.BUG_REPORT_EXTERN == "(define-extern sandover-bug-report (function none))"
    assert presets.BUG_REPORT_FORMS[0] == presets.BUG_REPORT_EXTERN


def test_validate_fails_paths_users_orgs_and_local_names(tmp_path):
    rep = sample_report(log_tail=[r"loading D:\My Games\Jak ISO\x", r"<home>\OneDrive - Contoso\a",
                                  "<path>/Users/jdoe/x", "saved by JDOE-PC"])
    probs = bug_report.load(write_zip(tmp_path / "b.zip", rep), env={"COMPUTERNAME": "JDOE-PC"}).problems
    assert [p.split(":")[0] for p in probs] == ["absolute path left in", "organisation folder left in",
                                                "user folder left in", "local user or machine name left in"]
    assert bug_report.main(["validate", str(tmp_path / "b.zip")]) == 1
    clean = sample_report(log_tail=[r"<home>\OneDrive - <org>\a", "/home/<user>/x", "<path>/x.iso"])
    assert bug_report.load(write_zip(tmp_path / "c.zip", clean), env={"COMPUTERNAME": "JDOE-PC"}).problems == []




def test_rig_runs_gk_from_the_bin_root(tmp_path, monkeypatch):
    # a bin staged with its own GOAL root must not boot the primary's GAME.CGO (no bug-report code)
    from sandover.rig import capture

    monkeypatch.setenv("SANDOVER_DATA", str(tmp_path))
    bin_dir = tmp_path / "bin" / "b"
    bin_dir.mkdir(parents=True)
    assert capture.bin_root(bin_dir) is None
    (bin_dir / "build.json").write_text(json.dumps({"root": "worktrees/b-root"}), encoding="utf-8")
    assert capture.bin_root(bin_dir) is None
    (tmp_path / "worktrees" / "b-root").mkdir(parents=True)
    assert capture.bin_root(bin_dir) == tmp_path / "worktrees" / "b-root"
