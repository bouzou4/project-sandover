"""Proves the kmachine.cpp bridge split moved code verbatim and kept the registration order.

    python -m sandover.bridge_check --base <git ref before the split> [--root <checkout>]

Checks: (1) the ordered list of bridge symbol names registered by init_common_pc_port_functions
is identical before and after (the table expanded at its two call sites); (2) every top-level
block removed from kmachine.cpp reappears byte for byte in a bridge file.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

from sandover.bridge_sources import KERNEL, bridge_files

REG = re.compile(r'make_func_symbol_func\(\s*"([^"]+)"')
ENTRY = re.compile(r'\{"([^"]+)", \(void\*\)(\w+), BridgeScope::(AllGames|Jak1)\}')


def git_show(root: Path, ref: str, rel: str) -> str:
    return subprocess.run(["git", "show", f"{ref}:{rel}"], cwd=root, capture_output=True,
                          text=True, encoding="utf-8", check=True).stdout


def registrations(km: str, table: str | None) -> list[str]:
    """Names registered by init_common_pc_port_functions, in call order."""
    body = km[km.index("void init_common_pc_port_functions("):]
    if table is None:
        return REG.findall(body)
    entries = ENTRY.findall(table)
    out = []
    for m in re.finditer(r'make_func_symbol_func\(\s*"([^"]+)"|register_sandover_bridge\(\w+, BridgeScope::(\w+)\)',
                         body):
        if m.group(1):
            out.append(m.group(1))
        else:
            out += [n for n, _, scope in entries if scope == m.group(2)]
    return out


def blocks(text: str) -> list[str]:
    """Top-level blocks: runs of lines ending where the brace depth returns to 0."""
    out, cur, depth, seen = [], [], 0, False
    for line in text.split("\n"):
        cur.append(line)
        t = re.sub(r'"(\\.|[^"\\])*"', '""', line)
        t = re.sub(r"'(\\.|[^'\\])'", "''", t).split("//")[0]
        depth += t.count("{") - t.count("}")
        seen = seen or "{" in t or "}" in t
        st = t.strip()
        if depth == 0 and (seen or st.endswith(";")) and st and not st.startswith(("/*", "*", "//")):
            out.append("\n".join(cur).strip("\n"))
            cur, seen = [], False
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True)
    ap.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[3])
    a = ap.parse_args()
    rel = (KERNEL / "kmachine.cpp").as_posix()
    old = git_show(a.root, a.base, rel)
    files = bridge_files(a.root)
    new_km = files[0].read_text(encoding="utf-8")
    table = files[-1].read_text(encoding="utf-8")
    bridge = "\n".join(p.read_text(encoding="utf-8") for p in files[1:-1])
    bridge += "\n" + (files[-1].parent / "bridge.h").read_text(encoding="utf-8")

    ok = True
    before = registrations(old, None)
    after = registrations(new_km, table)
    bridge_names = [n for n, _, _ in ENTRY.findall(table)]
    # the bridge subset of the old list: names the table owns
    before_bridge = [n for n in before if n in set(bridge_names)]
    same = before_bridge == bridge_names and len(bridge_names) == len(set(bridge_names))
    print(f"bridge symbols: before {len(before_bridge)}, after {len(bridge_names)}, "
          f"identical and ordered: {same}")
    ok &= same
    same_full = before == after
    print(f"full registration order ({len(before)} names), table expanded in place: {same_full}")
    ok &= same_full

    moved = missing = edited = 0
    for b in blocks(old):
        if b in new_km:
            continue
        if b.startswith("#include") or "\nvoid init_common_pc_port_functions(" in b:
            edited += 1  # includes pruned, registration replaced by the table call
            continue
        if b in bridge:
            moved += 1
        else:
            code = [ln for ln in b.split("\n") if ln.strip() and not ln.strip().startswith(("//", "/*", "*"))]
            print("NOT moved verbatim:", code[0][:100] if code else b[:100])
            missing += 1
    print(f"blocks removed from kmachine.cpp and found verbatim in bridge files: {moved}; "
          f"not found: {missing}; edited in place: {edited}")
    ok &= missing == 0
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
