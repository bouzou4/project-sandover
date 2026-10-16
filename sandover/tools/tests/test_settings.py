import json
import re
from pathlib import Path

import pytest

from sandover import settings

REPO = Path(__file__).resolve().parents[3]
PCKERNEL = REPO / "goal_src" / "jak1" / "pc" / "pckernel.gc"
IMPL = REPO / "goal_src" / "jak1" / "pc" / "pckernel-impl.gc"


def test_round_trip_defaults():
    version, values, problems = settings.parse(settings.dumps())
    assert version == settings.VERSION and values == settings.DEFAULTS and problems == []


def test_unknown_keys_are_skipped_like_the_game():
    text = """(sandover 4
      (ssao? #f)
      (future-thing (nested (deeper 1) "a ) in a string") 2)
      (tonemap filmic)
      )"""
    version, values, problems = settings.parse(text)
    assert version == 4
    assert values["ssao?"] is False and values["tonemap"] == "filmic"
    assert any("future-thing" in p for p in problems) and any("newer" in p for p in problems)


def test_missing_keys_keep_defaults():
    _, values, _ = settings.parse("(sandover 1 (bloom? #f))")
    assert values["bloom?"] is False and values["material-look"] == "bold"


def test_paper_white_clamps_like_the_game():
    for raw, want in ((150, 150), (174, 174), (60, 80), (1000, 400), (0, 80)):
        _, values, _ = settings.parse(f"(sandover 2 (paper-white-nits {raw}))")
        assert values["paper-white-nits"] == want, raw


def test_calibration_keys_follow_the_display_when_negative():
    _, values, problems = settings.parse("(sandover 2 (hdr-peak-nits -1) (hdr-black-nits -1.000))")
    assert values["hdr-peak-nits"] == -1 and values["hdr-black-nits"] == -1.0 and problems == []
    _, values, problems = settings.parse("(sandover 2 (hdr-peak-nits 20000) (hdr-black-nits 0.05))")
    assert values["hdr-peak-nits"] == 10000 and values["hdr-black-nits"] == 0.05
    assert len(problems) == 1
    assert settings.parse("(sandover 2 (hdr-peak-nits 100))")[1]["hdr-peak-nits"] == 400
    assert "(hdr-black-nits -1.000)" in settings.dumps() and "(hdr-peak-nits -1)" in settings.dumps()


def test_calibration_readers_clamp_like_the_game():
    block = _goal_block(PCKERNEL.read_text(encoding="utf-8"), "(defun sandover-settings-read-key")
    assert "(fmax 80.0 (fmin 400.0 (file-stream-read-float file)))" in block
    assert "(fmax 400.0 (fmin 10000.0 v))" in block
    assert "(fmin 1.0 v)" in block


def test_bad_values_reported():
    _, values, problems = settings.parse("(sandover 1 (tonemap aces) (fur? yes))")
    assert values["tonemap"] == "toon" and values["fur?"] is True
    assert len(problems) == 2


@pytest.mark.parametrize("text", ["", "(settings #x1 (fps 60))", "(sandover 1 (ssao? #t)", "(sandover)"])
def test_rejected(text):
    with pytest.raises(ValueError):
        settings.parse(text)


def test_seed_off_is_all_off(tmp_path):
    path = settings.seed_off(tmp_path)
    assert path == tmp_path / "OpenGOAL" / "jak1" / "settings" / "sandover.gc"
    _, values, problems = settings.parse(path.read_text(encoding="utf-8"))
    assert problems == [] and values["hdr-output"] == "off" and values["tonemap"] == "clamp"
    assert not any(v is True for v in values.values())


def test_check_cli(tmp_path, capsys):
    good = settings.write(tmp_path / "a.gc")
    assert settings.main(["check", str(good)]) == 0
    bad = tmp_path / "b.gc"
    bad.write_text("(sandover 1 (nope 1))", encoding="utf-8")
    assert settings.main(["check", str(bad)]) == 1
    assert "nope" in capsys.readouterr().out


def _goal_block(text: str, start: str) -> str:
    i = text.index(start)
    j = text.find("\n(def", i + 1)
    return text[i:j if j > 0 else None]


def test_keys_match_the_game_reader_and_writer():
    src = PCKERNEL.read_text(encoding="utf-8")
    read = re.findall(r'\(\("([^"]+)"\)', _goal_block(src, "(defun sandover-settings-read-key"))
    written = re.findall(r'format file "  \(([^ ]+) ', _goal_block(src, "(defun sandover-settings-write"))
    assert read == list(settings.KEYS) and written == list(settings.KEYS)


def test_list_values_match_the_game_reader():
    block = _goal_block(PCKERNEL.read_text(encoding="utf-8"), "(defun sandover-settings-read-key")
    for key in ("material-look", "tonemap", "hdr-output", "antialias", "upscaler", "ssao-quality",
                "shadow-quality"):
        seg = block.split(f'(("{key}")', 1)[1].split('(("', 1)[0]
        names = re.findall(r"\(\('([a-z0-9-]+)\) \(set! \(-> obj [a-z-]+\) (\d)\)", seg)
        assert [n for n, _ in names] == list(settings.KEYS[key]), key
        assert [int(i) for _, i in names] == list(range(len(settings.KEYS[key]))), key


def test_defaults_match_the_game():
    block = _goal_block(IMPL.read_text(encoding="utf-8"), "(defun sandover-settings-reset!")
    got = dict(re.findall(r"\(set! \(-> obj ([a-z?-]+)\) ([^)]+)\)", block))
    field = {"materials?": "pbr?", "neighbours?": "vista?", "fp16-target?": "hdr-target?",
             "material-look": "pbr-look", "paper-white-nits": "paper-white"}
    field |= {"hdr-peak-nits": "hdr-peak", "hdr-black-nits": "hdr-black"}
    for key, want in settings.DEFAULTS.items():
        raw = got[field.get(key, key)]
        kind = settings.KEYS[key]
        if kind is bool:
            assert raw == ("#t" if want else "#f"), key
        elif isinstance(kind, settings.Range):
            assert float(raw) == float(want), key
        else:
            assert kind[int(raw)] == want, key


def test_version_matches_the_game():
    m = re.search(r"\(defconstant SANDOVER_SETTINGS_VERSION (\d+)\)", IMPL.read_text(encoding="utf-8"))
    assert int(m.group(1)) == settings.VERSION


def test_hdr10_is_the_default_and_v1_scrgb_migrates():
    assert settings.DEFAULTS["hdr-output"] == "pq"
    assert settings.parse("(sandover 1 (hdr-output scrgb))")[1]["hdr-output"] == "pq"
    assert settings.parse("(sandover 1 (hdr-output off))")[1]["hdr-output"] == "off"
    assert settings.parse("(sandover 2 (hdr-output scrgb))")[1]["hdr-output"] == "scrgb"
    block = _goal_block(PCKERNEL.read_text(encoding="utf-8"), "(defun sandover-settings-read ")
    assert "(< version 2) (zero? (-> obj hdr-output))" in block


def test_menu_strings_in_all_three_places():
    text_h = (REPO / "goal_src" / "jak1" / "engine" / "ui" / "text-h.gc").read_text(encoding="utf-8")
    mirror = (REPO / "decompiler" / "config" / "jak1" / "all-types.gc").read_text(encoding="utf-8")
    en = json.loads((REPO / "game" / "assets" / "jak1" / "text" / "game_custom_text_en-US.json").read_text(encoding="utf-8"))
    ids = re.findall(r"\((sandover-[a-z0-9-]+) #x([0-9a-f]+)\)", text_h)
    assert len(ids) >= 14
    for name, hexid in ids:
        assert f"({name} #x{hexid})" in mirror, name
        assert en.get(hexid), name
    used = set(re.findall(r"text-id (sandover-[a-z0-9-]+)", (REPO / "goal_src" / "jak1" / "pc" / "progress-pc.gc").read_text(encoding="utf-8")))
    used |= set(re.findall(r"\b(sandover-(?:look|tonemap|hdr|aa|scale|up|quality)-[a-z0-9-]+)\b", (REPO / "goal_src" / "jak1" / "pc" / "progress-pc.gc").read_text(encoding="utf-8")))
    assert used <= {n for n, _ in ids}


def test_antialias_defaults_to_msaa_and_round_trips():
    assert settings.DEFAULTS["antialias"] == "msaa" and settings.ALL_OFF["antialias"] == "msaa"
    for v in settings.KEYS["antialias"]:
        assert settings.parse(settings.dumps({"antialias": v}))[1]["antialias"] == v
    src = PCKERNEL.read_text(encoding="utf-8")
    write = _goal_block(src, "(defun sandover-settings-write")
    assert "((1) 'smaa) ((2) 'msaa2-smaa) ((3) 'off) (else 'msaa)" in write
    # the reset restores MSAA; the boot state is MSAA, so an untouched setting pushes nothing
    reset = _goal_block(IMPL.read_text(encoding="utf-8"), "(defun sandover-settings-reset!")
    assert "(set! (-> obj antialias) 0)" in reset
    assert "(set! (-> *sandover-applied* antialias) 0)" in _goal_block(src, "(defun sandover-settings-reapply")
    assert "(pc-set-smaa 0 (* 1000000 (-> want antialias)))" in _goal_block(src, "(defun sandover-settings-update")


def test_render_scale_and_upscaler():
    assert settings.DEFAULTS["render-scale"] == 100 and settings.DEFAULTS["upscaler"] == "fsr"
    assert settings.ALL_OFF["render-scale"] == 100
    for v in settings.KEYS["render-scale"]:
        assert settings.parse(settings.dumps({"render-scale": v}))[1]["render-scale"] == v
    _, values, problems = settings.parse("(sandover 2 (render-scale 80))")
    assert values["render-scale"] == 100 and problems  # the game keeps the value on no match
    src = PCKERNEL.read_text(encoding="utf-8")
    read = _goal_block(src, "(defun sandover-settings-read-key")
    seg = read.split('(("render-scale")', 1)[1].split('(("', 1)[0]
    pairs = re.findall(r"\(\((\d+)\) \(set! \(-> obj render-scale\) (\d)\)\)", seg)
    assert [int(a) for a, _ in pairs] == list(settings.KEYS["render-scale"])
    assert [int(b) for _, b in pairs] == [0, 1, 2, 3]
    write = _goal_block(src, "(defun sandover-settings-write")
    assert "((1) 77) ((2) 67) ((3) 50) (else 100)" in write
    impl = IMPL.read_text(encoding="utf-8")
    reset = _goal_block(impl, "(defun sandover-settings-reset!")
    assert "(set! (-> obj render-scale) 0)" in reset and "(set! (-> obj upscaler) 1)" in reset
    reapply = _goal_block(src, "(defun sandover-settings-reapply")
    assert "(set! (-> *sandover-applied* render-scale) 0)" in reapply
    assert "(set! (-> *sandover-applied* upscaler) 1)" in reapply
    assert "((1) 769231) ((2) 666667) ((3) 500000) (else 1000000)" in impl


def test_tier_rows_default_to_the_play_look():
    assert settings.DEFAULTS["ssao-quality"] == "high" and settings.DEFAULTS["shadow-quality"] == "high"
    assert settings.ALL_OFF["ssao-quality"] == "high" and settings.ALL_OFF["shadow-quality"] == "high"
    for key in ("ssao-quality", "shadow-quality"):
        for v in settings.KEYS[key]:
            assert settings.parse(settings.dumps({key: v}))[1][key] == v
    # high is the ssao preset the play look always used (deep, index 4); medium and low follow it
    header = (REPO / "game" / "graphics" / "opengl_renderer" / "SsaoPresets.inc").read_text(encoding="utf-8")
    names = re.findall(r'^\{"([a-z-]+)"', header, flags=re.M)
    assert names[4:7] == ["deep", "deep-medium", "deep-low"]
    src = PCKERNEL.read_text(encoding="utf-8")
    assert "(pc-set-ssao-preset (- 6 (-> want ssao-quality)))" in _goal_block(src, "(defun sandover-settings-update")
    gfx = (REPO / "game" / "graphics" / "gfx.h").read_text(encoding="utf-8")
    assert "int shadow_quality = 2;" in gfx and "QUALITY = 40," in gfx


def test_reapply_resets_the_new_sub_options():
    """Every field added with the tier rows is forgotten by sandover-settings-reapply at its boot
    state, and shadow option 40 (map size and taps) goes back to the engine default."""
    reapply = _goal_block(PCKERNEL.read_text(encoding="utf-8"), "(defun sandover-settings-reapply")
    for field, boot in (("ssao-quality", "2"), ("shadow-quality", "2"), ("smaa?", "#f"), ("sun?", "#f")):
        assert f"(set! (-> *sandover-applied* {field}) {boot})" in reapply, field
    assert "(pc-set-shadow-option 40 2000000)" in reapply
    km = (REPO / "game" / "kernel" / "common" / "kmachine.cpp").read_text(encoding="utf-8")
    assert "case ShadowOption::QUALITY:" in km


def test_smaa_and_sun_rows():
    assert settings.DEFAULTS["smaa?"] is True and settings.DEFAULTS["sun?"] is True
    # with the stock MSAA default at 2x, the shipped mode is msaa2-smaa
    pch = (REPO / "goal_src" / "jak1" / "pc" / "pckernel-h.gc").read_text(encoding="utf-8")
    assert re.search(r"^\(defconstant PC_DEFAULT_MSAA 2\)", pch, flags=re.M)
    assert settings.ALL_OFF["smaa?"] is False and settings.ALL_OFF["sun?"] is False
    # before version 3 the SMAA choice lived in the antialias list
    assert settings.parse("(sandover 2 (antialias smaa))")[1]["smaa?"] is True
    assert settings.parse("(sandover 2 (antialias msaa2-smaa))")[1]["smaa?"] is True
    assert settings.parse("(sandover 2 (antialias off))")[1]["smaa?"] is False
    assert settings.parse("(sandover 3 (antialias smaa) (smaa? #f))")[1]["smaa?"] is False
    # an older all-off file (an older rig's seed) keeps the sun off
    assert settings.parse("(sandover 2 (neighbours? #f))")[1]["sun?"] is False
    assert settings.parse("(sandover 2 (neighbours? #t))")[1]["sun?"] is True
    block = _goal_block(PCKERNEL.read_text(encoding="utf-8"), "(defun sandover-settings-read ")
    assert "(set! (-> obj smaa?) (or (= (-> obj antialias) 1) (= (-> obj antialias) 2)))" in block


def test_menu_rows():
    menu = (REPO / "goal_src" / "jak1" / "pc" / "progress-pc.gc").read_text(encoding="utf-8")
    page = menu[menu.index("(define *sandover-options*"):menu.index("(define *sandover-calibration-options*")]
    rows = re.findall(r":name \(text-id ([a-z0-9-]+)\)", page)
    assert rows == ["sandover-ssao", "sandover-ssao-quality", "sandover-tile-lights", "sandover-bloom",
                    "sandover-shadows", "sandover-shadow-quality", "sandover-materials", "sandover-fur",
                    "sandover-water", "sandover-glint", "sandover-neighbours", "sandover-sun", "sandover-fp16",
                    "sandover-hdr-output", "sandover-hdr-calibration", "sandover-smaa",
                    "sandover-render-scale", "sandover-upscaler", "sandover-bug-report", "back"]


def test_older_file_takes_the_picked_looks():
    """The material look and tonemap rows are gone: a version 2 file's alternate looks migrate to
    the picks (bold, toon); a version 3 file or the REPL can still set clamp or filmic."""
    _, values, _ = settings.parse("(sandover 2 (material-look cinematic) (tonemap filmic))")
    assert values["material-look"] == "bold" and values["tonemap"] == "toon"
    _, values, _ = settings.parse("(sandover 3 (material-look cinematic) (tonemap filmic))")
    assert values["material-look"] == "cinematic" and values["tonemap"] == "filmic"
    block = _goal_block(PCKERNEL.read_text(encoding="utf-8"), "(defun sandover-settings-read ")
    mig = block[block.index("(when (< version 3)"):]
    assert "(set! (-> obj pbr-look) 1)" in mig and "(set! (-> obj tonemap) 2)" in mig


def test_glint_row():
    """One on/off row for the water sun glint: on in the play look, off in the boot state, and
    forgotten by sandover-settings-reapply so the next update sends it."""
    assert settings.DEFAULTS["glint?"] is True and settings.ALL_OFF["glint?"] is False
    assert settings.parse(settings.dumps({"glint?": False}))[1]["glint?"] is False
    reapply = _goal_block(PCKERNEL.read_text(encoding="utf-8"), "(defun sandover-settings-reapply")
    assert "(set! (-> *sandover-applied* glint?) #f)" in reapply


def _list_keys():
    """{key: (reader, read names)} for every list-valued key of sandover-settings-read-key, and
    {key: (write line, written names)} for sandover-settings-write."""
    src = PCKERNEL.read_text(encoding="utf-8")
    read = _goal_block(src, "(defun sandover-settings-read-key")
    readers = {}
    for m in re.finditer(r'\(\("([a-z0-9?-]+)"\)\s*\(case \(([a-z-]+) file\)(.*?)(?=\(\("|\Z)', read, re.S):
        names = set(re.findall(r"\(\('([a-z0-9-]+)\)", m.group(3)))
        if names:
            readers[m.group(1)] = (m.group(2), names)
    write = _goal_block(src, "(defun sandover-settings-write")
    writers = {}
    for m in re.finditer(r'\(format file "  \(([a-z0-9?-]+) ~A\)~%" (\(case .*)', write):
        writers[m.group(1)] = (m.group(2), set(re.findall(r"""['"]([a-z0-9-]+)""", m.group(2))))
    return readers, writers


def test_list_keys_are_written_bare_and_read_through_read_name():
    readers, writers = _list_keys()
    assert set(readers) == {"material-look", "tonemap", "hdr-output", "antialias", "upscaler",
                            "ssao-quality", "shadow-quality"}
    assert set(writers) == set(readers)
    for key, (reader, names) in readers.items():
        assert reader == "sandover-read-name", key
        line, written = writers[key]
        assert '"' not in line, f"{key} is written quoted: {line}"
        assert written == names, key
