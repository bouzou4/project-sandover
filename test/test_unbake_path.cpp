#include <fstream>
#include <random>

#include "game/graphics/opengl_renderer/background/Unbake.h"

#include "gtest/gtest.h"

namespace {
void touch(const fs::path& p) {
  fs::create_directories(p.parent_path());
  std::ofstream(p) << "x";
}
}  // namespace

TEST(UnbakePath, FallsBackFromTheFr3DirToTheLightingFolders) {
  const fs::path root = fs::temp_directory_path() /
                       ("sandover-test-unbake-path-" + std::to_string(std::random_device{}()));
  const fs::path fr3 = root / "proj" / "out" / "jak1" / "fr3";
  const fs::path proj_light = root / "proj" / "sandover" / "data" / "lighting";
  const fs::path env_data = root / "env";
  fs::create_directories(fr3);
  const std::string env = env_data.string();

  // a launcher install: sidecars only in the package folder, with or without SANDOVER_DATA
  EXPECT_TRUE(unbake_find(fr3, "beach", nullptr, root / "proj").empty());
  touch(proj_light / "beach.unbake");
  EXPECT_EQ(unbake_find(fr3, "beach", nullptr, root / "proj"), proj_light / "beach.unbake");
  EXPECT_EQ(unbake_find(fr3, "beach", "", root / "proj"), proj_light / "beach.unbake");
  EXPECT_EQ(unbake_find(fr3, "beach", env.c_str(), root / "proj"), proj_light / "beach.unbake");

  // SANDOVER_DATA beats the package, the copy next to the fr3 beats both
  touch(env_data / "lighting" / "beach.unbake");
  EXPECT_EQ(unbake_find(fr3, "beach", env.c_str(), root / "proj"),
            env_data / "lighting" / "beach.unbake");
  touch(fr3 / "beach.unbake");
  EXPECT_EQ(unbake_find(fr3, "beach", env.c_str(), root / "proj"), fr3 / "beach.unbake");
  EXPECT_TRUE(unbake_find(fr3, "jungle", env.c_str(), root / "proj").empty());

  fs::remove_all(root);
}
