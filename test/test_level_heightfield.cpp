#include <algorithm>
#include <cmath>
#include <limits>

#include "common/custom_data/Tfrag3Data.h"

#include "game/graphics/opengl_renderer/LevelHeightfield.h"

#include "gtest/gtest.h"

namespace {
void tri(tfrag3::CollisionMesh& m, float a[3], float b[3], float c[3]) {
  for (float* p : {a, b, c}) {
    tfrag3::CollisionMesh::Vertex v{};
    v.x = p[0] * 4096.f;
    v.y = p[1] * 4096.f;
    v.z = p[2] * 4096.f;
    m.vertices.push_back(v);
  }
}

void quad(tfrag3::CollisionMesh& m, float x0, float z0, float x1, float z1, float y) {
  float a[3] = {x0, y, z0}, b[3] = {x1, y, z0}, c[3] = {x1, y, z1}, d[3] = {x0, y, z1};
  tri(m, a, b, c);
  tri(m, a, c, d);
}

// ground at 0 over 0..20 m, a roof at 6 m over 5..10 m, a wall at x = 15 rising to 4 m
tfrag3::CollisionMesh scene() {
  tfrag3::CollisionMesh m;
  quad(m, 0, 0, 20, 20, 0.f);
  quad(m, 5, 5, 10, 10, 6.f);
  float a[3] = {15, 0, 2}, b[3] = {15, 0, 18}, c[3] = {15, 4, 18}, d[3] = {15, 4, 2};
  tri(m, a, b, c);
  tri(m, a, c, d);
  return m;
}
}  // namespace

TEST(LevelHeightfield, TopAndBottomLayers) {
  auto h = heightfield_build(scene(), "test");
  ASSERT_TRUE(h);
  EXPECT_EQ(h->texel, 1.f);
  EXPECT_FLOAT_EQ(h->height_top(2.5f, 2.5f), 0.f);    // open ground
  EXPECT_FLOAT_EQ(h->height_top(7.5f, 7.5f), 6.f);    // under the roof: the roof
  EXPECT_FLOAT_EQ(h->height_bottom(7.5f, 7.5f), 0.f);  // ... and the ground below it
  EXPECT_FLOAT_EQ(h->height_top(15.2f, 10.f), 4.f);   // the wall's top edge
  EXPECT_TRUE(std::isinf(h->height_top(-50.f, 0.f)));  // outside: open
  EXPECT_TRUE(std::isinf(h->height_bottom(-50.f, 0.f)));
}

TEST(LevelHeightfield, OpenSpanUnderRoofs) {
  auto h = heightfield_build(scene(), "test");
  ASSERT_TRUE(h);
  EXPECT_FLOAT_EQ(h->solid_at(7.5f, 3.f, 7.5f, 0.25f), 0.f);   // under the roof: open
  EXPECT_FLOAT_EQ(h->solid_at(7.5f, 7.f, 7.5f, 0.25f), 0.f);   // above it
  EXPECT_FLOAT_EQ(h->solid_at(7.5f, -1.f, 7.5f, 0.25f), 1.f);  // in the ground
  EXPECT_FLOAT_EQ(h->solid_at(15.2f, 2.f, 10.f, 0.25f), 1.f);  // in the wall
  EXPECT_FLOAT_EQ(h->solid_at(2.5f, 1.f, 2.5f, 0.25f), 0.f);   // over open ground
  EXPECT_FLOAT_EQ(h->solid_at(7.5f, 6.f, 7.5f, 0.25f), 1.f);   // a roof with no thickness
  EXPECT_FLOAT_EQ(h->solid_span(7.5f, 7.5f, 5.f, 6.5f, 0.25f), 1.f);  // a step through it
  EXPECT_FLOAT_EQ(h->solid_span(7.5f, 7.5f, 1.f, 5.f, 0.25f), 0.f);   // below it
}

TEST(LevelHeightfield, SlabRange) {
  HeightfieldSpec spec;
  spec.y_max = 3.f;  // a slab below the roof: the floor only
  auto h = heightfield_build(scene(), "test", spec);
  ASSERT_TRUE(h);
  EXPECT_FLOAT_EQ(h->height_top(7.5f, 7.5f), 0.f);
  EXPECT_LE(h->height_top(15.2f, 10.f), 3.f);
}

TEST(LevelHeightfield, SkyLevelsFromLevelInfo) {
  EXPECT_TRUE(heightfield_level_has_sky("village1"));
  EXPECT_TRUE(heightfield_level_has_sky("jungle"));
  EXPECT_FALSE(heightfield_level_has_sky("robocave"));
  EXPECT_FALSE(heightfield_level_has_sky("jungleb"));
  EXPECT_FALSE(heightfield_level_has_sky("not-a-level"));
}

TEST(LevelHeightfield, MarkerEncoding) {
  // the lowest real top and the highest real bottom must not read as "none"
  EXPECT_EQ(heightfield_encode_top(0.f, 0.f, 10.f), 1);
  EXPECT_EQ(heightfield_encode_top(10.f, 0.f, 10.f), 65534);
  EXPECT_EQ(heightfield_encode_bottom(10.f, 0.f, 10.f), 65534);
  EXPECT_EQ(heightfield_encode_bottom(0.f, 0.f, 10.f), 1);
  EXPECT_EQ(heightfield_encode_top(-std::numeric_limits<float>::infinity(), 0.f, 10.f),
            kHeightfieldNoTop);
  EXPECT_EQ(heightfield_encode_bottom(std::numeric_limits<float>::infinity(), 0.f, 10.f),
            kHeightfieldNoBottom);
  EXPECT_NEAR(heightfield_decode(heightfield_encode_top(3.7f, 0.f, 10.f), 0.f, 10.f), 3.7f, 1e-3);
}

TEST(LevelHeightfield, GpuMipsKeepMaxTopMinBottomAndMarkers) {
  auto h = heightfield_build(scene(), "test");
  ASSERT_TRUE(h);
  ASSERT_GE(h->gpu_mips.size(), 2u);
  const auto& m0 = h->gpu_mips[0];
  const u32 W = h->width;
  // a texel beyond the collision (the padding column) holds both markers
  bool found_empty = false;
  for (size_t k = 0; k < (size_t)W * h->height; k++) {
    if (std::isinf(h->top[k])) {
      EXPECT_EQ(m0[2 * k], kHeightfieldNoTop);
      EXPECT_EQ(m0[2 * k + 1], kHeightfieldNoBottom);
      found_empty = true;
    } else {
      EXPECT_GE(m0[2 * k], 1);
      EXPECT_LE(m0[2 * k + 1], 65534);
    }
  }
  EXPECT_TRUE(found_empty);
  // every level: each cell holds the max top / min bottom of the texels i >> k maps to it
  for (size_t l = 1; l < h->gpu_mips.size(); l++) {
    const auto& cur = h->gpu_mips[l];
    const int nw = heightfield_mip_dim(W, (int)l), nh = heightfield_mip_dim(h->height, (int)l);
    ASSERT_EQ(cur.size(), (size_t)nw * nh * 2);
    std::vector<u16> t((size_t)nw * nh, kHeightfieldNoTop);
    std::vector<u16> b((size_t)nw * nh, kHeightfieldNoBottom);
    for (u32 j = 0; j < h->height; j++) {
      for (u32 i = 0; i < W; i++) {
        const size_t c = (size_t)(j >> l) * nw + (i >> l), s0 = ((size_t)j * W + i) * 2;
        t[c] = std::max(t[c], m0[s0]);
        b[c] = std::min(b[c], m0[s0 + 1]);
      }
    }
    for (size_t c = 0; c < t.size(); c++) {
      EXPECT_EQ(cur[2 * c], t[c]);
      EXPECT_EQ(cur[2 * c + 1], b[c]);
    }
  }
  // the roof (6 m) survives to the 1x1 level as the top, the ground (0 m) as the bottom
  const auto& last = h->gpu_mips.back();
  EXPECT_NEAR(heightfield_decode(last[0], h->y0, h->y1), 6.f, 1e-3);
  EXPECT_NEAR(heightfield_decode(last[1], h->y0, h->y1), 0.f, 1e-3);
}

TEST(LevelHeightfield, OddWidthKeepsTheLastColumnAtEveryLevel) {
  for (u32 W : {1001u, 1002u, 1023u, 37u}) {
    LevelHeightfield m;
    m.width = W;
    m.height = 5;
    m.y0 = 0.f;
    m.y1 = 10.f;
    m.top.assign((size_t)W * 5, 0.f);
    m.bottom.assign((size_t)W * 5, 0.f);
    m.top[(size_t)2 * W + W - 1] = 8.f;  // a roof in the last column only
    heightfield_build_gpu_mips(m);
    ASSERT_EQ((int)m.gpu_mips.size(), kHeightfieldLevels);
    const int A = heightfield_alloc_dim(W);
    EXPECT_EQ(A % (1 << (kHeightfieldLevels - 1)), 0);
    for (int k = 0; k < kHeightfieldLevels; k++) {
      // the GL level k of the allocation (A >> k) holds every cell of the map's mip k
      EXPECT_GE(A >> k, heightfield_mip_dim(W, k));
      // the shader's cell for the roof texel: (index) >> k, at the row of the roof
      const int mw = heightfield_mip_dim(W, k);
      const size_t c = (size_t)(2 >> k) * mw + ((W - 1) >> k);
      EXPECT_NEAR(heightfield_decode(m.gpu_mips[k][2 * c], m.y0, m.y1), 8.f, 1e-3)
          << "width " << W << " level " << k;
    }
  }
}
