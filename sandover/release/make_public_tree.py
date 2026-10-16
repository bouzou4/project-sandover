"""Produce a clean export of the repository for the public import.

    python make_public_tree.py OUT_DIR [--ref REF] [--check-links]

Takes the tracked files of REF (default HEAD) via ``git archive``, deletes every path
matched by ``public-exclude.txt``, and removes the console-script entries and Taskfile
tasks that call excluded tools, so what remains still imports. ``--check-links`` prints
markdown links in the export that point at a path that is no longer there.
"""

from __future__ import annotations

import argparse
import fnmatch
import io
import re
import subprocess
import sys
import tarfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
EXCLUDE_FILE = HERE / "public-exclude.txt"
PYPROJECT = "sandover/tools/pyproject.toml"
TASKFILE = "sandover/Taskfile.yml"
REMOVED_ENTRY_POINTS = ("sandover-hub", "sandover-picker")
REMOVED_TASKS = ("review",)


def load_patterns(path: Path = EXCLUDE_FILE) -> list[str]:
    out = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            out.append(line)
    return out


def is_excluded(rel: str, patterns: list[str]) -> bool:
    for pat in patterns:
        if pat.endswith("/**"):
            base = pat[:-3]
            if rel == base or rel.startswith(base + "/"):
                return True
        elif fnmatch.fnmatchcase(rel, pat):
            return True
    return False


def strip_entry_points(text: str, names=REMOVED_ENTRY_POINTS) -> str:
    keep = [ln for ln in text.splitlines(keepends=True)
            if not any(re.match(rf"\s*{re.escape(n)}\s*=", ln) for n in names)]
    return "".join(keep)


def strip_tasks(text: str, names=REMOVED_TASKS) -> str:
    """Drop top-level-of-``tasks:`` blocks (two-space indent) named in ``names``."""
    out, skipping = [], False
    for ln in text.splitlines(keepends=True):
        m = re.match(r"^  ([\w:-]+):\s*$", ln)
        if m:
            skipping = m.group(1) in names
        elif skipping and ln.strip() and not ln.startswith("   "):
            skipping = False
        if not skipping:
            out.append(ln)
    return "".join(out)


def export(ref: str, out: Path, repo: Path | None = None) -> list[str]:
    """Write the filtered tree of ``ref`` into ``out``; returns the exported paths."""
    patterns = load_patterns()
    cmd = ["git", "archive", "--format=tar", ref]
    data = subprocess.run(cmd, cwd=repo, check=True, capture_output=True).stdout
    out.mkdir(parents=True, exist_ok=True)
    written = []
    with tarfile.open(fileobj=io.BytesIO(data)) as tar:
        for m in tar.getmembers():
            if not m.isfile() or is_excluded(m.name, patterns):
                continue
            body = tar.extractfile(m).read()
            if m.name == PYPROJECT:
                body = strip_entry_points(body.decode("utf-8")).encode("utf-8")
            elif m.name == TASKFILE:
                body = strip_tasks(body.decode("utf-8")).encode("utf-8")
            dest = out / m.name
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(body)
            dest.chmod(m.mode & 0o7777)
            written.append(m.name)
    return written


LINK = re.compile(r"\[[^\]]*\]\(([^)\s]+)\)")


def markdown_links(text: str):
    """Yield link targets that are relative file paths (no scheme, no pure anchor)."""
    for target in LINK.findall(text):
        target = target.split("#", 1)[0]
        if target and not re.match(r"^[a-z][a-z0-9+.-]*:", target, re.I) and not target.startswith("/"):
            yield target


def dangling_links(root: Path, subdir: str = "") -> list[tuple[str, str]]:
    bad = []
    for md in sorted((root / subdir).rglob("*.md")):
        for target in markdown_links(md.read_text(encoding="utf-8", errors="replace")):
            if not (md.parent / target).exists():
                bad.append((md.relative_to(root).as_posix(), target))
    return bad


MENTION = re.compile(
    r"(BRIEF\.md|STATE\.md|CLAUDE\.md|docs/design|howto/picker\.md|howto/hub\.md|ADR-0005|"
    r"(?<![\w])hub\.py|(?<![\w])picker\.py|hubcheck\.py|"
    r"sandover-picker|sandover\.picker|sandover-hub|sandover\.hub|"
    r"(?i:review hub|preset picker|design doc|design §|\bthe (?:project |original )?brief\b))")
SPAN = re.compile(r"`([\w./-]+\.(?:md|py|toml|ya?ml))`")
TEXT_SUFFIXES = (".md", ".py", ".toml", ".yaml", ".yml", ".js", ".html")


def dangling_refs(root: Path, subdir: str = "sandover") -> list[tuple[str, str]]:
    """Backticked file paths in shipped text that resolve nowhere in the tree."""
    bad = []
    for p in sorted((root / subdir).rglob("*")):
        if not p.is_file() or p.suffix not in (".md", ".py"):
            continue
        for span in SPAN.findall(p.read_text(encoding="utf-8", errors="replace")):
            if not span.startswith(("kb/", "sandover/")) or span.startswith("sandover/local/"):
                continue
            bases = (p.parent, root, root / "sandover", root / "sandover" / "kb",
                     root / "sandover" / "tools", root / "sandover" / "tools" / "sandover")
            if not any((b / span).exists() for b in bases):
                bad.append((p.relative_to(root).as_posix(), span))
    return bad


def excluded_mentions(root: Path, subdir: str = "sandover") -> list[tuple[str, str]]:
    """Mentions of files that stay private, in shipped text."""
    hits = []
    for p in sorted((root / subdir).rglob("*")):
        rel = p.relative_to(root).as_posix()
        if not p.is_file() or p.suffix not in TEXT_SUFFIXES or rel.startswith("sandover/release/")                 or rel.endswith("test_public_export.py"):
            continue
        for m in MENTION.finditer(p.read_text(encoding="utf-8", errors="replace")):
            hits.append((p.relative_to(root).as_posix(), m.group(1)))
    return hits


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("out", type=Path)
    ap.add_argument("--ref", default="HEAD")
    ap.add_argument("--check-links", action="store_true")
    args = ap.parse_args(argv)
    files = export(args.ref, args.out)
    print(f"exported {len(files)} files to {args.out}")
    if args.check_links:
        bad = dangling_links(args.out, "sandover")
        for src, target in bad:
            print(f"dangling: {src} -> {target}")
        print(f"{len(bad)} dangling links")
        refs = dangling_refs(args.out) + excluded_mentions(args.out)
        for src, target in refs:
            print(f"dangling reference: {src} -> {target}")
        print(f"{len(refs)} dangling references")
    return 0


if __name__ == "__main__":
    sys.exit(main())
