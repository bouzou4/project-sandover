"""Condense <level>-summary.json files into the markdown tables used in the unbake finding."""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path

from .levels import LEVELS

STRONG_U8 = 40.0  # slots whose fitted sun is dimmer than this carry too little signal to locate it


def load(out: Path) -> list[dict]:
    rows = []
    for lvl in LEVELS:
        p = out / f"{lvl}-summary.json"
        if p.exists():
            rows.append(json.loads(p.read_text()))
    return rows


def fmt_angles(r: dict, key: str) -> str:
    return " ".join(f"{s['slot']}:{s[key]:.0f}" for s in r["slots"] if key in s)


def sun_table(rows: list[dict]) -> str:
    out = [("| level | category | sun slots: angle to table (deg) | prior-free vs table-init, strong slots (deg, max, sign-folded) "
            "| median / p90 u8 (no occl.) | median / p90 u8 (with v) | R2 (with v) | verts needing occlusion "
            "| tfrag / tie / shrub median (with v) |"),
           "|---|---|---|---|---|---|---|---|---|"]
    for r in rows:
        strong = [s for s in r["slots"] if "angle_blind_vs_table_init_deg" in s and sum(s["lgt_fit_u8"]) / 3 >= STRONG_U8]
        blind = [min(s["angle_blind_vs_table_init_deg"], 180 - s["angle_blind_vs_table_init_deg"]) for s in strong]
        k = r["kinds"]
        kinds = " / ".join(f"{k[t]['vis']['median']:.1f}" if t in k else "-" for t in ("tfrag", "tie", "shrub"))
        b = f"{max(blind):.1f}" if blind else "-"
        out.append(f"| {r['level']} | {r['category']} | {fmt_angles(r, 'angle_table_deg')} | {b} | "
                   f"{r['overall_novis']['median']:.1f} / {r['overall_novis']['p90']:.0f} | "
                   f"{r['overall_vis']['median']:.1f} / {r['overall_vis']['p90']:.0f} | {r['overall_vis']['r2']:.2f} | "
                   f"{r['vertices_needing_occlusion_frac']:.0%} | {kinds} |")
    return "\n".join(out)


def effect_table(rows: list[dict]) -> str:
    out = [("| level | slot | role | lights | lit verts | ambient-only median / p90 on lit "
            "| inv-sq median / p90 on lit | windowed median / p90 on lit |"),
           "|---|---|---|---|---|---|---|---|"]
    for r in rows:
        for e in r["effects"]:
            if "n_lights" not in e:
                out.append(f"| {r['level']} | {e['slot']} | {e['role']} | not modeled | | | | |")
                continue

            def mp(d):
                return f"{d['median']:.0f} / {d['p90']:.0f}" if d else "-"

            inv = e.get("invsq", {}).get("err_lit")
            win = e.get("windowed", {}).get("err_lit")
            out.append(f"| {r['level']} | {e['slot']} | {e['role']} | {e['n_lights']} | {e.get('lit_frac', 0):.1%} | "
                       f"{mp(e.get('baseline_ambient_only_lit'))} | {mp(inv)} | {mp(win)} |")
    return "\n".join(out)


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else Path(os.environ.get("SANDOVER_DATA", "sandover/data")) / "unbake")
    rows = load(out)
    print(sun_table(rows))
    print()
    print(effect_table(rows))


if __name__ == "__main__":
    main()
