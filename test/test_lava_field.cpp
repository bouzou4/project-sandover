// Lava field builder and bake share (LavaField.h).

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "common/custom_data/Tfrag3Data.h"
#include "common/util/FileUtil.h"
#include "common/util/Serializer.h"
#include "common/util/compress.h"

#include "game/graphics/opengl_renderer/LavaField.h"
#include "gtest/gtest.h"

namespace {
using namespace lava_field;

// two triangles of a horizontal quad at height y, wound to face up
void quad(std::vector<Tri>& t, float x0, float x1, float z0, float z1, float y) {
  t.push_back({{{x0, y, z0}, {x0, y, z1}, {x1, y, z1}}});
  t.push_back({{{x0, y, z0}, {x1, y, z1}, {x1, y, z0}}});
}

size_t at(const Field& f, float x, float z) {
  return (size_t)((z - f.z0) / f.texel_m) * f.w + (size_t)((x - f.x0) / f.texel_m);
}
}  // namespace

TEST(LavaField, CollisionAndVolumes) {
  std::vector<Tri> lava, ground;
  quad(lava, 0, 10, 0, 10, 2.f);
  // a volume beside it at y 1, with an island reaching the surface and a bed under it
  const Volume vol{"test", 1, "test-lava", 20.f, 30.f, 0.f, 10.f, 1.f};
  quad(ground, 22, 24, 2, 4, 1.5f);
  quad(ground, 15, 35, -5, 15, -3.f);
  // cover: the island and the bed, plus a walkway 3 m over the first lava
  std::vector<Tri> cover = ground;
  quad(cover, 2, 4, 0, 10, 5.f);
  Field f = build(lava, ground, cover, {vol}, 1.f, 4.f, 1024);
  ASSERT_EQ(f.w, 38);
  ASSERT_EQ(f.h, 18);
  EXPECT_FLOAT_EQ(f.texel_m, 1.f);
  EXPECT_FLOAT_EQ(f.y0, 1.f);
  EXPECT_NEAR(f.collide_m2, 100.f, 25.f);  // texels under the edge vertices count too
  // the island's 4 texels plus the 3 more its corner vertices fall in (conservative)
  EXPECT_EQ(f.dry_texels, 7);
  EXPECT_FLOAT_EQ(f.volume_m2, 93.f);
  EXPECT_FLOAT_EQ(f.cov[at(f, 5.5f, 5.5f)], 1.f);
  EXPECT_FLOAT_EQ(f.covh[at(f, 5.5f, 5.5f)], 1.f);  // lava at 2, origin 1
  EXPECT_FLOAT_EQ(f.cov[at(f, 25.5f, 7.5f)], 1.f);
  EXPECT_FLOAT_EQ(f.covh[at(f, 25.5f, 7.5f)], 0.f);
  EXPECT_FLOAT_EQ(f.cov[at(f, 22.5f, 2.5f)], 0.f);  // the island
  EXPECT_FLOAT_EQ(f.cov[at(f, 15.5f, 5.5f)], 0.f);  // between them
  EXPECT_FLOAT_EQ(f.roof[at(f, 3.5f, 5.5f)], 5.f);    // under the walkway
  EXPECT_TRUE(std::isinf(f.roof[at(f, 7.5f, 5.5f)]));  // open sky
  EXPECT_FLOAT_EQ(f.near_m[at(f, 12.5f, 5.5f)], 2.f);      // beside the first lava
  EXPECT_TRUE(std::isinf(f.roof[at(f, 22.5f, 2.5f)]));  // island top only 0.5 m over the lava
}

TEST(LavaField, TexelGrowsToFit) {
  std::vector<Tri> lava;
  quad(lava, 0, 3000, 0, 100, 0.f);
  Field f = build(lava, {}, {}, {}, 1.f, 0.f, 1024);
  EXPECT_LE(f.w, 1024);
  EXPECT_NEAR(f.texel_m, 3000.f / 1024.f, 1e-3f);
  EXPECT_TRUE(build({}, {}, {}, {}, 1.f, 4.f, 1024).empty());
}

TEST(LavaField, VolumesFromTheGame) {
  EXPECT_EQ(volumes_of("ogre").size(), 1u);
  EXPECT_EQ(volumes_of("village3").size(), 1u);
  EXPECT_EQ(volumes_of("lavatube").size(), 1u);
  EXPECT_TRUE(volumes_of("firecanyon").empty());
  EXPECT_NEAR(volumes_of("ogre")[0].surface_m, 29.f, 0.01f);
}

TEST(LavaField, BakeShareCountsLavaHuedSlots) {
  tfrag3::Level lev;
  lev.tfrag_trees[0].emplace_back();
  auto& c = lev.tfrag_trees[0][0].colors;
  c.color_count = 4;
  c.data.assign(128, 0);
  auto set = [&](int pal, int r, int g, int b) {
    for (int col = 0; col < 4; col++) {
      c.data[pal * 16 + col * 4 + 0] = r;
      c.data[pal * 16 + col * 4 + 1] = g;
      c.data[pal * 16 + col * 4 + 2] = b;
    }
  };
  for (int s = 0; s < 4; s++) {
    set(s, 100, 100, 100);
  }
  EXPECT_FLOAT_EQ(bake_lava_share(lev), 0.f);
  set(4, 200, 200, 200);  // lightning: white, not lava
  EXPECT_FLOAT_EQ(bake_lava_share(lev), 0.f);
  set(5, 120, 60, 0);  // lava: 180 against a mean sun slot of 300
  EXPECT_NEAR(bake_lava_share(lev), 180.f / 480.f, 1e-4f);
}

// The real levels, when their fr3 files are around ($LAVA_FIELD_FR3, else out/jak1/fr3 of the
// project): what the renderer will build at load.
TEST(LavaField, RealLevels) {
  std::filesystem::path dir;
  if (const char* p = std::getenv("LAVA_FIELD_FR3")) {
    dir = p;
  } else {
    dir = file_util::get_jak_project_dir() / "out" / "jak1" / "fr3";
  }
  if (!std::filesystem::exists(dir / "ogre.fr3")) {
    GTEST_SKIP() << "no fr3 files in " << dir.string();
  }
  for (const char* name : {"firecanyon", "lavatube", "village3", "ogre", "village1"}) {
    auto data = file_util::read_binary_file((dir / fmt::format("{}.fr3", name)).string());
    auto raw = compression::decompress_zstd(data.data(), data.size());
    Serializer ser(raw.data(), raw.size());
    tfrag3::Level lev;
    lev.serialize(ser);
    const Field f = build_level(lev, name);
    const float share = bake_lava_share(lev);
    if (const char* dump = std::getenv("LAVA_FIELD_DUMP"); dump && !f.empty()) {
      // cov, near, roof as float32 rows, for offline predictions
      std::vector<u8> out;
      for (const auto* v : {&f.cov, &f.near_m, &f.roof}) {
        const auto* p = reinterpret_cast<const u8*>(v->data());
        out.insert(out.end(), p, p + v->size() * sizeof(float));
      }
      file_util::write_binary_file(fmt::format("{}/{}.field", dump, name), out.data(), out.size());
      file_util::write_text_file(
          fmt::format("{}/{}.json", dump, name),
          fmt::format("{{\"w\": {}, \"h\": {}, \"x0\": {}, \"z0\": {}, \"texel\": {}, "
                      "\"y0\": {}, \"share\": {}}}",
                      f.w, f.h, f.x0, f.z0, f.texel_m, f.y0, share));
    }
    fmt::print("{}: {}x{} at {:.2f} m, collision lava {:.0f} m2, volumes {:.0f} m2 ({} dry), "
               "lava {:.1f}..{:.1f} m, roofed {:.0f} m2, lava at two heights {:.0f} m2, "
               "bake lava share {:.3f}\n",
               name, f.w, f.h, f.texel_m, f.collide_m2, f.volume_m2, f.dry_texels, f.y0,
               f.y0 + [&] {
                 float m = 0.f;
                 for (size_t k = 0; k < f.cov.size(); k++) {
                   m = f.cov[k] > 0.f ? std::max(m, f.covh[k]) : m;
                 }
                 return m;
               }(),
               f.roofed_m2, f.collide_spread_m2, share);
    const std::string n = name;
    if (n == "village1") {
      EXPECT_TRUE(f.empty());
      EXPECT_LT(share, 0.05f);
    } else if (n == "firecanyon") {
      EXPECT_FALSE(f.empty());
      EXPECT_GE(share, 0.2f);  // the bake holds some lava light
    } else {
      EXPECT_FALSE(f.empty());
      EXPECT_LT(share, 0.05f) << name;
    }
    if (n == "ogre") {
      EXPECT_GT(f.volume_m2, 10000.f);  // the arena lake is the actor, not collision
    }
  }
}

// A level that goes unused for a while (the loader drops it from the in-use list after a few
// frames but keeps it loaded) and comes back with the same load keeps or rebuilds its field.
TEST(LavaField, InactiveThenActiveSameLoad) {
  // drawn: kept while the load is the same, dropped once another load replaces it
  EXPECT_TRUE(keep_entry(true, true, 100, 100));
  EXPECT_FALSE(keep_entry(true, false, 100, 101));
  // not drawn: kept for kKeepFrames, longer than the loader's 180-frame grace
  EXPECT_TRUE(keep_entry(false, false, 100, 100 + 180));
  EXPECT_TRUE(keep_entry(false, false, 100, 100 + kKeepFrames));
  EXPECT_FALSE(keep_entry(false, false, 100, 101 + kKeepFrames));

  // an upload never consumes the prepared data: a later upload gets the same texels
  std::vector<Tri> lava;
  quad(lava, 0, 10, 0, 10, 2.f);
  Prepared p;
  p.field = build(lava, {}, {}, {}, 1.f, 4.f, 1024);
  ASSERT_FALSE(p.field.empty());
  const auto cov = p.field.cov;
  const auto first = near_texels(p);
  const auto second = near_texels(p);
  ASSERT_FALSE(first.empty());
  EXPECT_EQ(first, second);
  EXPECT_EQ(p.field.cov, cov);
  EXPECT_EQ(first.size(), (size_t)p.field.w * p.field.h * 3);
  for (float v : first) {
    EXPECT_TRUE(std::isfinite(v));
  }
}
