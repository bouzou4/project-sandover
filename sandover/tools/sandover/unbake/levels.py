"""Per-level slot roles for the fit (see sandover/kb/facts/mood.md for sources)."""

from __future__ import annotations

from dataclasses import dataclass, field

# category: cycle = hour-table blend runs; quick = update-mood-quick pins slot 0 (no day cycle);
# bespoke = level-specific code rewrites tables or crossfades contexts every frame.


@dataclass
class LevelRoles:
    category: str
    sun: list[int]
    effects: dict[int, str] = field(default_factory=dict)
    note: str = ""


V1_SUN = [0, 1, 2, 3, 4, 6, 7]

LEVELS: dict[str, LevelRoles] = {
    "village1": LevelRoles("cycle", V1_SUN, {5: "flames"}),
    "beach": LevelRoles("cycle", V1_SUN, {5: "flames"}, "uses update-mood-village1"),
    "jungle": LevelRoles("cycle", V1_SUN, {5: "light"}),
    "training": LevelRoles("cycle", V1_SUN, {5: "light"}),
    "misty": LevelRoles("cycle", [0, 1], {2: "flames", 3: "flames", 4: "flames", 5: "flames", 6: "light", 7: "light"}),
    "village2": LevelRoles("cycle", [0, 1], {2: "lightning", 3: "lightning", 4: "flames", 5: "flames", 6: "flames", 7: "flames"}),
    "swamp": LevelRoles("cycle", [0, 1], {2: "lightning", 3: "lightning", 4: "flames", 5: "flames", 6: "flames", 7: "flames"}),
    "sunken": LevelRoles("cycle", [0, 1], {2: "light", 3: "fade", 4: "caustics", 5: "caustics", 6: "caustics", 7: "caustics"}),
    "sunkenb": LevelRoles("cycle", [0, 1], {2: "light", 3: "fade", 4: "caustics", 5: "caustics", 6: "caustics", 7: "caustics"}),
    "rolling": LevelRoles("cycle", [0, 1, 2, 3], {4: "light", 5: "light", 6: "light", 7: "light"},
                          "lightning call passes #t: sky only, slots 2-3 stay sun"),
    "firecanyon": LevelRoles("cycle", [0, 1, 2, 3], {4: "lava", 5: "lava", 6: "lava", 7: "lava"}),
    "snow": LevelRoles("cycle", [0, 1, 2, 3], {4: "flames", 5: "flames", 6: "flames", 7: "light"}),
    "ogre": LevelRoles("bespoke", [0, 1, 2, 3], {4: "lightning", 5: "lightning"}, "3-context distance crossfade"),
    "village3": LevelRoles("bespoke", [0, 1, 2, 3], {4: "flames", 5: "flames", 6: "flames"},
                           "slots 0-3 mood-lights rewritten every frame"),
    "finalboss": LevelRoles("bespoke", V1_SUN, {}, "second context crossfade"),
    "maincave": LevelRoles("quick", [0], {4: "flames", 5: "flames", 6: "flames", 7: "flames"}),
    "robocave": LevelRoles("quick", [0], {4: "flames", 5: "flames", 6: "flames", 7: "flames"}),
    "darkcave": LevelRoles("quick", [0], {s: "fade-lights" for s in range(1, 8)}),
    "jungleb": LevelRoles("quick", [0], {}),
    "lavatube": LevelRoles("quick", [0], {1: "light", 4: "lava", 5: "lava", 6: "lava", 7: "lava"}),
    "citadel": LevelRoles("quick", [0], {1: "light", 4: "flames", 5: "flames", 6: "flames"}),
}

POINT_LIGHT_KINDS = {"flames", "light", "lava", "fade-lights"}
