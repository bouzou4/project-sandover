"""The bridge table registers 72 unique symbols, each declared and defined, scoped as before."""
import re
from pathlib import Path

from sandover.bridge_sources import bridge_files

REPO = Path(__file__).resolve().parents[3]
SD = REPO / "game" / "kernel" / "common" / "sandover"
ENTRY = re.compile(r'\{"([^"]+)", \(void\*\)(\w+), BridgeScope::(AllGames|Jak1)\}')


def table():
    return ENTRY.findall((SD / "bridge.cpp").read_text(encoding="utf-8"))


def test_72_unique_symbols_in_order_with_xr_rate_first():
    t = table()
    assert len(t) == 72
    assert len({n for n, _, _ in t}) == 72
    assert t[0] == ("pc-get-xr-game-rate", "pc_get_xr_game_rate", "AllGames")
    assert {s for _, _, s in t[1:]} == {"Jak1"}


def test_every_entry_declared_and_defined_once():
    hdr = (SD / "bridge.h").read_text(encoding="utf-8")
    src = "\n".join(p.read_text(encoding="utf-8") for p in bridge_files(REPO)[1:-1])
    for _, fn, _ in table():
        assert re.search(rf"^[\w ]+\b{fn}\(.*\);$", hdr, re.M), fn
        assert len(re.findall(rf"^[\w ]+\b{fn}\(.*\) \{{$", src, re.M)) == 1, fn


def test_kmachine_registers_both_scopes_in_place():
    km = (REPO / "game" / "kernel" / "common" / "kmachine.cpp").read_text(encoding="utf-8")
    assert km.count("register_sandover_bridge(") == 2
    assert "BridgeScope::AllGames" in km and "BridgeScope::Jak1" in km
