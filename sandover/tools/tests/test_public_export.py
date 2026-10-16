import importlib.util
import re
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
_spec = importlib.util.spec_from_file_location(
    "make_public_tree", REPO / "sandover" / "release" / "make_public_tree.py")
mpt = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mpt)


def test_patterns_match_trees_and_files():
    pats = mpt.load_patterns()
    assert mpt.is_excluded("CLAUDE.md", pats)
    assert mpt.is_excluded(".claude/agents/x.md", pats)
    assert mpt.is_excluded("sandover/docs/design/sections/07-milestones.md", pats)
    assert not mpt.is_excluded("AGENTS.md", pats)
    assert not mpt.is_excluded("sandover/kb/howto/parity.md", pats)


def test_strip_helpers():
    toml = 'a = "x"\nsandover-hub = "sandover.hub:main"\nb = "y"\n'
    assert mpt.strip_entry_points(toml) == 'a = "x"\nb = "y"\n'
    yml = "tasks:\n  keep:\n    cmds:\n      - a\n\n  review:\n    cmds:\n      - b\n"
    assert mpt.strip_tasks(yml) == "tasks:\n  keep:\n    cmds:\n      - a\n\n"


def test_markdown_links_ignore_urls_and_anchors():
    got = list(mpt.markdown_links("[a](x.md#s) [b](https://e.org/y) [c](#top) [d](../z.md)"))
    assert got == ["x.md", "../z.md"]


def test_export_has_no_excluded_path_and_no_dead_entry_point(tmp_path):
    try:
        subprocess.run(["git", "rev-parse", "HEAD"], cwd=REPO, check=True, capture_output=True)
    except (subprocess.CalledProcessError, FileNotFoundError):
        pytest.skip("not a git checkout")
    files = mpt.export("HEAD", tmp_path, REPO)
    pats = mpt.load_patterns()
    assert files
    assert not [f for f in files if mpt.is_excluded(f, pats)]
    for name in mpt.REMOVED_ENTRY_POINTS:
        for rel in (mpt.PYPROJECT, mpt.TASKFILE):
            text = (tmp_path / rel).read_text(encoding="utf-8")
            assert not re.search(rf"\b{re.escape(name)}\b", text), (name, rel)
    scripts = (tmp_path / mpt.PYPROJECT).read_text(encoding="utf-8")
    assert "sandover-viewer" in scripts and "sandover-capture" in scripts


def test_every_entry_point_target_exists_in_export(tmp_path):
    _checkout()
    files = set(mpt.export("HEAD", tmp_path, REPO))
    scripts = (tmp_path / mpt.PYPROJECT).read_text(encoding="utf-8")
    for mod in re.findall(r'=\s*"sandover\.([\w.]+):\w+"', scripts):
        base = "sandover/tools/sandover/" + mod.replace(".", "/")
        assert base + ".py" in files or base + "/__init__.py" in files, mod


def test_kb_markdown_links_resolve():
    bad = mpt.dangling_links(REPO, "sandover/kb")
    assert not bad, bad[:20]


def _checkout():
    try:
        subprocess.run(["git", "rev-parse", "HEAD"], cwd=REPO, check=True, capture_output=True)
    except (subprocess.CalledProcessError, FileNotFoundError):
        pytest.skip("not a git checkout")


def test_export_keeps_exec_bits(tmp_path):
    import os
    import sys
    _checkout()
    if sys.platform == "win32":
        pytest.skip("POSIX file modes")
    files = set(mpt.export("HEAD", tmp_path, REPO))
    ls = subprocess.run(["git", "ls-files", "-s"], cwd=REPO, check=True, capture_output=True,
                        text=True).stdout.splitlines()
    exe = [l.split("	", 1)[1] for l in ls if l.startswith("100755")]
    exe = [p for p in exe if p in files]
    assert exe
    assert all(os.access(tmp_path / p, os.X_OK) for p in exe)


def test_shipped_text_names_no_excluded_file(tmp_path):
    _checkout()
    mpt.export("HEAD", tmp_path, REPO)
    assert not mpt.excluded_mentions(tmp_path)
    assert not mpt.dangling_refs(tmp_path)
