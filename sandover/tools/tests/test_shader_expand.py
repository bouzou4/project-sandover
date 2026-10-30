import re
from pathlib import Path

import pytest

from sandover import shader_expand as se

ROOT = Path(__file__).resolve().parents[3]
CPP = (ROOT / se.CPP / "Shader.cpp").read_text()
TABLE = se.parse_table(CPP)
PROGRAMS = se.programs(ROOT)


def test_table_is_read_from_shader_cpp():
    assert TABLE is not se.LEGACY
    assert len(TABLE) >= len(se.LEGACY)
    assert TABLE[-1].token == "LEVEL_HEIGHT_GLSL"
    every = {s.token for s in TABLE if s.every}
    assert every == {"CANOPY_FRAG", "WORLD_SHADOW_FRAG", "WORLD_SHADOW_VERT", "LEVEL_HEIGHT_GLSL"}


def test_legacy_prefix_of_the_table_keeps_today_s_order():
    legacy = [s for s in TABLE if s in se.LEGACY]
    assert legacy == list(se.LEGACY)


def test_no_token_contains_another():
    toks = [s.token for s in TABLE]
    assert len(set(toks)) == len(toks)
    for a in toks:
        for b in toks:
            assert a == b or a not in b


def test_every_snippet_file_is_in_the_table_and_back():
    files = {s.file for s in TABLE}
    on_disk = {p.name for p in (ROOT / se.SHADER_DIR).glob("*.glsl")}
    assert files == on_disk


def test_programs_cover_the_library():
    names = {n for n, _ in PROGRAMS}
    assert {"tfrag3", "merc2", "emerc", "ocean_water", "world_shadow", "actor_shadow_occ"} <= names
    for name, frag in PROGRAMS:
        assert (ROOT / se.SHADER_DIR / f"{name}.vert").exists()
        assert (ROOT / se.SHADER_DIR / f"{frag}.frag").exists()


@pytest.mark.parametrize("version", list(se.VERSIONS))
def test_every_program_expands_cleanly(version):
    tokens = [s.token for s in TABLE] + ["HEIGHT_SCALE", "SCISSOR_HEIGHT", "SCISSOR_ADJUST"]
    for name, frag in PROGRAMS:
        for stage, text in se.expand(ROOT, name, frag, version, TABLE).items():
            code = re.sub(r"//.*", "", text)
            left = [t for t in tokens if re.search(rf"(?<![A-Z_]){t}(?![A-Z_])", code)]
            assert not left, f"{name}.{stage} still holds {left}"


def test_first_use_replaces_only_the_first_token():
    table = (se.Snippet("TOK", "a.glsl", se.FRAG, False),)
    assert se.splice("TOK\nTOK\n", se.FRAG, table, lambda f: "body") == "body\nTOK\n"


def test_every_use_replaces_all_and_never_rescans():
    table = (se.Snippet("TOK", "a.glsl", se.FRAG, True),)
    out = se.splice("TOK x TOK", se.FRAG, table, lambda f: "[TOK]")
    assert out == "[TOK] x [TOK]"


def test_later_rows_see_text_spliced_by_earlier_ones():
    table = (se.Snippet("A", "a", se.FRAG, False), se.Snippet("B", "b", se.FRAG, False))
    files = {"a": "B", "b": "done"}
    assert se.splice("A", se.FRAG, table, files.__getitem__) == "done"
    assert se.splice("B A", se.FRAG, table[::-1], files.__getitem__) == "done B"


def test_stage_mask_limits_where_a_row_applies():
    table = (se.Snippet("T", "t", se.VERT | se.TESC, False),)
    assert se.splice("T", se.FRAG, table, lambda f: "x") == "T"
    assert se.splice("T", se.TESC, table, lambda f: "x") == "x"


def test_compare_separates_whitespace_from_real_changes(tmp_path):
    a, b = tmp_path / "a", tmp_path / "b"
    for d in (a, b):
        d.mkdir()
    (a / "same.vert").write_text("x\ny\n")
    (b / "same.vert").write_text("x\ny\n")
    (a / "ws.vert").write_text("x\ny\n")
    (b / "ws.vert").write_text("x\n\n  y\n")
    (a / "diff.vert").write_text("x\n")
    (b / "diff.vert").write_text("z\n")
    res = se.compare(a, b)
    assert res["raw_identical"] == 1
    assert res["whitespace_only"] == ["ws.vert"]
    assert res["different"] == ["diff.vert"]


def test_dump_writes_every_program_for_both_versions(tmp_path):
    n = se.dump(ROOT, tmp_path)
    assert n == len(list(tmp_path.rglob("*.*")))
    assert (tmp_path / "jak1" / "tfrag3.vert").exists()
    assert (tmp_path / "jak2" / "ocean_water+ocean_common.tese").exists()
    a = (tmp_path / "jak1" / "tfrag3.vert").read_text()
    b = (tmp_path / "jak2" / "tfrag3.vert").read_text()
    assert a != b or "HEIGHT_SCALE" not in (ROOT / se.SHADER_DIR / "tfrag3.vert").read_text()
