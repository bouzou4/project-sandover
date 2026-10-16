"""CMake configure seed: the cached results of a configure's try-compile checks, replayed
with ``cmake -C`` so a fresh worktree's configure skips them.

A fresh configure of this tree runs ~280 try-compiles (SDL most of them), each a ninja + clang
compile + lld link, which is most of its time. :func:`harvest` reads a finished configure's
``CMakeConfigureLog.yaml`` and ``CMakeCache.txt`` and writes :func:`seed_path`: every cached check
result, grouped by the CMake files whose code defines the check. At configure time the seed
applies a group only if those files still hash the same and the CMake and clang versions
match, so an edited check or a toolchain update falls back to running the check.

The seed holds check names and values only, never paths; the source root is derived from
the seed's own location. ``python -m sandover.buildseed verify <plain build> <seeded build>``
proves a seeded configure equals a plain one (cache, generated headers, compile commands).
"""

from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
import sys
from pathlib import Path

from sandover.paths import root

SEED_REL = Path("sandover") / "cmake" / "configure-seed.cmake"
PRESET = "Release-windows-clang"
# check_type_size caches the size under VAR next to its HAVE_VAR try-compile result
_TYPE_SIZE = "CheckTypeSize.cmake"


def seed_path(src: Path | None = None) -> Path:
    return (src or root()) / SEED_REL


def _unquote(s: str) -> str:
    s = s.strip()
    if s.startswith('"') and s.endswith('"'):
        s = s[1:-1].replace('\\"', '"').replace("\\\\", "\\")
    return s


def parse_log(text: str) -> list[dict]:
    """try_compile-v1 events of a CMakeConfigureLog.yaml: variable, cached, backtrace frames."""
    events, cur, section = [], None, None
    for line in text.splitlines():
        if line.startswith("  -") and line.strip() == "-":
            cur, section = {"frames": []}, None
            events.append(cur)
            continue
        if cur is None:
            continue
        s = line.strip()
        if line.startswith("    ") and not line.startswith("     "):
            key = s.split(":", 1)[0]
            section = key
            if key == "kind":
                cur["kind"] = _unquote(s.split(":", 1)[1])
            continue
        if section == "backtrace" and s.startswith("- "):
            cur["frames"].append(_unquote(s[2:]))
        elif section == "buildResult" and line.startswith("      ") and not line.startswith("       "):
            k, _, v = s.partition(":")
            if k in ("variable", "cached"):
                cur[k] = _unquote(v)
    return [e for e in events if e.get("kind") == "try_compile-v1"]


_FRAME = re.compile(r"^(.*):(\d+) \((\w+)\)$")


def _frame_file(frame: str) -> str | None:
    m = _FRAME.match(frame)
    return m.group(1) if m else None


def _rel(path: str, src: Path) -> str | None:
    """``path`` relative to the source root, or None for a file outside it (CMake modules)."""
    if not Path(path).is_absolute():
        return Path(path).as_posix()  # CMake logs project files relative to the source root
    try:
        return Path(path).resolve().relative_to(src.resolve()).as_posix()
    except ValueError:
        return None


def read_cache(cache: Path) -> dict[str, tuple[str, str]]:
    out = {}
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith(("#", "//")):
            continue
        m = re.match(r"^([^:=]+):([A-Z]+)=(.*)$", line)
        if m:
            out[m.group(1)] = (m.group(2), m.group(3))
    return out


def file_hash(p: Path) -> str:
    # line endings normalised so a CRLF checkout matches the committed seed
    return hashlib.sha256(p.read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def clang_version(compiler: str = "clang") -> str:
    """``clang version X.Y.Z`` without the vendor suffix: two builds of one version match."""
    out = subprocess.run([compiler, "--version"], capture_output=True, text=True).stdout
    m = re.search(r"clang version [^ \n]+", out)
    return m.group(0) if m else ""


def collect(build: Path, src: Path) -> tuple[dict[tuple[str, ...], dict[str, str]], list[str]]:
    """Groups {defining files: {variable: value}} from a finished configure, plus the
    variables left out (path-valued or not in the cache)."""
    cache = read_cache(build / "CMakeCache.txt")
    events = parse_log((build / "CMakeFiles" / "CMakeConfigureLog.yaml").read_text(
        encoding="utf-8", errors="replace"))
    groups: dict[tuple[str, ...], dict[str, str]] = {}
    skipped = []
    for e in events:
        var = e.get("variable")
        if not var or e.get("cached") != "true":
            continue
        files = [_frame_file(f) for f in e["frames"]]
        rel = tuple(sorted({r for f in files if f and (r := _rel(f, src))}))
        names = [var]
        size = var[len("HAVE_"):]
        if any(f and f.endswith(_TYPE_SIZE) for f in files) and var.startswith("HAVE_") and size in cache:
            names.append(size)
        for n in names:
            if n not in cache:
                skipped.append(n)
                continue
            value = cache[n][1]
            if re.search(r"(?<![A-Za-z])[A-Za-z]:[/\\]|^/", value):
                skipped.append(n)
                continue
            groups.setdefault(rel, {})[n] = value
    return groups, skipped


def _q(v: str) -> str:
    return '"' + v.replace("\\", "\\\\").replace('"', '\\"').replace("$", "\\$") + '"'


#: whole-seed gate: the top level sets flags and third-party options every check sees
GATE_FILES = ("CMakeLists.txt", "CMakePresets.json")
_SRC_REF = re.compile(r"\$\{(?:CMAKE_CURRENT_SOURCE_DIR|CMAKE_CURRENT_LIST_DIR|PROJECT_SOURCE_DIR)\}"
                      r"/([\w./-]+\.(?:c|cc|cpp|cxx))")


def check_sources(files: tuple[str, ...], src: Path) -> list[str]:
    """Source files under ``src`` that the defining ``files`` hand to try_compile
    (``${CMAKE_CURRENT_SOURCE_DIR}/x.c`` and the like), resolved from each file's directory up."""
    found = set()
    for f in files:
        try:
            text = (src / f).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for ref in _SRC_REF.findall(text):
            d = (src / f).parent
            while True:
                if (d / ref).is_file():
                    found.add((d / ref).relative_to(src).as_posix())
                    break
                if d == src or src not in d.parents:
                    break
                d = d.parent
    return sorted(found - set(files))


_TOOLSET = (r"MSVC/([0-9.]+)/include", r"Windows Kits/10/Include/([0-9.]+)/")


def toolset(compiler: str = "clang") -> str:
    """MSVC toolset and Windows SDK versions clang searches (versions only), or ''."""
    probe = subprocess.run([compiler, "-E", "-v", "-x", "c", "-"], input="", capture_output=True,
                           text=True).stderr.replace("\\", "/")
    found = [re.search(p, probe) for p in _TOOLSET]
    return " ".join(f"{k} {m.group(1)}" for k, m in zip(("msvc", "sdk"), found) if m)


def render(groups: dict[tuple[str, ...], dict[str, str]], src: Path, cmake_version: str,
           clang: str, tools: str = "") -> str:
    gate = " ".join(f"{_q(f)} {file_hash(src / f)}" for f in GATE_FILES)
    out = [
        "# Generated by `python -m sandover.buildseed harvest`; do not edit by hand.",
        "# Cached try-compile results for `cmake -C` (sandover/kb/howto/build-and-extract.md).",
        "# A group applies only if its defining files hash the same; any mismatch runs the checks.",
        f"set(_seed_cmake {_q(cmake_version)})",
        f"set(_seed_clang {_q(clang)})",
        f"set(_seed_tools {_q(tools)})",
        'get_filename_component(_seed_src "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)',
        "macro(_seed_skip why)",
        '  message(STATUS "configure seed: ${why}; running all checks")',
        "  return()",
        "endmacro()",
        "macro(_seed_group)",
        "  set(_seed_ok TRUE)",
        "  set(_seed_args ${ARGN})",
        "  while(_seed_args)",
        "    list(POP_FRONT _seed_args _f _h)",
        '    if(NOT EXISTS "${_seed_src}/${_f}")',
        "      set(_seed_ok FALSE)",
        "      break()",
        "    endif()",
        '    file(READ "${_seed_src}/${_f}" _seed_txt)',
        '    string(REPLACE "\\r\\n" "\\n" _seed_txt "${_seed_txt}")',
        '    string(SHA256 _seed_h "${_seed_txt}")',
        '    if(NOT _seed_h STREQUAL _h)',
        "      set(_seed_ok FALSE)",
        "      break()",
        "    endif()",
        "  endwhile()",
        "endmacro()",
        # CACHE INTERNAL forces: never overwrite an existing tree's own results
        'if(NOT CMAKE_SCRIPT_MODE_FILE AND EXISTS "${CMAKE_BINARY_DIR}/CMakeCache.txt")',
        '  _seed_skip("existing build tree")',
        "endif()",
        'if(DEFINED CMAKE_BUILD_TYPE AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")',
        '  _seed_skip("build type ${CMAKE_BUILD_TYPE} is not Release")',
        "endif()",
        'set(_seed_cc "clang")',
        "if(DEFINED CMAKE_C_COMPILER)",
        '  set(_seed_cc "${CMAKE_C_COMPILER}")',
        "endif()",
        'get_filename_component(_seed_cc_name "${_seed_cc}" NAME_WE)',
        'if(NOT _seed_cc_name STREQUAL "clang")',
        '  _seed_skip("C compiler ${_seed_cc} is not clang")',
        "endif()",
        "if(NOT CMAKE_VERSION VERSION_EQUAL _seed_cmake)",
        '  _seed_skip("CMake ${CMAKE_VERSION} != ${_seed_cmake}")',
        "endif()",
        'find_program(_seed_cc_path "${_seed_cc}" NO_CACHE)',
        "if(NOT _seed_cc_path)",
        '  _seed_skip("${_seed_cc} not found")',
        "endif()",
        'execute_process(COMMAND "${_seed_cc_path}" --version OUTPUT_VARIABLE _seed_out ERROR_QUIET)',
        'string(REGEX MATCH "clang version [^ \\n]+" _seed_out "${_seed_out}")',
        "if(NOT _seed_out STREQUAL _seed_clang)",
        '  _seed_skip("${_seed_out} != ${_seed_clang}")',
        "endif()",
        "if(CMAKE_HOST_WIN32)",
        "  set(_seed_null NUL)",
        "else()",
        "  set(_seed_null /dev/null)",
        "endif()",
        'execute_process(COMMAND "${_seed_cc_path}" -E -v -x c - INPUT_FILE "${_seed_null}"',
        "  OUTPUT_QUIET ERROR_VARIABLE _seed_probe)",
        'string(REPLACE "\\\\" "/" _seed_probe "${_seed_probe}")',
        'set(_seed_out "")',
        f'string(REGEX MATCH "{_TOOLSET[0]}" _seed_m "${{_seed_probe}}")',
        "if(_seed_m)",
        '  string(APPEND _seed_out "msvc ${CMAKE_MATCH_1}")',
        "endif()",
        f'string(REGEX MATCH "{_TOOLSET[1]}" _seed_m "${{_seed_probe}}")',
        "if(_seed_m)",
        '  string(APPEND _seed_out " sdk ${CMAKE_MATCH_1}")',
        "endif()",
        'string(STRIP "${_seed_out}" _seed_out)',
        "if(NOT _seed_out STREQUAL _seed_tools)",
        '  _seed_skip("toolset \'${_seed_out}\' != \'${_seed_tools}\'")',
        "endif()",
        f"_seed_group({gate})",
        "if(NOT _seed_ok)",
        '  _seed_skip("top-level CMakeLists.txt or CMakePresets.json changed (re-run '
        '`python -m sandover.buildseed harvest`)")',
        "endif()",
        "set(_seed_used 0)",
        "set(_seed_stale 0)",
    ]
    for files, values in sorted(groups.items()):
        keyed = (*files, *check_sources(files, src))
        args = " ".join(f"{_q(f)} {file_hash(src / f)}" for f in keyed)
        out.append(f"_seed_group({args})")
        out.append("if(_seed_ok)")
        for k, v in sorted(values.items()):
            out.append(f"  set({k} {_q(v)} CACHE INTERNAL \"\")")
        out.append(f"  math(EXPR _seed_used \"${{_seed_used}} + {len(values)}\")")
        out.append("else()")
        out.append(f"  math(EXPR _seed_stale \"${{_seed_stale}} + {len(values)}\")")
        out.append("endif()")
    out += [
        'message(STATUS "configure seed: ${_seed_used} cached checks applied, '
        '${_seed_stale} left to run (changed CMake files)")',
        "if(_seed_stale GREATER 0)",
        '  message(STATUS "configure seed: stale; re-run `python -m sandover.buildseed harvest` '
        'from a plain configure")',
        "endif()",
    ]
    return "\n".join(out) + "\n"


def harvest(build: Path, src: Path | None = None, dest: Path | None = None) -> Path:
    src = src or root()
    cache = read_cache(build / "CMakeCache.txt")
    if Path(cache.get("CMAKE_HOME_DIRECTORY", ("", ""))[1]).resolve() != src.resolve():
        raise SystemExit(f"{build} was configured from another source tree")
    if cache.get("CMAKE_BUILD_TYPE", ("", ""))[1] != "Release" or Path(
            cache.get("CMAKE_C_COMPILER", ("", ""))[1]).stem != "clang":
        raise SystemExit(f"{build} is not a {PRESET} configure")
    groups, skipped = collect(build, src)
    cmake_version = ".".join(cache[k][1] for k in (
        "CMAKE_CACHE_MAJOR_VERSION", "CMAKE_CACHE_MINOR_VERSION", "CMAKE_CACHE_PATCH_VERSION"))
    cc = cache["CMAKE_C_COMPILER"][1]
    text = render(groups, src, cmake_version, clang_version(cc), toolset(cc))
    dest = dest or seed_path(src)
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(text, encoding="utf-8", newline="\n")
    n = sum(len(v) for v in groups.values())
    print(f"wrote {n} check results in {len(groups)} groups; left out {len(skipped)}: "
          f"{' '.join(skipped[:10])}", file=sys.stderr)
    return dest


# --- verification: a seeded configure must produce the same build inputs as a plain one ---

_VOLATILE = re.compile(r"^(CMAKE_CACHEFILE_DIR|CMAKE_HOME_DIRECTORY|.*_BINARY_DIR|.*_SOURCE_DIR)$")
# tool locations differ by recipe (scoop shim vs resolved exe), not by what gets compiled
_TOOLS = {"CMAKE_C_COMPILER_LAUNCHER": "<LAUNCHER>", "CMAKE_CXX_COMPILER_LAUNCHER": "<LAUNCHER>",
          "CMAKE_MAKE_PROGRAM": "<NINJA>"}


def _forms(p: str) -> list[str]:
    q = Path(p)
    forms = {p, q.as_posix(), str(q), str(q).replace("\\", "\\\\"), q.as_posix().replace(":", "$:")}
    # CMake records the drive letter in either case
    forms |= {f[0].swapcase() + f[1:] for f in forms if f[1:2] in (":", "$")}
    return sorted(forms, key=len, reverse=True)


def _normaliser(build: Path):
    """Replace the tree's recorded build and source dirs and tool paths with placeholders.
    Paths come from the cache, so a build tree moved after configuring still normalises."""
    c = read_cache(build / "CMakeCache.txt")
    subs = [(f, "<BUILD>") for f in _forms(c["CMAKE_CACHEFILE_DIR"][1])]
    subs += [(f, "<SRC>") for f in _forms(c["CMAKE_HOME_DIRECTORY"][1])]
    for k, tag in _TOOLS.items():
        if k in c and c[k][1]:
            subs += [(f, tag) for f in _forms(c[k][1])]

    def norm(text: str) -> str:
        for a, b in subs:
            text = text.replace(a, b)
        text = re.sub(r"<(LAUNCHER|NINJA)>\.exe", r"<\1>", text)
        # custom-command script ids hash the build dir path
        text = re.sub(r"(\.bat) [0-9a-f]{16}\b", r"\1 <ID>", text)
        # regeneration deps: the worktree's own git HEAD, and CMake's ninja lookup when it ran
        text = re.sub(r"\.git/worktrees/[^/\s]+/", ".git/worktrees/<WT>/", text)
        return re.sub(r" \S+/CMakeNinjaFindMake\.cmake", "", text)
    return c, norm


def _generated(build: Path) -> dict[str, Path]:
    keep = {".h", ".hpp", ".inl", ".c", ".cpp", ".def", ".rc", ".in", ".pc"}
    out = {}
    for p in build.rglob("*"):
        if not p.is_file() or p.suffix not in keep:
            continue
        rel = p.relative_to(build).as_posix()
        if rel.startswith("CMakeFiles/") or "/CMakeFiles/" in rel:
            continue
        out[rel] = p
    return out


def verify(plain: Path, seeded: Path) -> list[str]:
    """Differences between two configures of the same commit (empty list = equivalent).
    Compares cache values, generated sources/headers, build.ninja and every compile command,
    with each tree's own source, build and tool paths normalised away."""
    diffs = []
    ca, na = _normaliser(plain)
    cb, nb = _normaliser(seeded)
    for k in sorted(set(ca) | set(cb)):
        if _VOLATILE.match(k) or k.endswith("-ADVANCED"):
            continue
        va, vb = na(ca.get(k, ("", "<missing>"))[1]), nb(cb.get(k, ("", "<missing>"))[1])
        if va != vb:
            diffs.append(f"cache {k}: {va!r} != {vb!r}")
    ga, gb = _generated(plain), _generated(seeded)
    for rel in sorted(set(ga) | set(gb)):
        if rel not in ga or rel not in gb:
            diffs.append(f"generated {rel}: only in {'plain' if rel in ga else 'seeded'}")
            continue
        if na(ga[rel].read_text(encoding="utf-8", errors="replace")) != nb(
                gb[rel].read_text(encoding="utf-8", errors="replace")):
            diffs.append(f"generated {rel} differs")
    for name in ("compile_commands.json", "build.ninja"):
        la = na((plain / name).read_text(encoding="utf-8", errors="replace")).splitlines()
        lb = nb((seeded / name).read_text(encoding="utf-8", errors="replace")).splitlines()
        if la != lb:
            bad = next((i for i, (x, y) in enumerate(zip(la, lb)) if x != y), min(len(la), len(lb)))
            diffs.append(f"{name} differs at line {bad + 1}: {la[bad][:160] if bad < len(la) else ''!r} "
                         f"!= {lb[bad][:160] if bad < len(lb) else ''!r}")
    return diffs


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="python -m sandover.buildseed", description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    h = sub.add_parser("harvest", help="write the seed from a finished plain configure")
    h.add_argument("build", type=Path, help="build dir of a configure run WITHOUT the seed")
    v = sub.add_parser("verify", help="compare a plain and a seeded configure of one commit")
    v.add_argument("plain", type=Path)
    v.add_argument("seeded", type=Path)
    a = ap.parse_args(argv)
    if a.cmd == "harvest":
        src = Path(read_cache(a.build / "CMakeCache.txt")["CMAKE_HOME_DIRECTORY"][1])
        print(harvest(a.build, src))
        return 0
    diffs = verify(a.plain, a.seeded)
    for d in diffs:
        print(d)
    print("equivalent" if not diffs else f"{len(diffs)} differences")
    return 1 if diffs else 0


if __name__ == "__main__":
    sys.exit(main())
