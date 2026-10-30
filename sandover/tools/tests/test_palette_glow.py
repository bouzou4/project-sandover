"""The numpy reference for PaletteGlow.cpp gives the values test/test_palette_glow.cpp expects."""
import pytest

from sandover.unbake.palette_glow import OGRE_A, OGRE_L, entry, level_splits


@pytest.mark.parametrize("colour, want", [((208, 107, 107), 120.41), ((132, 122, 122), 108.27),
                                          ((255, 90, 90), 122.54), ((255, 255, 255), 0.0)])
def test_ogre_constant_entries(colour, want):
    assert entry([colour] * 4, OGRE_A, OGRE_L) == pytest.approx(want, abs=0.01)


def test_level_gate():
    village3_l = [[436.2, 192.0, 28.6], [451.9, 157.7, 2.3], [446.2, 185.1, 31.0], [478.0, 191.1, 21.1]]
    assert level_splits(OGRE_L)
    assert not level_splits(village3_l)
