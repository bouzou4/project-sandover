// LightTracker world slots: a row present every frame shows after frames_in frames and is gone
// frames_out + fade_frames - 1 frames after it stops being listed.

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "game/graphics/opengl_renderer/LightTracker.h"
#include "game/graphics/opengl_renderer/WorldLights.h"
#include "gtest/gtest.h"

namespace {
LightTracker::World row(int r) {
  LightTracker::World w{};
  w.row = r;
  w.pos[0] = (float)r;
  w.power[0] = w.power[1] = w.power[2] = 1.f;
  w.radius = 1.f;
  return w;
}
}  // namespace

TEST(LightTracker, WorldRowShowsAndFades) {
  LightTracker t;
  LightTracker::Params p;
  p.frames_in = p.frames_out = 3;
  p.fade_frames = 8;
  const float cam[3] = {};
  const std::vector<LightTracker::Candidate> none;
  const std::vector<LightTracker::World> rows = {row(1), row(2)};
  for (int f = 1; f <= 10; f++) {
    t.update(none, rows, 0.01f, cam, p);
    ASSERT_EQ(t.world().size(), f < p.frames_in ? 0u : 2u) << "frame " << f;
    for (const auto& o : t.world()) {
      EXPECT_GT(o.power[0], 0.f);
      EXPECT_LE(o.power[0], 1.f);
    }
  }
  EXPECT_FLOAT_EQ(t.world()[0].power[0], 1.f);
  const std::vector<LightTracker::World> empty;
  for (int f = 1; f <= p.frames_out + p.fade_frames - 1; f++) {
    t.update(none, empty, 0.01f, cam, p);
    EXPECT_EQ(t.world().size(), f < p.frames_out + p.fade_frames - 1 ? 2u : 0u) << "miss " << f;
  }
}

TEST(LightTracker, WorldSlotsFreedUnderChurn) {
  LightTracker t;
  LightTracker::Params p;
  const float cam[3] = {};
  const std::vector<LightTracker::Candidate> none;
  for (int f = 0; f < 200; f++) {
    std::vector<LightTracker::World> rows;
    for (int i = 0; i < LightTracker::kWorldSlots; i++) {
      rows.push_back(row(1000 + f * LightTracker::kWorldSlots + i));
    }
    t.update(none, rows, 0.01f, cam, p);
  }
  const std::vector<LightTracker::World> one = {row(7)};
  for (int f = 0; f < p.frames_in; f++) {
    t.update(none, one, 0.01f, cam, p);
  }
  ASSERT_EQ(t.world().size(), 1u);
  EXPECT_EQ(t.world()[0].id, 7);
}

TEST(LightTracker, WorldSwapKeepsSteadyRowsWhenFull) {
  LightTracker t;
  LightTracker::Params p;
  const float cam[3] = {};
  const std::vector<LightTracker::Candidate> none;
  std::vector<LightTracker::World> rows;
  for (int i = 0; i < LightTracker::kWorldSlots; i++) {
    rows.push_back(row(100 + i));
  }
  for (int f = 0; f < p.frames_in + p.fade_frames; f++) {
    t.update(none, rows, 0.01f, cam, p);
  }
  ASSERT_EQ(t.world().size(), (size_t)LightTracker::kWorldSlots);
  // one row swapped for a new one, the new row listed first
  std::vector<LightTracker::World> swapped = {row(999)};
  swapped.insert(swapped.end(), rows.begin() + 1, rows.end());
  for (int f = 0; f < 12; f++) {
    t.update(none, swapped, 0.01f, cam, p);
    int steady = 0;
    for (const auto& o : t.world()) {
      if (o.id >= 101 && o.id < 100 + LightTracker::kWorldSlots) {
        EXPECT_FLOAT_EQ(o.power[0], 1.f) << "row " << o.id << " frame " << f;
        steady++;
      }
    }
    EXPECT_EQ(steady, LightTracker::kWorldSlots - 1) << "frame " << f;
  }
}

TEST(LightTracker, WorldOnlyFrameSnapsLightsAndHoldsTiles) {
  LightTracker t;
  LightTracker::Params p;
  p.frames_in = 1;
  // a tile step with no candidates would hide the slot at once and start its fade-out
  p.frames_out = 1;
  const float cam[3] = {};
  LightTracker::Candidate c{};
  c.pos[2] = 5.f;
  c.power[0] = c.power[1] = c.power[2] = 1.f;
  c.fill = 1.f;
  const std::vector<LightTracker::Candidate> one = {c};
  const std::vector<LightTracker::World> none;
  for (int f = 0; f < p.fade_frames; f++) {
    t.update(one, none, 0.01f, cam, p);
  }
  ASSERT_EQ(t.tiles().size(), 1u);
  const float tile_power = t.tiles()[0].power[0];
  LightTracker::World flash = row(5);
  flash.snap = true;
  flash.suppress = false;
  // a frame with no tile readback: the snap light is full at once, the tiles are held
  t.update({}, {flash}, 0.01f, cam, p, false);
  ASSERT_EQ(t.world().size(), 1u);
  EXPECT_FLOAT_EQ(t.world()[0].power[0], 1.f);
  ASSERT_EQ(t.tiles().size(), 1u);
  EXPECT_FLOAT_EQ(t.tiles()[0].power[0], tile_power);
  // the flash ends: still listed at zero power, dark that same frame; tiles still held
  flash.power[0] = flash.power[1] = flash.power[2] = 0.f;
  t.update({}, {flash}, 0.01f, cam, p, false);
  ASSERT_EQ(t.world().size(), 1u);
  EXPECT_FLOAT_EQ(t.world()[0].power[0], 0.f);
  ASSERT_EQ(t.tiles().size(), 1u);
  EXPECT_FLOAT_EQ(t.tiles()[0].power[0], tile_power);
  EXPECT_EQ(t.live_slots(), 1);
}

// world_lights::select glow props (pop-in fix): walking the camera toward a glowing mushroom patch
// or ice crystal, the props whose lit sphere is in view change power smoothly, and the drawn power
// (through the tracker's ramp) changes by at most Params::ramp a frame. The 8 nearest (fade 0)
// switch props on and off at full power.
namespace {
const float kCls[world_lights::NUM_CLASSES] = {0.08f, 0.01f, 0.005f, 0.05f, 0.12f, 0.3f};
constexpr float kTanHalf = 0.96f;  // corner of an 80 x 49 degree view

float luma3(const float* c) {
  return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
}

bool sphere_in_view(const float* pos, float r, const float* cam, const float* fwd) {
  float d = 0.f, c = 0.f;
  for (int i = 0; i < 3; i++) {
    d += (pos[i] - cam[i]) * (pos[i] - cam[i]);
  }
  d = std::sqrt(d);
  for (int i = 0; i < 3; i++) {
    c += fwd[i] * (pos[i] - cam[i]) / std::max(d, 1e-3f);
  }
  return std::acos(std::clamp(c, -1.f, 1.f)) - std::asin(std::min(r / d, 1.f)) -
             std::atan(kTanHalf) - 3.1416f * std::max(1.f - d / r, 0.f) <
         0.f;
}

struct Walk {
  float select_step = 0.f;  // largest per-step change of an in-view prop, over its full power
  float drawn_step = 0.f;   // the same after the tracker
  float target_far = 0.f;   // the target's drawn power 30 m out, over its full power
  float target_end = 0.f;   // and at the end (4 m)
};

using RowPow = std::vector<std::pair<int, float>>;
float find_pow(const RowPow& v, int row) {
  for (const auto& p : v) {
    if (p.first == row) {
      return p.second;
    }
  }
  return 0.f;
}

// camera from 60 m to 4 m from `target` in 0.25 m steps (one frame each), 2 m up, looking at it
Walk walk(const std::vector<world_lights::LevelWeights>& levels,
          const float* target,
          float full,
          float dir_rad,
          const world_lights::GlowView& base) {
  Walk w;
  LightTracker t;
  LightTracker::Params p;
  const std::vector<LightTracker::Candidate> none;
  RowPow prev_sel, prev_drawn;
  std::map<int, std::pair<std::array<float, 3>, float>> where;  // row: position, radius
  bool have_prev = false;
  for (float d = 60.f; d >= 4.f; d -= 0.25f) {
    const float cam[3] = {target[0] + d * std::cos(dir_rad), target[1] + 2.f,
                          target[2] + d * std::sin(dir_rad)};
    world_lights::GlowView g = base;
    g.reserved = 6;  // EmissiveLights: kGlowSpare + kEcoReserve
    float n = 0.f;
    for (int i = 0; i < 3; i++) {
      g.fwd[i] = target[i] - cam[i];
      n += g.fwd[i] * g.fwd[i];
    }
    for (float& f : g.fwd) {
      f /= std::sqrt(n);
    }
    const auto sel = world_lights::select(levels, cam, 16, kCls, 1.f, 8.f, g);
    RowPow cur_sel;
    std::vector<LightTracker::World> rows;
    int target_row = -1;
    for (const auto& s : sel) {
      if (!s.added || s.snap) {
        continue;
      }
      cur_sel.push_back({s.row, luma3(s.power)});
      where[s.row] = {{s.pos_m[0], s.pos_m[1], s.pos_m[2]}, s.radius_m};
      float e = 0.f;
      for (int i = 0; i < 3; i++) {
        e += (s.pos_m[i] - target[i]) * (s.pos_m[i] - target[i]);
      }
      if (e < 1e-4f) {
        target_row = s.row;
      }
      LightTracker::World r{};
      r.row = s.row;
      std::copy(s.pos_m, s.pos_m + 3, r.pos);
      std::copy(s.power, s.power + 3, r.power);
      r.radius = s.radius_m;
      r.suppress = false;
      r.ramp = base.fade_m > 0.f;
      rows.push_back(r);
    }
    t.update(none, rows, 0.01f, cam, p, false);
    RowPow cur_drawn;
    for (const auto& o : t.world()) {
      cur_drawn.push_back({o.id, luma3(o.power)});
    }
    if (have_prev) {
      // only lights whose lit sphere is in view: the others light nothing on screen
      for (const auto& [row, pr] : where) {
        if (!sphere_in_view(pr.first.data(), pr.second, cam, g.fwd)) {
          continue;
        }
        w.select_step = std::max(
            w.select_step, std::abs(find_pow(cur_sel, row) - find_pow(prev_sel, row)) / full);
        w.drawn_step = std::max(
            w.drawn_step, std::abs(find_pow(cur_drawn, row) - find_pow(prev_drawn, row)) / full);
      }
    }
    if (std::abs(d - 30.f) < 0.1f) {
      w.target_far = target_row >= 0 ? find_pow(cur_drawn, target_row) / full : 0.f;
    }
    w.target_end = target_row >= 0 ? find_pow(cur_drawn, target_row) / full : 0.f;
    prev_sel = cur_sel;
    prev_drawn = cur_drawn;
    have_prev = true;
  }
  return w;
}

std::vector<world_lights::LevelWeights> levels_of(std::initializer_list<const char*> names) {
  std::vector<world_lights::LevelWeights> out;
  for (const char* n : names) {
    world_lights::LevelWeights lw;
    lw.name = n;
    out.push_back(lw);
  }
  return out;
}
}  // namespace

TEST(WorldLightsGlow, ApproachIsSmoothAndNearestEightPops) {
  struct Target {
    std::vector<world_lights::LevelWeights> levels;
    float pos[3];
    float full;  // world_glow x gain x luma(colour)
  };
  // a cv-mushroom patch in maincave and a snow-crystals cluster (WorldLights.inc rows)
  const Target targets[] = {
      {levels_of({"maincave", "robocave"}),
       {954.655f, 36.349f, -2940.899f},
       0.3f * 0.8f * (0.299f * 0.023f + 0.587f * 1.f + 0.114f * 0.394f)},
      {levels_of({"snow"}),
       {660.911f, 229.029f, -3336.637f},
       0.3f * 1.f * (0.299f * 0.475f + 0.587f * 0.694f + 0.114f * 1.f)},
  };
  world_lights::GlowView fixed;
  fixed.tan_half = kTanHalf;
  fixed.fade_m = 70.f;
  world_lights::GlowView cheap = fixed;
  cheap.slots = 8;
  world_lights::GlowView legacy = fixed;
  legacy.fade_m = 0.f;
  int legacy_pops = 0, legacy_far_dark = 0, far_full = 0;
  for (const auto& t : targets) {
    for (int k = 0; k < 8; k++) {
      const float a = k * 0.785398f;
      const std::string lv = t.levels[0].name;
      const Walk wf = walk(t.levels, t.pos, t.full, a, fixed);
      far_full += wf.target_far > 0.9f;
      const Walk wc = walk(t.levels, t.pos, t.full, a, cheap);
      for (const Walk* w : {&wf, &wc}) {
        EXPECT_LT(w->drawn_step, 0.07f) << lv << " dir " << k;
        EXPECT_GT(w->target_end, 0.9f) << lv << " dir " << k;
      }
      const Walk wl = walk(t.levels, t.pos, t.full, a, legacy);
      legacy_pops += wl.select_step >= 0.99f;
      legacy_far_dark += wl.target_far < 0.1f;
    }
  }
  // with 8 glow slots most approaches have the target at full power 30 m out
  EXPECT_GE(far_full, 12) << far_full << " of 16";
  // the bug: with the 8 nearest at full power, props switch on as the camera walks in
  EXPECT_GT(legacy_pops, 0);
  EXPECT_GT(legacy_far_dark, 0);
}

TEST(WorldLightsGlow, DarkCaveUnchanged) {
  // no glow props there; the dark eco crystal and cave crystals keep their paths
  const auto levels = levels_of({"darkcave"});
  world_lights::GlowView fixed;
  fixed.fade_m = 70.f;
  for (float x = -40.f; x <= 40.f; x += 20.f) {
    const float cam[3] = {x, 0.f, x};
    const auto a = world_lights::select(levels, cam, 16, kCls, 1.f, 8.f, world_lights::GlowView());
    const auto b = world_lights::select(levels, cam, 16, kCls, 1.f, 8.f, fixed);
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); i++) {
      EXPECT_EQ(a[i].row, b[i].row);
      EXPECT_EQ(a[i].power[0], b[i].power[0]);
    }
  }
}

TEST(LightTracker, RampRowSlewsAndOthersDoNot) {
  LightTracker t;
  LightTracker::Params p;
  const float cam[3] = {};
  const std::vector<LightTracker::Candidate> none;
  auto r = row(3);
  r.ramp = true;
  for (int f = 0; f < 40; f++) {
    t.update(none, {r}, 0.01f, cam, p, false);
  }
  ASSERT_EQ(t.world().size(), 1u);
  EXPECT_NEAR(t.world()[0].power[0], 1.f, 1e-5f);
  r.power[0] = r.power[1] = r.power[2] = 0.2f;
  t.update(none, {r}, 0.01f, cam, p, false);
  EXPECT_NEAR(t.world()[0].power[0], 1.f - p.ramp, 1e-5f);
  auto s = row(3);
  s.power[0] = s.power[1] = s.power[2] = 0.2f;
  t.update(none, {s}, 0.01f, cam, p, false);
  EXPECT_NEAR(t.world()[0].power[0], 0.2f, 1e-5f);
}

namespace {
// every row select() lists goes to the tracker, baked ones included; returns how many times a
// drawn light lost more than 1/fade_frames of its full power in one frame (an eviction)
int walk_all(const std::vector<world_lights::LevelWeights>& levels,
             const float* target,
             float dir_rad,
             int spare,
             float fade_m = 70.f) {
  LightTracker t;
  LightTracker::Params p;
  const std::vector<LightTracker::Candidate> none;
  std::map<int, float> full, prev;
  int bad = 0;
  for (float d = 60.f; d >= 4.f; d -= 0.25f) {
    const float cam[3] = {target[0] + d * std::cos(dir_rad), target[1] + 2.f,
                          target[2] + d * std::sin(dir_rad)};
    world_lights::GlowView g;
    g.tan_half = kTanHalf;
    g.fade_m = fade_m;
    g.reserved = spare;
    float n = 0.f;
    for (int i = 0; i < 3; i++) {
      g.fwd[i] = target[i] - cam[i];
      n += g.fwd[i] * g.fwd[i];
    }
    for (float& f : g.fwd) {
      f /= std::sqrt(n);
    }
    std::vector<LightTracker::World> rows;
    for (const auto& s : world_lights::select(levels, cam, 16, kCls, 1.f, 8.f, g)) {
      LightTracker::World r{};
      r.row = s.row;
      std::copy(s.pos_m, s.pos_m + 3, r.pos);
      std::copy(s.power, s.power + 3, r.power);
      r.radius = s.radius_m;
      r.suppress = !s.added;
      r.snap = s.snap;
      r.ramp = s.added && !s.snap;
      rows.push_back(r);
    }
    t.update(none, rows, 0.01f, cam, p, false);
    std::map<int, float> cur;
    for (const auto& o : t.world()) {
      cur[o.id] = luma3(o.power);
      full[o.id] = std::max(full[o.id], cur[o.id]);
    }
    for (const auto& [row, v] : prev) {
      const float now = cur.count(row) ? cur[row] : 0.f;
      bad += v - now > full[row] / (float)p.fade_frames + 1e-5f;
    }
    prev = cur;
  }
  return bad;
}
}  // namespace

TEST(WorldLightsGlow, BakedLightsAreNeverEvicted) {
  const float mush[3] = {954.655f, 36.349f, -2940.899f};
  const float ice[3] = {660.911f, 229.029f, -3336.637f};
  const auto cave = levels_of({"maincave", "robocave"});
  const auto snow = levels_of({"snow"});
  int bad = 0, bad_no_room = 0;
  for (int k = 0; k < 8; k++) {
    const float a = k * 0.785398f;
    // kGlowSpare + kEcoReserve, no eco shot lit
    bad += walk_all(cave, mush, a, 6) + walk_all(snow, ice, a, 6);
    bad_no_room += walk_all(cave, mush, a, 0) + walk_all(snow, ice, a, 0);
  }
  EXPECT_EQ(bad, 0);
  // the check sees the evictions of the first version (glow props in every slot left)
  EXPECT_GT(bad_no_room, 0);
}

// Launch lights: a tile toward a slotted launch-placed emitter, at its depth band and hue, stands
// for it (the beach ledge eco: its tile sat on the lip 6 m in front of it, 77 m out)
namespace {
LightTracker::Candidate cand(float x, float z, float r, float g, float b) {
  LightTracker::Candidate c{};
  c.pos[0] = x;
  c.pos[2] = z;
  c.power[0] = r;
  c.power[1] = g;
  c.power[2] = b;
  c.fill = 1.f;
  return c;
}
std::vector<float> tile_x(const std::vector<LightTracker::Candidate>& cands,
                          const std::vector<LightTracker::Emitter>& emitters) {
  LightTracker t;
  LightTracker::Params p;
  p.frames_in = 1;
  p.emitters = &emitters;
  const float cam[3] = {};
  for (int f = 0; f < p.fade_frames + 2; f++) {
    t.update(cands, {}, 0.01f, cam, p);
  }
  std::vector<float> x;
  for (const auto& o : t.tiles()) {
    x.push_back(o.pos[0]);
  }
  return x;
}
}  // namespace

TEST(LightTracker, TileTowardLaunchedEmitterGivesNoLight) {
  const LightTracker::Emitter blue{{0.f, 0.f, 77.f}, 5.f, {0.42f, 0.47f, 1.f}};
  // displaced 6 m toward the camera, same hue: gone; 3 m beside it: kept
  const auto x = tile_x({cand(0.f, 71.f, 0.17f, 0.22f, 0.39f), cand(3.f, 71.f, 0.17f, 0.22f, 0.39f)},
                        {blue});
  ASSERT_EQ(x.size(), 1u);
  EXPECT_FLOAT_EQ(x[0], 3.f);
}

TEST(LightTracker, EmittersInLineKeepTheOtherTile) {
  const LightTracker::Emitter near_blue{{0.f, 0.f, 20.f}, 5.f, {0.42f, 0.47f, 1.f}};
  const LightTracker::Emitter far_blue{{0.f, 0.f, 40.f}, 5.f, {0.42f, 0.47f, 1.f}};
  // an orange lamp tile behind the eco, and one in front of a farther eco
  EXPECT_EQ(tile_x({cand(0.f, 40.f, 1.f, 0.6f, 0.2f)}, {near_blue}).size(), 1u);
  EXPECT_EQ(tile_x({cand(0.f, 20.f, 1.f, 0.6f, 0.2f)}, {far_blue}).size(), 1u);
  // a blue tile past the eco's reach, and an eco hidden behind a cliff whose own blue tile is
  // 20 m nearer: neither is the eco's tile
  EXPECT_EQ(tile_x({cand(0.f, 40.f, 0.4f, 0.45f, 1.f)}, {near_blue}).size(), 1u);
  EXPECT_EQ(tile_x({cand(0.f, 20.f, 0.4f, 0.45f, 1.f)}, {far_blue}).size(), 1u);
}

namespace {
constexpr u32 kLedgeEco = 9985;  // beach:eco-6, the ledge eco of the 2026-10-15 play capture
// beach rows gated alive or crate (WorldLights.inc)
constexpr u32 kBeachAids[] = {14465, 14467, 14474, 14466, 14304, 3090, 2285, 14483,
                              9985,  14232, 14516, 3096,  3093,  3094,  3092, 3095};
world_lights::GlowView view_neg_z() {
  world_lights::GlowView g;
  g.tan_half = kTanHalf;
  g.fwd[0] = g.fwd[1] = 0.f;
  g.fwd[2] = -1.f;
  return g;
}
const world_lights::Selected* launched_row(const std::vector<world_lights::Selected>& sel) {
  for (const auto& s : sel) {
    if (s.launched) {
      return &s;
    }
  }
  return nullptr;
}
bool queried(u32 aid) {
  const int n = world_lights::goal_begin(12);
  for (int i = 0; i < n; i++) {
    if ((u32)(world_lights::goal_query(i) & 0xffffffffu) == aid) {
      return true;
    }
  }
  return false;
}
}  // namespace

// an entity row lights while its entity launches a glow group, at the launch origin; a paused
// frame keeps the list; 30 frames after the last launch the GOAL query answers again
TEST(WorldLightsLaunch, RowFollowsLaunchesThenHandsBack) {
  world_lights::reload();
  const auto levels = levels_of({"beach"});
  const float origin[3] = {-53.75f, 31.9f, -445.4f};
  const float cam[3] = {-53.75f, 33.f, -435.f};
  const auto g = view_neg_z();
  auto sel = [&]() { return world_lights::select(levels, cam, 16, kCls, 1.f, 8.f, g); };
  EXPECT_EQ(launched_row(sel()), nullptr);
  EXPECT_TRUE(queried(kLedgeEco));
  world_lights::launch_push("group-eco-blue", origin, kLedgeEco, nullptr);
  world_lights::launch_end(false);
  {
    const auto s = sel();
    ASSERT_NE(launched_row(s), nullptr);
    EXPECT_FLOAT_EQ(launched_row(s)->pos_m[1], origin[1]);
  }
  EXPECT_EQ(world_lights::launch_lit().size(), 1u);
  EXPECT_TRUE(queried(kLedgeEco));  // still asked, so the hand-back is current
  world_lights::launch_end(true);   // paused: the list stays
  EXPECT_NE(launched_row(sel()), nullptr);
  auto census_lit = [&]() {
    for (const auto& x : sel()) {
      if (!x.launched && std::abs(x.pos_m[1] - 31.803f) < 1e-3f &&
          std::abs(x.pos_m[2] + 445.468f) < 1e-3f) {
        return true;
      }
    }
    return false;
  };
  auto goal_says_lit = [&]() {
    const int n = world_lights::goal_begin(12);
    for (int i = 0; i < n; i++) {
      if ((u32)(world_lights::goal_query(i) & 0xffffffffu) == kLedgeEco) {
        world_lights::goal_set(i, 1000);
      }
    }
  };
  for (int f = 0; f < 30; f++) {  // stopped launching: dark for the hold, whatever GOAL says
    world_lights::launch_end(false);
    goal_says_lit();
    EXPECT_EQ(launched_row(sel()), nullptr) << "frame " << f;
    EXPECT_FALSE(census_lit()) << "frame " << f;
  }
  world_lights::launch_end(false);
  goal_says_lit();  // e.g. an eco paused past its pause distance: the census row lights again
  EXPECT_TRUE(census_lit());
  world_lights::reload();
}

// smoke, debris and broken-machine groups never light their entity (citb-coil-broken spawns
// part-off every frame once complete); the pickup stream, a child carrying the eco's entity, does
TEST(WorldLightsLaunch, OnlyGlowGroupsLight) {
  world_lights::reload();
  const auto levels = levels_of({"beach"});
  const float origin[3] = {-53.75f, 31.9f, -445.4f};
  const float cam[3] = {-53.75f, 33.f, -435.f};
  world_lights::launch_push("group-citb-coil-off", origin, kLedgeEco, nullptr);
  world_lights::launch_push("group-eco-blue-collect", origin, 0, nullptr);  // no entity
  world_lights::launch_end(false);
  EXPECT_EQ(launched_row(world_lights::select(levels, cam, 16, kCls, 1.f, 8.f, view_neg_z())),
            nullptr);
  int calls = 0;
  EXPECT_EQ(world_lights::launch_count(&calls), 0);
  EXPECT_EQ(calls, 2);
  world_lights::launch_push("group-eco-blue-collect", origin, kLedgeEco, nullptr);
  world_lights::launch_end(false);
  EXPECT_NE(launched_row(world_lights::select(levels, cam, 16, kCls, 1.f, 8.f, view_neg_z())),
            nullptr);
  world_lights::reload();
}

// more launched emitters in view than slots: only slotted ones take over their screen tiles
TEST(WorldLightsLaunch, UnslottedEmittersKeepTheirTiles) {
  world_lights::reload();
  const auto levels = levels_of({"beach"});
  const float cam[3] = {0.f, 10.f, 0.f};
  int k = 0;
  for (u32 aid : kBeachAids) {
    const float pos[3] = {(float)(k % 8) * 4.f - 14.f, 10.f, -30.f - (float)(k / 8) * 20.f};
    world_lights::launch_push("group-eco-blue", pos, aid, nullptr);
    k++;
  }
  world_lights::launch_end(false);
  const auto sel = world_lights::select(levels, cam, 4, kCls, 1.f, 8.f, view_neg_z());
  int slotted = 0;
  for (const auto& s : sel) {
    slotted += s.launched;
  }
  const auto lit = world_lights::launch_lit();
  EXPECT_GT(slotted, 0);
  EXPECT_LT(slotted, (int)std::size(kBeachAids));
  EXPECT_EQ((int)lit.size(), slotted);
  for (const auto& e : lit) {
    bool in_out = false;
    for (const auto& s : sel) {
      in_out |= s.launched && s.pos_m[0] == e.pos_m[0] && s.pos_m[2] == e.pos_m[2];
    }
    EXPECT_TRUE(in_out);
  }
  world_lights::reload();
}

// a listed group's offset is in the launcher's frame (as vector-orient-by-quat!)
TEST(WorldLightsLaunch, GroupOffsetFollowsLauncher) {
  world_lights::reload();
  const auto levels = levels_of({"beach"});
  const float origin[3] = {-53.75f, 31.f, -445.4f};
  const float cam[3] = {-53.75f, 33.f, -435.f};
  // 90 degrees about z: the vent offset (0, 1, 0) points along -x
  const float q[4] = {0.f, 0.f, 0.70710678f, 0.70710678f};
  world_lights::launch_push("group-part-vent-blue-active", origin, kLedgeEco, q);
  world_lights::launch_end(false);
  const auto sel = world_lights::select(levels, cam, 16, kCls, 1.f, 8.f, view_neg_z());
  const auto* s = launched_row(sel);
  ASSERT_NE(s, nullptr);
  EXPECT_NEAR(s->pos_m[0], origin[0] - 1.f, 1e-4f);
  EXPECT_NEAR(s->pos_m[1], origin[1], 1e-4f);
  world_lights::reload();
}
