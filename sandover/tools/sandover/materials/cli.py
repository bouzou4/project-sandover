"""``sandover-materials``: sheets, prompt, merge, load, spotcheck, apply, summary, emissive-table, emissive-maps, derive-maps, emitter-score."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from sandover.materials import (actor_emissive, emissive_maps, emissive_table, emitter_score, judge,
                                pbr_maps, sheets, table)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="sandover-materials", description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("sheets", help="write manifest.csv and the numbered judge sheets")
    p = sub.add_parser("prompt", help="print the vision judge prompt for one batch")
    p.add_argument("--pass", dest="pass_id", choices=judge.PASSES, required=True)
    p.add_argument("--batch", type=int, required=True)
    sub.add_parser("status", help="list missing judgements per pass")
    sub.add_parser("merge", help="combine both passes into materials.csv")
    sub.add_parser("refresh-levels", help="update the levels column from texture-levels.json")
    sub.add_parser("load", help="push labels into FiftyOne and save the needs-review view")
    s = sub.add_parser("spotcheck", help="stratified sample for the maintainer, with sheets and CSV")
    s.add_argument("-n", type=int, default=100)
    a = sub.add_parser("apply", help="fold human corrections back into materials.csv")
    src = a.add_mutually_exclusive_group(required=True)
    src.add_argument("--csv", type=Path, help="CSV with combo_id and human_class columns")
    src.add_argument("--fiftyone", action="store_true", help="read class:<name> sample tags")
    r = sub.add_parser("rejudge", help="re-run the vision judge on the needs-review set")
    r.add_argument("--with-notes", action="store_true",
                    help="inject human spot-check notes into the prompt as guidance")
    r.add_argument("--limit", type=int, default=None)
    m = sub.add_parser("summary", help="distribution, agreement and confusions")
    m.add_argument("--markdown", type=Path)
    sub.add_parser("emissive-table", help="precompute per-texture E from the sidecars (Browse tab); "
                    "see `sandover-materials emissive-table --help`")
    sub.add_parser("emissive-maps", help="rasterize per-vertex E into per-texture emissive maps; "
                    "see `sandover-materials emissive-maps --help`")
    sub.add_parser("derive-maps", help="per-class normal/roughness maps from each albedo; "
                    "see `sandover-materials derive-maps --help`")

    sub.add_parser("emitter-score", help="score emissive textures as emitters vs lit receivers; "
                    "see `sandover-materials emitter-score --help`")
    sub.add_parser("actor-emissive", help="E for sprite/particle/merc textures (engine table); "
                    "see `sandover-materials actor-emissive --help`")

    argv = sys.argv[1:] if argv is None else argv
    if argv and argv[0] == "emissive-table":
        return emissive_table.main(argv[1:])
    if argv and argv[0] == "emissive-maps":
        return emissive_maps.main(argv[1:])
    if argv and argv[0] == "derive-maps":
        return pbr_maps.main(argv[1:])
    if argv and argv[0] == "emitter-score":
        return emitter_score.main(argv[1:])
    if argv and argv[0] == "actor-emissive":
        return actor_emissive.main(argv[1:])
    args = ap.parse_args(argv)

    if args.cmd == "sheets":
        rows = sheets.build_manifest()
        sheets.write_csv(sheets.manifest_path(), rows, sheets.MANIFEST_FIELDS)
        out = sheets.render_all(rows, sheets.materials_dir() / "sheets")
        print(f"{len(rows)} textures, {len(out)} sheets under {out[0].parent}")
    elif args.cmd == "prompt":
        n = len(sheets.read_csv(sheets.manifest_path()))
        print(judge.prompt(args.pass_id, args.batch, n))
    elif args.cmd == "status":
        table.print_status()
    elif args.cmd == "merge":
        table.merge()
    elif args.cmd == "refresh-levels":
        table.refresh_levels()
    elif args.cmd == "load":
        table.load_fiftyone()
    elif args.cmd == "spotcheck":
        table.spotcheck(args.n)
    elif args.cmd == "apply":
        table.apply_corrections(args.csv, from_fiftyone=args.fiftyone)
    elif args.cmd == "rejudge":
        print(table.rejudge(args.with_notes, args.limit))
    elif args.cmd == "summary":
        md = table.summary()
        print(md)
        if args.markdown:
            args.markdown.write_text(md, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
