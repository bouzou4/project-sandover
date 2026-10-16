#include <vector>

#include "game/graphics/opengl_renderer/background/CanopyVol.h"

#include "gtest/gtest.h"

TEST(CanopyVol, CellsAverageTheirTexelsAndKeepOddEdges) {
  // 3 x 3 texels (r unused, g lowest, b highest, a volume): cells 2 x 2, the last row and
  // column of cells hold one or two texels only
  const u32 w = 3, h = 3;
  std::vector<u16> t(w * h * 4, 0);
  for (u32 y = 0; y < h; y++) {
    for (u32 x = 0; x < w; x++) {
      u16* p = &t[(y * w + x) * 4];
      p[1] = (u16)(100 * (y * w + x));
      p[2] = 60000;
      p[3] = x == 2 ? 50000 : 10000;
    }
  }
  CanopyVolMap m;
  canopy_vol_cells(t.data(), w, h, &m);
  ASSERT_EQ(m.cw, 2u);
  ASSERT_EQ(m.ch, 2u);
  auto cell = [&](u32 cx, u32 cy, int c) { return m.cells[(cy * m.cw + cx) * 4 + c]; };
  EXPECT_EQ(cell(0, 0, 0), (0 + 100 + 300 + 400) / 4);  // texels (0,0) (1,0) (0,1) (1,1)
  EXPECT_EQ(cell(1, 0, 0), (200 + 500) / 2);            // the last column: (2,0) (2,1)
  EXPECT_EQ(cell(1, 1, 0), 800);                        // the corner texel alone
  EXPECT_EQ(cell(1, 1, 2), 50000);                      // volume of the last column kept
  EXPECT_EQ(cell(0, 1, 2), 10000);
  EXPECT_EQ(cell(0, 0, 1), 60000);
  EXPECT_EQ(cell(0, 0, 3), 0);
}
