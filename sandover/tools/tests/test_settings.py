import json
import re
from pathlib import Path

import pytest

from sandover import settings, settings_table
from sandover.bridge_sources import bridge_text

REPO = Path(__file__).resolve().parents[3]
PCKERNEL = REPO / "goal_src" / "jak1" / "pc" / "pckernel.gc"
IMPL = REPO / "goal_src" / "jak1" / "pc" / "pckernel-impl.gc"
MENU = REPO / "goal_src" / "jak1" / "pc" / "progress-pc.gc"
CONSTS = settings_table.constants(IMPL.read_text(encoding="utf-8"))
BOOL, ENUM, INTMAP, FLOAT, WORD, BYTES, INT = (CONSTS[f"SANDOVER_FIELD_{k}"] for k in (
    "BOOL", "ENUM", "INTMAP", "FLOAT", "WORD", "BYTES", "INT"))
NEGATIVE, DECIMALS, NO_BOOT = CONSTS["SANDOVER_FLAG_NEGATIVE"], CONSTS["SANDOVER_FLAG_DECIMALS"], CONSTS["SANDOVER_NO_BOOT"]


def _rows() -> dict[str, dict]:
    """The rows of *sandover-fields* by file key."""
    return {r["key"]: r for r in settings_table.fields()}


def _flat(text: str) -> str:
    return " ".join(text.split())


def _page_row(menu: str, name: str) -> str:
    """The text of a row of the Sandover page (a lambda row, not its *sandover-menu* entry)."""
    page = menu[menu.index("(define *sandover-options*"):menu.index("(define *sandover-calibration-options*")]
    row = page[page.index(f":name (text-id {name})"):]
    return row[:row.index("(new 'static")] if "(new 'static" in row else row


def test_round_trip_defaults():
    version, values, problems = settings.parse(settings.dumps())
    assert version == settings.VERSION and values == settings.DEFAULTS and problems == []


def test_unknown_keys_are_skipped_like_the_game():
    text = """(sandover 5
      (ssao? #f)
      (future-thing (nested (deeper 1) "a ) in a string") 2)
      (tonemap filmic)
      )"""
    version, values, problems = settings.parse(text)
    assert version == 5
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
    rows = _rows()
    pw, peak, black = rows["paper-white-nits"], rows["hdr-peak-nits"], rows["hdr-black-nits"]
    assert all(r["kind"] == FLOAT for r in (pw, peak, black))
    assert (pw["lo"], pw["hi"], pw["flags"]) == (80.0, 400.0, 0)
    assert (peak["lo"], peak["hi"], peak["flags"]) == (400.0, 10000.0, NEGATIVE)
    # the black level only has an upper clamp: its lower bound is below every value that reaches it
    assert black["lo"] < 0 and black["hi"] == 1.0 and black["flags"] == NEGATIVE | DECIMALS
    block = _flat(_goal_block(PCKERNEL.read_text(encoding="utf-8"), "(defun sandover-field-read"))
    assert ("(if (and (logtest? (-> f flags) SANDOVER_FLAG_NEGATIVE) (< v 0.0)) -1.0 "
            "(fmax (-> f lo) (fmin (-> f hi) v)))") in block
    assert "(file-stream-read-float file)" in block


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
    assert values["vanilla-rendering?"] is True
    assert not any(v is True for k, v in values.items() if k != "vanilla-rendering?")


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
    # one table drives the reader and the writer; the VR profile's file adds its keys at the end
    assert [r["key"] for r in settings_table.fields()] == list(settings.KEYS) + list(settings.VR_KEYS)
    assert "(sandover-field-find *pc-temp-string*)" in _goal_block(src, "(defun sandover-settings-read-key")
    assert "(dotimes (i (-> *sandover-fields* length))" in _goal_block(src, "(defun sandover-settings-write")
    assert [r["kind"] for r in settings_table.fields()][-1] == INT  # the VR key is written last


def test_list_values_match_the_game_reader():
    rows = _rows()
    for key in ("material-look", "tonemap", "hdr-output", "upscaler", "ssao-quality",
                "shadow-quality", "motion-blur", "camera-blur"):
        assert rows[key]["kind"] == ENUM, key
        assert rows[key]["names"] == list(settings.KEYS[key]), key
    # a value that migrates from an older file: the index shifts down for the version 3 tiers
    block = _flat(_goal_block(PCKERNEL.read_text(encoding="utf-8"), "(defun sandover-field-read"))
    assert ('(if (and (< *sandover-settings-file-version* 4) (string= (-> f key) "ssao-quality") '
            '(or (= i 1) (= i 2))) (+ i -1) i)') in block


def test_defaults_match_the_game():
    rows = _rows()
    for key, want in settings.DEFAULTS.items():
        kind, row = settings.KEYS[key], rows[key]
        if kind is str:
            # a name field: empty (its first byte cleared) by default
            assert want == "" and row["kind"] == BYTES, key
            continue
        raw = row["default"]
        if kind is bool:
            assert row["kind"] == BOOL and raw == (1 if want else 0), key
        elif isinstance(kind, settings.Range):
            assert row["kind"] == FLOAT and float(raw) == float(want), key
        else:
            assert kind[int(raw)] == want, key
    reset = _flat(_goal_block(IMPL.read_text(encoding="utf-8"), "(defun sandover-settings-reset!"))
    assert "(sandover-field-set! obj (-> *sandover-fields* i) (-> *sandover-fields* i default))" in reset


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
    used |= set(re.findall(r"(?<!\*)\b(sandover-(?:look|tonemap|hdr|aa|scale|up|quality)-[a-z0-9-]+)\b", (REPO / "goal_src" / "jak1" / "pc" / "progress-pc.gc").read_text(encoding="utf-8")))
    assert used <= {n for n, _ in ids}


def test_vanilla_rendering_switch():
    """Off in the play look, on in the boot state (rig and parity slots); vanilla rendering is the
    boot state, so it pushes nothing."""
    assert settings.DEFAULTS["vanilla-rendering?"] is False and settings.ALL_OFF["vanilla-rendering?"] is True
    assert settings.parse(settings.dumps({"vanilla-rendering?": True}))[1]["vanilla-rendering?"] is True
    src = PCKERNEL.read_text(encoding="utf-8")
    row = _rows()["vanilla-rendering?"]
    assert row["default"] == 0 and row["boot"] == 1
    assert "(sandover-boot-state! *sandover-applied*)" in _goal_block(src, "(defun sandover-settings-reapply")
    effective = _goal_block(src, "(defun sandover-effective!")
    assert "((sandover-vanilla? src) (sandover-boot-state! dst))" in effective
    assert "(zero? (pc-temporal-aa-state))" in _goal_block(src, "(defun sandover-vanilla?")
    # an older file's antialias and smaa? keys are skipped
    assert "antialias" not in settings.KEYS and "smaa?" not in settings.KEYS


def test_dlss_upscaler_round_trips_and_maps_to_engine_mode_2():
    assert settings.KEYS["upscaler"] == ("fsr3", "dlss", "fsr4")
    for v in settings.KEYS["upscaler"]:
        assert settings.parse(settings.dumps({"upscaler": v}))[1]["upscaler"] == v
    src = PCKERNEL.read_text(encoding="utf-8")
    row = _rows()["upscaler"]
    # the written name of an index outside the list is fsr3; dlss is index 1
    assert row["names"][row["fallback"]] == "fsr3" and row["names"].index("dlss") == 1
    # engine mode = upscaler + 1: 1 FSR 3.1, 2 DLSS (gfx.h AaMode)
    assert "(pc-set-aa 0 (* 1000000 (+ 1 (-> want upscaler))))" in src
    menu = MENU.read_text(encoding="utf-8")
    # offered only where the engine reports it available (or a file already names it)
    assert "(nonzero? (pc-get-aa-available 1))" in menu
    assert "(sandover-up-fsr3 sandover-up-dlss)" in menu
    # the DLSS mode names per render scale entry (100, 77, 67, 59, 50, 33)
    block = menu.split("(define *sandover-render-quality-dlss*", 1)[1].split("(define", 1)[0]
    assert re.findall(r"text-id sandover-uq-([a-z]+)", block) == [
        "dlaa", "quality", "quality", "balanced", "performance", "ultra"]


def test_fsr4_upscaler_round_trips_and_maps_to_engine_mode_3():
    assert settings.parse(settings.dumps({"upscaler": "fsr4"}))[1]["upscaler"] == "fsr4"
    assert _rows()["upscaler"]["names"].index("fsr4") == 2
    menu = MENU.read_text(encoding="utf-8")
    # the value is the list index: FSR 4 sits after DLSS, which is stepped over where unavailable
    assert "(sandover-up-fsr3 sandover-up-dlss sandover-up-fsr4)" in menu
    assert "(nonzero? (pc-get-aa-available 2))" in menu
    skip = _goal_block(menu, "(defun sandover-list-skip?")
    assert "(zero? (pc-get-aa-available 1))" in skip and "(zero? (pc-get-aa-available 2))" in skip
    # gfx.h AaMode: engine mode = upscaler + 1
    gfx = (REPO / "game" / "graphics" / "gfx.h").read_text(encoding="utf-8")
    assert "AA_FSR4 = 3," in gfx


def test_render_scale_and_upscaler():
    assert settings.DEFAULTS["render-scale"] == 100 and settings.DEFAULTS["upscaler"] == "fsr3"
    assert settings.ALL_OFF["render-scale"] == 100
    for v in settings.KEYS["render-scale"]:
        assert settings.parse(settings.dumps({"render-scale": v}))[1]["render-scale"] == v
    _, values, problems = settings.parse("(sandover 2 (render-scale 80))")
    assert values["render-scale"] == 100 and problems  # the game keeps the value on no match
    rows = _rows()
    row = rows["render-scale"]
    # the file holds the percent; the field is the index (0..5), 100 for an index outside the list
    assert row["kind"] == INTMAP and row["ints"] == list(settings.KEYS["render-scale"])
    assert row["ints"][row["fallback"]] == 100
    impl = IMPL.read_text(encoding="utf-8")
    assert (row["default"], rows["upscaler"]["default"]) == (0, 0)
    assert (row["boot"], rows["upscaler"]["boot"]) == (0, -1)
    # an older file's spatial upscaler (bilinear, fsr) keeps the default
    _, values, _ = settings.parse("(sandover 3 (upscaler bilinear))")
    assert values["upscaler"] == "fsr3"
    assert "((1) 769231) ((2) 666667) ((3) 588235) ((4) 500000) ((5) 333333) (else 1000000)" in impl


def test_tier_rows_default_to_the_play_look():
    assert settings.DEFAULTS["ssao-quality"] == "medium" and settings.DEFAULTS["shadow-quality"] == "high"
    assert settings.ALL_OFF["ssao-quality"] == "medium" and settings.ALL_OFF["shadow-quality"] == "high"
    for key in ("ssao-quality", "shadow-quality"):
        for v in settings.KEYS[key]:
            assert settings.parse(settings.dumps({key: v}))[1][key] == v
    # the tier is the preset index: low, medium (deep, the play look), high, ultra
    header = (REPO / "game" / "graphics" / "opengl_renderer" / "SsaoPresets.inc").read_text(encoding="utf-8")
    names = re.findall(r'^\{"([a-z-]+)"', header, flags=re.M)
    assert names == ["deep-low", "deep", "deep-high", "deep-ultra"]
    src = PCKERNEL.read_text(encoding="utf-8")
    assert "(pc-set-ssao-preset (-> want ssao-quality))" in _goal_block(src, "(defun sandover-settings-update")


def test_ssao_tiers_before_version_4_keep_their_pixels():
    """Version 3 had low, medium, high = SAO deep-low, deep-medium, deep; deep is medium now."""
    for old, new in (("low", "low"), ("medium", "low"), ("high", "medium")):
        assert settings.parse(f"(sandover 3 (ssao-quality {old}))")[1]["ssao-quality"] == new
    assert settings.parse("(sandover 4 (ssao-quality high))")[1]["ssao-quality"] == "high"
    assert _rows()["ssao-quality"]["names"] == ["low", "medium", "high", "ultra"]
    read = _flat(_goal_block(PCKERNEL.read_text(encoding="utf-8"), "(defun sandover-field-read"))
    assert '(string= (-> f key) "ssao-quality") (or (= i 1) (= i 2))) (+ i -1) i)' in read
    gfx = (REPO / "game" / "graphics" / "gfx.h").read_text(encoding="utf-8")
    assert "int shadow_quality = 2;" in gfx and "QUALITY = 40," in gfx


def test_reapply_resets_the_new_sub_options():
    """Every field added with the tier rows is forgotten by sandover-settings-reapply at its boot
    state, and shadow option 40 (map size and taps) goes back to the engine default."""
    src = PCKERNEL.read_text(encoding="utf-8")
    reapply = _goal_block(src, "(defun sandover-settings-reapply")
    rows = _rows()
    for key, value in (("ssao-quality", 1), ("shadow-quality", 2), ("sun?", 0)):
        assert rows[key]["boot"] == value, key
    assert "(pc-set-shadow-option 40 2000000)" in reapply
    km = bridge_text(REPO)
    assert "case ShadowOption::QUALITY:" in km


def test_sun_row():
    assert settings.DEFAULTS["sun?"] is True and settings.ALL_OFF["sun?"] is False
    # an older all-off file (an older rig's seed) keeps the sun off
    assert settings.parse("(sandover 2 (neighbours? #f))")[1]["sun?"] is False
    assert settings.parse("(sandover 2 (neighbours? #t))")[1]["sun?"] is True


def test_menu_rows():
    menu = MENU.read_text(encoding="utf-8")
    page = menu[menu.index("(define *sandover-options*"):menu.index("(define *sandover-calibration-options*")]
    # a row is a game-option with a name, or a (sandover-row type name) macro call
    rows = [a or b for a, b in re.findall(r":name \(text-id ([a-z0-9-]+)\)|\(sandover-row [a-z-]+ ([a-z0-9-]+)\)", page)]
    assert rows == ["sandover-vanilla", "sandover-ssao", "sandover-ssao-quality", "sandover-tile-lights", "sandover-bloom",
                    "sandover-shadows", "sandover-shadow-quality", "sandover-materials", "sandover-fur",
                    "sandover-water", "sandover-glint", "sandover-neighbours", "sandover-sun",
                    "sandover-motion-blur", "sandover-camera-blur", "sandover-contact-shadows", "sandover-fp16",
                    "sandover-hdr-output", "sandover-hdr-calibration",
                    "sandover-upscaler", "sandover-render-scale", "sandover-audio", "sandover-bug-report", "back"]
    audio = menu[menu.index("(define *sandover-audio-options*"):menu.index("(defun sandover-option-pointer")]
    assert re.findall(r":name \(text-id ([a-z0-9-]+)\)", audio) == [
        "sandover-audio-output", "sandover-audio-device", "sandover-audio-test", "back"]


def test_menu_table_binds_every_row_to_a_field():
    """Each row of the page that edits a field is in *sandover-menu* with a file key that exists; the
    parent column names the switch or list whose off state greys the row."""
    keys = {r["key"]: r for r in settings_table.fields()}
    rows = settings_table.menu()
    assert len({r["name"] for r in rows}) == len(rows)
    for r in rows:
        assert r["field"] in keys, r
        if r["parent"]:
            assert r["parent"] in keys, r
        # a list row has its carousel in the table; a switch or number has none
        kind = keys[r["field"]]["kind"]
        if kind in (ENUM, INTMAP, WORD):
            assert r["carousel"] or r["name"] == "sandover-upscaler", r
        else:
            assert not r["carousel"], r
    assert all(keys[r["parent"]]["kind"] in (BOOL, ENUM, INTMAP) for r in rows if r["parent"])
    assert {r["name"]: r["parent"] for r in rows if r["parent"]} == {
        "sandover-ssao-quality": "ssao?", "sandover-tile-lights": "ssao?",
        "sandover-shadow-quality": "shadows?", "sandover-camera-blur": "motion-blur"}
    menu = MENU.read_text(encoding="utf-8")
    page = menu[menu.index("(define *sandover-options*"):menu.index("(define *sandover-calibration-options*")]
    # fifteen rows take the table's rule; the five with their own rule keep an explicit lambda
    macro = re.findall(r"\(sandover-row [a-z-]+ ([a-z0-9-]+)\)", page)
    assert macro == ["sandover-ssao", "sandover-ssao-quality", "sandover-tile-lights", "sandover-bloom",
                     "sandover-shadows", "sandover-shadow-quality", "sandover-materials", "sandover-fur",
                     "sandover-water", "sandover-glint", "sandover-neighbours", "sandover-sun",
                     "sandover-motion-blur", "sandover-camera-blur", "sandover-fp16"]
    for name in ("sandover-contact-shadows", "sandover-hdr-output", "sandover-hdr-calibration",
                 "sandover-upscaler", "sandover-render-scale"):
        assert "(lambda ()" in _page_row(menu, name), name


def test_audio_rows_round_trip():
    assert settings.KEYS["audio-output"] == ("auto", "stereo", "5.1", "7.1", "headphones", "spatial")
    assert settings.DEFAULTS["audio-output"] == "auto" and settings.DEFAULTS["audio-device"] == ""
    assert settings.ALL_OFF["audio-output"] == "stereo"  # the vanilla stream, as at boot
    name = "OpenAL Soft on Speakers (Steam Streaming Speakers)"
    text = settings.dumps({"audio-output": "5.1", "audio-device": name})
    assert f'(audio-device "{name}")' in text and "(audio-output 5.1)" in text
    values = settings.parse(text)[1]
    assert values["audio-output"] == "5.1" and values["audio-device"] == name
    # an older file has neither key: auto on the Windows default device
    values = settings.parse("(sandover 4 (ssao? #t))")[1]
    assert values["audio-output"] == "auto" and values["audio-device"] == ""
    _, values, problems = settings.parse("(sandover 4 (audio-device default))")
    assert values["audio-device"] == "" and problems


def test_spatial_output_round_trip():
    # Spatial is saved like the other outputs, in both profiles, with the device it was picked on
    name = "OpenAL Soft on Digital Output (USB Audio DAC)"
    for vr in (False, True):
        text = settings.dumps({"audio-output": "spatial", "audio-device": name}, vr=vr)
        assert "(audio-output spatial)" in text and f'(audio-device "{name}")' in text
        _, values, problems = settings.parse(text, vr=vr)
        assert values["audio-output"] == "spatial" and values["audio-device"] == name and not problems
        assert settings.dumps(values, vr=vr) == text
    # every output survives a write and a read
    for out in settings.KEYS["audio-output"]:
        assert settings.parse(settings.dumps({"audio-output": out}))[1]["audio-output"] == out


def test_engine_start_reader_matches_the_game():
    # gk reads audio-output before OpenAL starts (main.cpp, snd::ParseSavedAudio): its names are
    # the game's, and objects (the engine's own name) is never written
    spatial = (REPO / "game" / "sound" / "989snd" / "spatial.cpp").read_text(encoding="utf-8")
    assert 'return out == AudioOutput::Objects ? "spatial" : AudioOutputName(out);' in spatial
    for n in settings.KEYS["audio-output"][:-1]:
        assert f'return "{n}";' in spatial, n
    main = (REPO / "game" / "main.cpp").read_text(encoding="utf-8")
    assert "snd::SavedAudioFileName(" in main and "snd::ConfigureObjectsAtStart(saved.device)" in main
    assert main.index("snd::ConfigureObjectsAtStart") > main.index("setup_logging(")


def test_audio_reader_matches_the_game():
    src = PCKERNEL.read_text(encoding="utf-8")
    rows = _rows()
    row = rows["audio-output"]
    # a bare word (5.1 would read as a number) matched against the names in index order
    assert row["kind"] == WORD and row["words"] == list(settings.KEYS["audio-output"])
    assert row["words"][row["fallback"]] == "auto"
    read = _flat(_goal_block(src, "(defun sandover-field-read"))
    assert "(file-stream-seek-past-whitespace file) (file-stream-read-word file)" in read
    assert "(string= *pc-temp-string* (-> f words i))" in read
    assert '(format file "~S" (-> f words' in _flat(_goal_block(src, "(defun sandover-field-write"))
    # the device field's size matches the mirror's limit
    impl = IMPL.read_text(encoding="utf-8")
    assert f"(audio-device uint8 {settings.DEVICE_NAME_BYTES + 1})" in impl
    device = rows["audio-device"]
    assert device["kind"] == BYTES and device["size"] == settings.DEVICE_NAME_BYTES + 1
    assert "(sandover-read-quoted file (the-as (pointer uint8) addr) (-> f size))" in read


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
    row = _rows()["glint?"]
    assert row["kind"] == BOOL and row["default"] == 1 and row["boot"] == 0


def test_contact_shadows_row():
    """One on/off row, off by default and in the boot state, greyed and forced off without temporal
    AA; on sends shadow option 41 at the strong level, and reapply forgets it."""
    assert settings.DEFAULTS["contact-shadows?"] is False and settings.ALL_OFF["contact-shadows?"] is False
    assert settings.parse(settings.dumps({"contact-shadows?": True}))[1]["contact-shadows?"] is True
    src = PCKERNEL.read_text(encoding="utf-8")
    reapply = _goal_block(src, "(defun sandover-settings-reapply")
    assert "(set! (-> *sandover-applied* contact-shadows?) #f)" in reapply
    assert "(pc-set-shadow-option 41 0)" in reapply
    assert "(pc-set-shadow-option 41 (if (-> want contact-shadows?) 2000000 0))" in _goal_block(
        src, "(defun sandover-settings-update")
    assert "(set! (-> dst contact-shadows?) #f)" in _goal_block(src, "(defun sandover-effective!")


def test_motion_blur_row():
    """One list row, Off / Weak / Medium / Strong, Medium in the play look and off in the boot
    state; the push sends the level as pc-set-motion-blur 0, and the reapply resets its options."""
    assert settings.KEYS["motion-blur"] == ("off", "weak", "medium", "strong")
    assert settings.DEFAULTS["motion-blur"] == "medium" and settings.ALL_OFF["motion-blur"] == "off"
    assert settings.parse(settings.dumps({"motion-blur": "strong"}))[1]["motion-blur"] == "strong"
    src = PCKERNEL.read_text(encoding="utf-8")
    reapply = _goal_block(src, "(defun sandover-settings-reapply")
    assert "(set! (-> *sandover-applied* motion-blur) 0)" in reapply
    assert "(pc-set-motion-blur 7 1)" in reapply
    update = _goal_block(src, "(defun sandover-settings-update")
    assert "(pc-set-motion-blur 0 (* 1000000 (-> want motion-blur)))" in update
    menu = {r["name"]: r for r in settings_table.menu()}
    assert menu["sandover-motion-blur"]["carousel"] == ["off", "sandover-mb-weak", "sandover-quality-medium", "sandover-mb-strong"]
    gfx = (REPO / "game" / "graphics" / "gfx.h").read_text(encoding="utf-8")
    assert "int mb_level = 0;" in gfx


def test_camera_blur_row():
    """Off / Vehicles / Always, Vehicles in the play look and off in the boot state; the push sends
    the mode as pc-set-motion-blur 10, the mounted flag goes as 11 on change from the game's own
    test (collide-action racer or flut), and the row is greyed while the strength row is off."""
    assert settings.KEYS["camera-blur"] == ("off", "vehicles", "always")
    assert settings.DEFAULTS["camera-blur"] == "vehicles" and settings.ALL_OFF["camera-blur"] == "off"
    assert settings.parse(settings.dumps({"camera-blur": "always"}))[1]["camera-blur"] == "always"
    src = PCKERNEL.read_text(encoding="utf-8")
    riding = _goal_block(src, "(defun sandover-riding-vehicle?")
    assert "(-> *target* control root-prim prim-core action) (collide-action racer flut)" in riding
    update = _goal_block(src, "(defun sandover-settings-update")
    assert "(pc-set-motion-blur 10 (* 1000000 (-> want camera-blur)))" in update
    assert "(pc-set-motion-blur 11 riding)" in update
    reapply = _goal_block(src, "(defun sandover-settings-reapply")
    assert "(pc-set-motion-blur 10 0)" in reapply and "(pc-set-motion-blur 11 0)" in reapply
    menu = {r["name"]: r for r in settings_table.menu()}
    assert menu["sandover-camera-blur"]["carousel"] == ["off", "sandover-cb-vehicles", "sandover-cb-always"]
    # greyed while the strength row is zero: the parent column, read by sandover-field-off?
    assert menu["sandover-camera-blur"]["parent"] == "motion-blur"
    off = _flat(_goal_block(IMPL.read_text(encoding="utf-8"), "(defun sandover-field-off?"))
    assert "(zero? (-> (the-as (pointer int32) (sandover-field-addr obj f))))" in off
    gfx = (REPO / "game" / "graphics" / "gfx.h").read_text(encoding="utf-8")
    assert "int mb_camera = 0;" in gfx


def test_list_keys_are_written_bare_and_read_through_read_name():
    src = PCKERNEL.read_text(encoding="utf-8")
    keys = {r["key"] for r in settings_table.fields() if r["kind"] == ENUM}
    assert keys == {"material-look", "tonemap", "hdr-output", "upscaler",
                    "ssao-quality", "shadow-quality", "motion-blur", "camera-blur"}
    # the names are symbols: written with ~A they are bare, and read back through the quote-tolerant reader
    text = IMPL.read_text(encoding="utf-8")
    for key in keys:
        seg = text[text.index(f'"{key}" :kind SANDOVER_FIELD_ENUM'):]
        assert ":names (new 'static 'boxed-array :type symbol" in seg[:seg.index("(new 'static 'sandover-field")], key
    read = _flat(_goal_block(src, "(defun sandover-field-read"))
    assert "((SANDOVER_FIELD_ENUM) (let ((name (sandover-read-name file)))" in read
    write = _flat(_goal_block(src, "(defun sandover-field-write"))
    assert '((SANDOVER_FIELD_ENUM) (format file "~A" (-> f names' in write
    assert write.count("~S") == 2  # the key and the audio word; a symbol name is never quoted
    assert '(format file " (~S " (-> f key))' in write


def test_gtao_tiers_run_without_temporal_aa():
    """A setting does only what its row says: no tier clamp; without motion vectors the renderer runs a
    fixed noise pattern with more denoise passes."""
    block = _goal_block(PCKERNEL.read_text(encoding="utf-8"), "(defun sandover-effective!")
    assert "(>= (-> dst ssao-quality)" not in block


def test_vr_profile_file_round_trips_with_its_msaa(tmp_path):
    path = settings.write(tmp_path / settings.VR_FILE_NAME, {"msaa": 4, "bloom?": False})
    version, values, problems = settings.parse(path.read_text(encoding="utf-8"), vr=True)
    assert problems == [] and values == settings.VR_DEFAULTS | {"msaa": 4, "bloom?": False}
    assert "(msaa 4)" in path.read_text(encoding="utf-8")
    # the flat file has no MSAA key (pc-settings.gc holds it); a VR key there is unknown
    assert "msaa" not in settings.dumps({"msaa": 4})
    assert any("msaa" in p for p in settings.parse("(sandover 4 (msaa 4))")[2])
    # a VR file without one keeps the default (the game then takes the flat screen's MSAA)
    assert "msaa" not in settings.parse("(sandover 4)", vr=True)[1]
    assert settings.check(path) == []
    bad = tmp_path / settings.VR_FILE_NAME
    bad.write_text("(sandover 4 (msaa 3))", encoding="utf-8")
    assert len(settings.check(bad)) == 1


def test_vr_defaults_match_the_game():
    """The VR profile's defaults: the flat ones but the render scale (sandover-settings-reset-vr!)."""
    assert settings.VR_DEFAULTS == settings.DEFAULTS | {"render-scale": settings.VR_RENDER_SCALE}
    impl = IMPL.read_text(encoding="utf-8")
    idx = int(re.search(r"\(defconstant SANDOVER_VR_RENDER_SCALE (\d)\)", impl).group(1))
    assert settings.KEYS["render-scale"][idx] == settings.VR_RENDER_SCALE == 67
    reset = _goal_block(impl, "(defun sandover-settings-reset-vr!")
    assert "(sandover-settings-reset! obj)" in reset
    assert "(set! (-> obj render-scale) SANDOVER_VR_RENDER_SCALE)" in reset
    # PSVR2 (findings 2026-10-27): each eye drawn at 4491x3468 at 100%; 67% stays under 3840x2160
    w, h = round(4491 * 2 / 3), round(3468 * 2 / 3)
    assert w * h <= 3840 * 2160 < round(4491 / 1.3) * round(3468 / 1.3)


def test_seed_off_writes_both_profiles(tmp_path):
    settings.seed_off(tmp_path)
    vr = settings.settings_path(tmp_path, vr=True)
    assert vr.name == "sandover-vr.gc" and vr.parent == settings.settings_path(tmp_path).parent
    _, values, problems = settings.parse(vr.read_text(encoding="utf-8"), vr=True)
    assert problems == [] and values["vanilla-rendering?"] is True and "msaa" not in values
    assert values["render-scale"] == 100


def test_profile_rows_and_strings():
    """The page names its profile; the upscaler row is the same list in VR (it runs per eye; the
    engine reports DLSS unavailable there); the render scale row greys only where it does nothing
    (no temporal AA and no XR session); contact shadows grey in stereo and XR."""
    menu = MENU.read_text(encoding="utf-8")
    assert "upscaler-vr" not in menu and "sandover-up-vr" not in menu
    contact = _page_row(menu, "sandover-contact-shadows")
    assert "(sandover-stereo-engine?)" in contact
    km = bridge_text(REPO)
    assert "return Gfx::g_dlss_supported && !(g_game_version == GameVersion::Jak1 && Gfx::stereo_requested())" in km
    assert "((sandover-xr-session?)" in menu and "the share of the eye frame (pc-get-xr-state)" in menu
    row = _page_row(menu, "sandover-render-scale")
    assert "(not (or (sandover-temporal-upscaler?) (sandover-xr-session?)))" in row
    assert "(text-id sandover-profile-vr) (text-id sandover-profile-flat)" in menu
    assert "(set! *sandover-menu-editing-func* sandover-menu-editing?)" in menu


