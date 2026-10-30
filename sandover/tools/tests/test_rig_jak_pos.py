"""Shot manifests: the optional Jak position."""

from pathlib import Path

from sandover.rig.capture import load_manifest

SHOTS = Path(__file__).resolve().parents[2] / "shots"


def test_jak_pos_is_optional_and_parsed(tmp_path):
    m = tmp_path / "m.yaml"
    m.write_text(
        "set: t\n"
        "shots:\n"
        "  - {name: a, continue: jungle-start, hour: 12}\n"
        "  - {name: b, continue: jungle-start, hour: 12, jak_pos: [1, 2.5, -3]}\n",
        encoding="utf-8")
    _, shots = load_manifest(m)
    assert shots[0].jak_pos is None
    assert shots[1].jak_pos == (1.0, 2.5, -3.0)


def test_temple_top_is_in_the_standard_lists():
    for manifest in ("shadows-v1.yaml", "emissive.yaml"):
        _, shots = load_manifest(SHOTS / manifest)
        top = [s for s in shots if s.name.startswith("jungle-temple-top")]
        assert len(top) == 1 and top[0].jak_pos and isinstance(top[0].camera, dict)
