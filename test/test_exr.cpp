// Round trip of the minimal EXR writer through its reader on a synthetic HDR image.

#include <cmath>
#include <filesystem>
#include <vector>

#include "common/util/exr.h"

#include "gtest/gtest.h"

TEST(Exr, HalfConversion) {
  EXPECT_EQ(exr::float_to_half(0.f), 0x0000);
  EXPECT_EQ(exr::float_to_half(-0.f), 0x8000);
  EXPECT_EQ(exr::float_to_half(1.f), 0x3c00);
  EXPECT_EQ(exr::float_to_half(2.f), 0x4000);
  EXPECT_EQ(exr::float_to_half(65504.f), 0x7bff);
  EXPECT_EQ(exr::float_to_half(1e6f), 0x7c00);
  EXPECT_EQ(exr::float_to_half(std::pow(2.f, -24.f)), 0x0001);
  EXPECT_EQ(exr::float_to_half(1.f + std::pow(2.f, -11.f)), 0x3c00);  // tie rounds to even
  EXPECT_TRUE(std::isnan(exr::half_to_float(exr::float_to_half(NAN))));
  for (u32 h = 0; h < 0x7c00; h++) {
    ASSERT_EQ(exr::float_to_half(exr::half_to_float((u16)h)), h);
  }
}

TEST(Exr, RoundTrip) {
  const int w = 37, h = 11;
  std::vector<float> px((size_t)w * h * 4);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      float* p = &px[((size_t)y * w + x) * 4];
      p[0] = x * 0.1f;
      p[1] = y * 0.37f;
      p[2] = (x + y) * 0.01f - 0.05f;
      p[3] = 123.f;
    }
  }
  const auto path = (std::filesystem::temp_directory_path() / "sandover_exr_test.exr").string();
  ASSERT_TRUE(exr::write_rgb_half(path, px.data(), w, h, 4, true, {{"tonemapMode", "1"}}));
  exr::Image img;
  ASSERT_TRUE(exr::read_rgb_half(path, &img));
  std::filesystem::remove(path);
  ASSERT_EQ(img.w, w);
  ASSERT_EQ(img.h, h);
  EXPECT_EQ(img.attrs.at("tonemapMode"), "1");
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const float* src = &px[((size_t)(h - 1 - y) * w + x) * 4];
      for (int c = 0; c < 3; c++) {
        const float got = img.rgb[((size_t)y * w + x) * 3 + c];
        EXPECT_NEAR(got, src[c], std::abs(src[c]) * 1e-3f + 1e-4f) << x << " " << y << " " << c;
      }
    }
  }
}
