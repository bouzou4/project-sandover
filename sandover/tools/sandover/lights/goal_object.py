"""Read data out of Jak 1 linked object files (v4, the ``*-ag.go`` art groups) without the
decompiler: the symbol and type links of every word, so a structure can be read by its fields.

Only what the dark eco pool table needs: art group members by type, their names, res-lump tags
(``texture-bucket``) and the merc effect extras (``mei-ripple``, ``mei-envmap-tint``). Field
offsets are the ``deftype`` layouts in ``goal_src/jak1`` (``art-h.gc``, ``merc-h.gc``, ``res-h.gc``),
counted from the object's type word.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class LinkedObject:
    code: bytes
    syms: dict[int, str] = field(default_factory=dict)  # word offset -> linked symbol or type name

    def u32(self, off: int) -> int:
        return struct.unpack_from("<I", self.code, off)[0]

    def f32(self, off: int) -> float:
        return struct.unpack_from("<f", self.code, off)[0]

    def string(self, ptr: int) -> str:
        n = self.u32(ptr)
        return self.code[ptr + 4:ptr + 4 + n].decode(errors="replace")

    def objects(self, type_name: str) -> list[int]:
        """Offsets of the type words of every object of ``type_name``."""
        return [o for o, n in sorted(self.syms.items()) if n == type_name]


def _seek(data: bytes, lp: int) -> tuple[int, int]:
    """One v2 symbol-link seek: variable length, the low two bits say how many bytes follow."""
    v = data[lp]
    seek, nxt = v, lp + 1
    if seek & 3:
        seek, nxt = (data[lp + 1] << 8) | v, lp + 2
        if seek & 2:
            seek, nxt = (data[lp + 2] << 16) | seek, lp + 3
            if seek & 1:
                seek, nxt = (data[lp + 3] << 24) | seek, lp + 4
    return seek & 0xFFFFFFFC, nxt


def parse(data: bytes) -> LinkedObject:
    """Parse a v4 object (header, data, then v2 link data), as ``link_v2_or_v4`` in
    ``decompiler/ObjectFile/LinkedObjectFileCreation.cpp`` does."""
    _tag, _length, version, code_size = struct.unpack_from("<4I", data, 0)
    if version != 4:
        raise ValueError(f"not a v4 object (version {version})")
    code = data[16:16 + code_size]
    lp = 16 + code_size + 12
    # pointer section: alternating seek / fix counts, 0xff continues, 0 ends
    if data[lp] == 0:
        lp += 1
    else:
        fixing = False
        while True:
            while True:
                c = data[lp]
                lp += 1
                if c != 0xFF:
                    break
                if data[lp] == 0:
                    lp += 1
                    fixing = not fixing
            fixing = not fixing
            if data[lp] == 0:
                break
        lp += 1
    obj = LinkedObject(code)
    while lp < len(data) and data[lp] != 0:
        if data[lp] & 0x80:
            lp += 1  # type link: method count byte
        end = data.index(b"\0", lp)
        name = data[lp:end].decode()
        lp = end + 1
        cp = 0
        while True:
            seek, lp = _seek(data, lp)
            cp += seek
            obj.syms[cp] = name
            if data[lp] == 0:
                break
        lp += 1
    return obj


def load(path: Path) -> LinkedObject:
    return parse(path.read_bytes())


# art (art-h.gc): name 8, length 12, extra 16; merc-ctrl: header at 32 (80 bytes), effects at 112
ART_NAME, ART_EXTRA = 8, 16
MERC_HEADER, MERC_EFFECTS, MERC_EFFECT_SIZE = 32, 112, 32
# res-lump (res-h.gc): length 4, data-base 12, tag 28; res-tag 16 bytes: name 0, elt-type 8,
# data-offset 12 (u16)
_RES_FMT = {"int8": "<b", "uint8": "<B", "int16": "<h", "uint16": "<H", "int32": "<i", "uint32": "<I",
            "float": "<f"}


def art_name(obj: LinkedObject, t: int) -> str:
    return obj.string(obj.u32(t + ART_NAME))


def res_tags(obj: LinkedObject, t: int) -> dict[str, object]:
    """``{tag name: first value}`` of the res-lump in an art object's ``extra`` (empty when it
    is ``#f``, as most pool art is)."""
    lump = obj.u32(t + ART_EXTRA)
    if obj.syms.get(lump - 4) != "res-lump":
        return {}
    base, count, tags = obj.u32(lump + 8), obj.u32(lump), obj.u32(lump + 24)
    out = {}
    for i in range(count):
        to = tags + 16 * i
        etype = obj.syms.get(to + 8, "?")
        off = struct.unpack_from("<H", obj.code, to + 12)[0]
        fmt = _RES_FMT.get(etype)
        out[obj.syms.get(to, "?")] = struct.unpack_from(fmt, obj.code, base + off)[0] if fmt else None
    return out


@dataclass
class MercEffect:
    bits: int
    envmap: bool
    ripple: tuple[float, float, float, float] | None  # x-base, z-base, grid-size, angle
    envmap_tint: tuple[int, int, int] | None  # r g b of mei-envmap-tint (128 = 1.0)


def merc_effects(obj: LinkedObject, t: int) -> list[MercEffect]:
    """Effects of a merc-ctrl and their ripple / envmap extras (merc-h.gc ``merc-effect``)."""
    count = obj.u32(t + MERC_HEADER + 24)
    out = []
    for e in range(count):
        eo = t + MERC_EFFECTS + MERC_EFFECT_SIZE * e
        bits, env = obj.code[eo + 17], obj.code[eo + 27]
        extra = obj.u32(eo + 28)
        env_off, _shader, _scroll, rip_off = obj.code[extra:extra + 4]
        ripple = struct.unpack_from("<4f", obj.code, extra + 16 * rip_off) if bits & 4 else None
        tint = None
        if env:
            r, g, b = obj.code[extra + 16 * env_off + 8:extra + 16 * env_off + 11]
            tint = (r, g, b)
        out.append(MercEffect(bits, bool(env), ripple, tint))
    return out
