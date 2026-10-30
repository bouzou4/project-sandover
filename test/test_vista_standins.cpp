// Stand-in hiding in vista neighbour mode: only stand-ins that sit on their depicted level's
// geometry are hidden; the village1 forest canopy depicts no level and always draws.

#include <algorithm>
#include <string>
#include <vector>

#include "game/graphics/opengl_renderer/Neighbours.h"
#include "gtest/gtest.h"

namespace {
bool has(const std::string& list, const std::string& name) {
  size_t pos = 0;
  while (pos <= list.size()) {
    size_t end = list.find(',', pos);
    if (end == std::string::npos) {
      end = list.size();
    }
    if (list.compare(pos, end - pos, name) == 0) {
      return true;
    }
    pos = end + 1;
  }
  return false;
}
}  // namespace

TEST(VistaStandins, HidesStandinsOnTheirLevel) {
  auto f = neighbours::standin_filter({"village1", "jungle"},
                                      {"firecanyon", "training", "beach", "misty", "village2",
                                       "rolling"});
  EXPECT_TRUE(has(f, "v1-lores-vil2-canvas")) << f;
  EXPECT_TRUE(has(f, "v1-lores-vil2-wall")) << f;
  EXPECT_TRUE(has(f, "v1-lores-rolling-precursor")) << f;
}

TEST(VistaStandins, NeverHidesTheForestCanopy) {
  for (const auto& drawn : std::vector<std::vector<std::string>>{
           {"firecanyon"},
           {"firecanyon", "village2", "rolling", "beach", "training", "misty"}}) {
    auto f = neighbours::standin_filter({"village1", "jungle"}, drawn);
    EXPECT_FALSE(has(f, "v1-lores-trees")) << f;
  }
}

TEST(VistaStandins, NothingHiddenWhileDepictedLevelsAreMissing) {
  EXPECT_EQ(neighbours::standin_filter({"village1"}, {}), "");
}

TEST(VistaStandins, ResidencyFollowsTheSameRule) {
  auto levels = neighbours::standin_levels({"village1", "jungle"}, {});
  auto has_level = [&](const char* l) {
    return std::find(levels.begin(), levels.end(), l) != levels.end();
  };
  EXPECT_TRUE(has_level("village2"));
  EXPECT_TRUE(has_level("rolling"));
  EXPECT_FALSE(has_level("firecanyon"));
}
