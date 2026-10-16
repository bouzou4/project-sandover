"""Build a shareable Sandover zip from the current staged build and this machine's derived data.

The zip holds the engine binaries (``gk``, ``goalc``, ``extractor`` and their libraries plus the
app-local C++ runtime), the source tree the extractor compiles (``goal_src``, decompiler config,
assets, shaders, presets), the remaster's derived data (material table, PBR maps, canopy maps,
``.unbake`` lighting sidecars), the five Jak 3 sky sprites and ``install.ps1``. Nothing from the
user's disc is copied (``iso_data``, ``decompiler_out``, the fr3 and CGO files): the installer runs
the extractor on the user's own image.

Layout inside the zip (the upstream release layout, so ``gk`` finds ``data/`` on its own; a
release zip, ``--version``, has no ``sandover-<sha>/`` folder, since the launcher extracts it as
is and runs ``extractor`` from the mod folder)::

    sandover-<sha>/
      gk.exe goalc.exe extractor.exe *.dll Install.cmd install.ps1 README.txt LICENSE build.json
      data/goal_src/...  data/decompiler/config/jak1/...  data/game/...  data/custom_assets/jak1/...
      data/sandover/presets/*.toml
      data/sandover/data/materials/{materials.csv,emissive-actors.csv,pbr-maps/}
      data/sandover/data/sky/jak3/*.png  (Jak 3's sky sprites, the sky bodies)
      data/sandover/data/lighting/{*.canopy,canopy/*.canopy,*.unbake}  (gk reads the sidecars here)

CLI::

    sandover-package build [--bin NAME | --build DIR] [--version X.Y.Z] [--out DIR] [--allow-skew]
    sandover-package sizes          # what would ship, per data set, without writing a zip

Release (the launcher mod-source asset, ``sandover/release/mod-source.json``)::

    cmake --preset Release-windows-clang-package
    cmake --build out/build/Package --target gk goalc extractor
    sandover-package build --build out/build/Package/bin --version 0.9.0
    # -> sandover-v0.9.0-windows.zip, uploaded to the GitHub release v0.9.0
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import subprocess
import sys
import zipfile
from dataclasses import dataclass, field
from pathlib import Path

from sandover import engine
from sandover.paths import data_dir, root

FILES = Path(__file__).with_name("package_files")
# tracked source the extractor and gk read from the project dir (gk lists goal_src/user at boot)
TRACKED = (
    "goal_src/goal-lib.gc",
    "goal_src/goos-lib.gs",
    "goal_src/common",
    "goal_src/jak1",
    "goal_src/user",
    "decompiler/config/jak1",
    "game/assets/fonts",
    "game/assets/jak1",
    "game/assets/sdl_controller_db.txt",
    "game/graphics/opengl_renderer/shaders",
    "custom_assets/jak1",
    "sandover/presets",
)
CRT = ("msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll", "vcruntime140.dll", "vcruntime140_1.dll")
# build-tree files staging does not copy (OpenAL Soft's LGPL notices, the objects-mode config)
BUILD_EXTRAS = ("OpenAL-Soft-*.txt", "sandover-alsoft-objects.ini")
# extensions that are already compressed
STORED = {".png", ".zip", ".zst"}


@dataclass
class Entry:
    """One file of the package: ``src`` on disk, ``arc`` its path under the package folder."""

    src: Path
    arc: str
    group: str


@dataclass
class Plan:
    entries: list[Entry] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)

    def add(self, src: Path, arc: str, group: str) -> None:
        self.entries.append(Entry(src, arc.replace("\\", "/"), group))

    def add_tree(self, src: Path, arc: str, group: str, pattern: str = "**/*") -> None:
        for f in sorted(src.glob(pattern)):
            if f.is_file():
                self.add(f, f"{arc}/{f.relative_to(src).as_posix()}", group)

    def sizes(self) -> dict[str, tuple[int, int]]:
        """Per group: (file count, bytes)."""
        out: dict[str, tuple[int, int]] = {}
        for e in self.entries:
            n, b = out.get(e.group, (0, 0))
            out[e.group] = (n + 1, b + e.src.stat().st_size)
        return out


def tracked_files(repo: Path, paths: tuple[str, ...] = TRACKED) -> list[str]:
    """Paths under ``paths`` that git tracks in ``repo`` (untracked scratch files never ship)."""
    out = subprocess.run(["git", "-C", str(repo), "ls-files", "-z", "--", *paths],
                         capture_output=True, text=True, check=True).stdout
    return sorted(p for p in out.split("\0") if p)


def crt_dir() -> Path | None:
    """The newest x64 Visual C++ runtime folder of an installed Visual Studio, if any."""
    pats = [os.path.join(os.environ.get(v, ""), "Microsoft Visual Studio", "*", "*", "VC", "Redist",
                         "MSVC", "*", "x64", "Microsoft.VC14*.CRT")
            for v in ("ProgramFiles", "ProgramFiles(x86)")]
    dirs = sorted({Path(d) for p in pats for d in glob.glob(p)}, key=lambda d: d.parts[-3])
    return dirs[-1] if dirs else None


def pick_bin(name: str | None) -> Path:
    if name:
        d = data_dir() / "bin" / name
        if not (d / engine.DONE).exists():
            raise SystemExit(f"{d} is not a complete staged build")
        return d
    d = engine.latest_staged()
    if d is None:
        raise SystemExit("no staged build under $SANDOVER_DATA/bin; run a play or rig session first")
    return d


def is_staged(bin_dir: Path) -> bool:
    return (bin_dir / engine.DONE).exists()


def bin_info(bin_dir: Path) -> dict:
    """``build.json`` of a staged copy; for a build tree's ``bin`` (``--build``), the same record
    computed from the checkout (:func:`engine.source_state`)."""
    if is_staged(bin_dir):
        return engine.build_info(bin_dir) or {}
    return engine.source_state(bin_dir / f"gk{engine.EXE}")


def build_dir(bin_dir: Path) -> Path:
    """Where the extractor comes from: the dev build for a staged copy, else the build tree itself."""
    return engine.build_bin() if is_staged(bin_dir) else bin_dir


def check_skew(bin_dir: Path, repo: Path) -> str | None:
    """Why the staged binaries (and the extractor next to the build) may not match the checkout's
    shaders and GOAL, or None."""
    info = bin_info(bin_dir)
    sha = info.get("engine_sha")
    if not sha:
        return f"{bin_dir.name} has no trusted engine commit ({info.get('reason') or 'no build.json'})"
    head = subprocess.run(["git", "-C", str(repo), "log", "-1", "--format=%h", "HEAD", "--", ".",
                           ":(exclude)sandover"], capture_output=True, text=True).stdout.strip()
    if head and not (head.startswith(sha) or sha.startswith(head)):
        return f"{bin_dir.name} was built from {sha}, the checkout's engine commit is {head}"
    return check_extractor(bin_dir, repo)


EXTRACTOR_SRC = "decompiler/extractor"


def check_extractor(bin_dir: Path, repo: Path) -> str | None:
    """Why the build folder's extractor may not belong with the staged ``gk``, or None. Its code
    lives in the shared libraries, so those must be byte-identical to the staged copies, and the
    exe must be newer than its own sources."""
    src = build_dir(bin_dir)
    ext = src / f"extractor{engine.EXE}"
    if not ext.exists():
        return f"{ext.name} not found in the build folder; build the extractor target"
    for dll in sorted(bin_dir.glob(f"*{engine.LIB}")) if src != bin_dir else []:
        live = src / dll.name
        if not live.exists() or live.read_bytes() != dll.read_bytes():
            return f"{dll.name} in the build folder differs from {bin_dir.name}; the extractor would not match gk"
    built = ext.stat().st_mtime
    for rel in tracked_files(repo, (EXTRACTOR_SRC,)):
        if (repo / rel).stat().st_mtime > built:
            return f"{rel} is newer than {ext.name}; rebuild the extractor"
    return None


def dirty_source(repo: Path, paths: tuple[str, ...] = TRACKED) -> list[str]:
    """Shipped source files with uncommitted edits (they would ship under a clean commit)."""
    out = subprocess.run(["git", "-C", str(repo), "status", "--porcelain", "--untracked-files=no",
                          "--", *paths], capture_output=True, text=True, check=True).stdout
    return [line[3:] for line in out.splitlines() if line.strip()]


# Jak 3's sky sprites the sky bodies read (SunDisc.cpp make_body_assets)
SKY_SPRITES = ("full-moon.png", "sky-glow-soft.png", "glow.png", "glow-soft.png", "sky-hotdot.png")


def plan(bin_dir: Path, repo: Path | None = None, data: Path | None = None) -> Plan:
    """Everything the package holds, grouped for the size report."""
    repo = repo or root()
    data = data or data_dir()
    p = Plan()
    build = build_dir(bin_dir)
    ships = ("gk", "goalc") if build == bin_dir else None  # a build tree also holds tests and tools
    for f in sorted(bin_dir.iterdir()):
        if f.suffix.lower() == ".dll" or (f.suffix.lower() == ".exe" and (ships is None or f.stem in ships)):
            p.add(f, f.name, "binaries")
    ext = build / f"extractor{engine.EXE}"
    if not ext.exists():
        raise SystemExit(f"{ext.name} not found in the build folder; build the extractor target")
    p.add(ext, ext.name, "binaries")
    for pat in BUILD_EXTRAS:
        for f in sorted(build.glob(pat)):
            p.add(f, f.name, "binaries")
    if needs_crt([e.src for e in p.entries if e.src.suffix.lower() in (".exe", ".dll")]):
        crt = crt_dir()
        if crt is None:
            p.warnings.append("no Visual C++ runtime folder found; the player needs the VC++ redistributable")
        for name in CRT if crt else ():
            if (crt / name).exists():
                p.add(crt / name, name, "binaries")
    p.add(repo / "LICENSE", "LICENSE", "source")
    for rel in tracked_files(repo):
        p.add(repo / rel, f"data/{rel}", "source")
    p.add(repo / ".github/scripts/releases/error-code-metadata.json",
          "data/launcher/error-code-metadata.json", "source")
    mat = data / "materials"
    for name in ("materials.csv", "emissive-actors.csv"):
        if (mat / name).exists():
            p.add(mat / name, f"data/sandover/data/materials/{name}", "materials")
        else:
            p.warnings.append(f"{mat / name} missing")
    p.add_tree(mat / "pbr-maps", "data/sandover/data/materials/pbr-maps", "pbr-maps")
    sky = data / "sky" / "jak3"
    p.add_tree(sky, "data/sandover/data/sky/jak3", "sky", "*.png")
    for name in SKY_SPRITES:
        if not (sky / name).is_file():
            p.warnings.append(f"{sky / name} missing: the sky bodies fall back to the sun disc")
    light = data / "lighting"
    p.add_tree(light, "data/sandover/data/lighting", "canopy", "*.canopy")
    p.add_tree(light / "canopy", "data/sandover/data/lighting/canopy", "canopy", "*.canopy")
    p.add_tree(repo / "out" / "jak1" / "fr3", "data/sandover/data/lighting", "unbake", "*.unbake")
    for name in ("install.ps1", "Install.cmd", "README.txt"):
        p.add(FILES / name, name, "installer")
    if not any(e.group == "unbake" for e in p.entries):
        p.warnings.append("no .unbake sidecars in out/jak1/fr3")
    return p


def needs_crt(binaries: list[Path]) -> bool:
    """Whether any binary imports the shared VC++ runtime (a static-runtime build does not)."""
    return any(b"vcruntime140" in f.read_bytes().lower() for f in binaries)


def write_zip(p: Plan, out: Path, top: str, info: dict) -> Path:
    """Every entry goes under ``top``; an empty ``top`` makes a flat (release) zip."""
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(".zip.tmp")
    pre = f"{top}/" if top else ""
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED, compresslevel=6, allowZip64=True) as z:
        for e in p.entries:
            kind = zipfile.ZIP_STORED if e.src.suffix.lower() in STORED else zipfile.ZIP_DEFLATED
            z.write(e.src, f"{pre}{e.arc}", compress_type=kind)
        z.writestr(f"{pre}build.json", json.dumps(info, indent=2) + "\n")
        z.writestr(f"{pre}data/log/.keep", "")
    tmp.replace(out)
    return out


def _mb(b: int) -> str:
    return f"{b / 1e6:,.1f} MB"


def report(p: Plan) -> str:
    rows = [f"{g:<10} {n:>6} files {_mb(b):>12}" for g, (n, b) in sorted(p.sizes().items())]
    total = sum(b for _, b in p.sizes().values())
    return "\n".join(rows + [f"{'total':<10} {len(p.entries):>6} files {_mb(total):>12}"])


def release_version(v: str) -> str:
    """``0.9.0`` or ``v0.9.0`` -> ``0.9.0``; anything else is refused, so the zip name matches the mod source."""
    v = v[1:] if v.startswith("v") else v
    if not re.fullmatch(r"\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?", v):
        raise SystemExit(f"--version must be X.Y.Z (optionally -suffix), got {v!r}")
    return v


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="sandover-package", description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("build", "sizes"):
        s = sub.add_parser(name)
        g = s.add_mutually_exclusive_group()
        g.add_argument("--bin", help="staged build under $SANDOVER_DATA/bin (default: newest auto-staged)")
        g.add_argument("--build", type=Path,
                       help="a build tree's bin folder instead (the release: out/build/Package/bin)")
        s.add_argument("--allow-skew", action="store_true",
                       help="package even if the binaries were built from another engine commit")
        if name == "build":
            s.add_argument("--out", type=Path, default=None,
                           help="output folder (default: $SANDOVER_DATA/package)")
            s.add_argument("--version", help="release version: a flat zip named sandover-v<version>-windows.zip, "
                                             "the layout the launcher expects")
    a = ap.parse_args(argv)
    version = release_version(a.version) if getattr(a, "version", None) else None
    bin_dir = a.build.resolve() if a.build else pick_bin(a.bin)
    if a.build and not (bin_dir / f"gk{engine.EXE}").exists():
        raise SystemExit(f"no gk{engine.EXE} in {bin_dir}")
    dirty = dirty_source(root())
    if dirty:
        print("refusing: shipped source has uncommitted edits: " + ", ".join(dirty[:10]), file=sys.stderr)
        return 1
    skew = check_skew(bin_dir, root())
    if skew and not a.allow_skew:
        print(f"refusing: {skew} (shaders and GOAL come from the checkout; --allow-skew to override)",
              file=sys.stderr)
        return 1
    p = plan(bin_dir)
    for w in p.warnings + ([skew] if skew else []):
        print(f"WARNING: {w}", file=sys.stderr)
    print(f"bin {bin_dir.name}")
    print(report(p))
    if a.cmd == "sizes":
        return 0
    info = {"schema": 1, "bin": bin_dir.name, **bin_info(bin_dir),
            "sizes": {g: b for g, (_, b) in p.sizes().items()}}
    if version:
        info["version"] = version
        name, top = f"sandover-v{version}-windows", ""
    else:
        name = top = f"sandover-{info.get('engine_sha') or 'dev'}"
    out = write_zip(p, (a.out or data_dir() / "package") / f"{name}.zip", top, info)
    print(f"wrote {out} ({_mb(out.stat().st_size)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
